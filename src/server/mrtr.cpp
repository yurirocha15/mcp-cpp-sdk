#include "mrtr.hpp"

#include "../detail/spawn_awaitable.hpp"

#include <mcp/auth/oauth.hpp>

#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <chrono>
#include <utility>

namespace mcp::detail {

namespace {

/// How often a retry waiting for its parked handler looks for its own cancellation, which it
/// passes on to the handler.
constexpr std::chrono::milliseconds g_cancel_poll_interval{50};

std::string_view key_prefix(std::string_view method) {
    if (method == "elicitation/create") {
        return "elicitation";
    }
    if (method == "sampling/createMessage") {
        return "sampling";
    }
    return "roots";
}

/// Wakes the request waiting for the entry. Posted, so that questions other coroutines of the
/// handler ask in the same turn of the strand join the round.
void schedule_park(const std::shared_ptr<MrtrEntry>& entry) {
    if (entry->park_scheduled) {
        return;
    }
    entry->park_scheduled = true;
    boost::asio::post(entry->strand, [entry] {
        entry->park_scheduled = false;
        if (entry->event == MrtrEntry::Event::ePending && !entry->requests.empty() &&
            !entry->abandoned) {
            entry->event = MrtrEntry::Event::eParked;
            entry->event_signal.cancel();
        }
    });
}

Task<nlohmann::json> request_input_on_strand(std::shared_ptr<MrtrEntry> entry, nlohmann::json method,
                                             std::optional<nlohmann::json> params) {
    if (entry->abandoned) {
        throw MrtrAbandoned{};
    }
    nlohmann::json request = {{"method", method}};
    if (params) {
        request["params"] = std::move(*params);
        // 2026-07-28 has no elicitation ids: an elicitation's response comes back with its key.
        if (method == "elicitation/create" && request["params"].is_object()) {
            request["params"].erase("elicitationId");
        }
    }
    const auto missing = missing_client_capabilities(entry->client_capabilities, request);
    if (!missing.empty()) {
        entry->missing_capabilities.merge_patch(missing);
        if (entry->event == MrtrEntry::Event::ePending) {
            entry->event = MrtrEntry::Event::eMissingCapability;
            entry->event_signal.cancel();
        }
        throw MrtrAbandoned{};
    }

    // [gcc11-sso: string-in-frame] The key stays a json value, which keeps its text on the heap.
    const nlohmann::json key = std::string(key_prefix(method.get_ref<const std::string&>())) + "-" +
                               std::to_string(++entry->next_key);
    for (;;) {
        entry->requests[key.get_ref<const std::string&>()] = request;
        schedule_park(entry);
        const auto round = entry->round;
        while (entry->round == round && !entry->abandoned) {
            boost::system::error_code ignored;
            co_await entry->resume_signal.async_wait(
                boost::asio::redirect_error(boost::asio::use_awaitable, ignored));
        }
        if (entry->abandoned) {
            throw MrtrAbandoned{};
        }
        // A response the retry left out is asked for again in the next round.
        const auto found = entry->responses.find(key.get_ref<const std::string&>());
        if (found != entry->responses.end()) {
            nlohmann::json response = std::move(*found);
            entry->responses.erase(key.get_ref<const std::string&>());
            co_return response;
        }
    }
}

Task<void> notify_on_strand(std::shared_ptr<MrtrEntry> entry,
                            std::shared_ptr<const std::string> message) {
    const MessageSender sink = entry->sink;
    if (!sink) {
        co_return;
    }
    // The handler's progress token is the first request's; the retry carrying it now may have
    // its own, or none.
    if (message->find("\"notifications/progress\"") != std::string::npos) {
        auto notification = nlohmann::json::parse(*message, nullptr, false);
        if (notification.is_object() && notification.value("method", "") == "notifications/progress" &&
            notification.contains("params") && notification["params"].is_object()) {
            if (!entry->progress_token) {
                co_return;
            }
            notification["params"]["progressToken"] = *entry->progress_token;
            message = std::make_shared<const std::string>(notification.dump());
        }
    }
    co_await sink(*message);
}

Task<MrtrEntry::Outcome> await_on_strand(std::shared_ptr<MrtrEntry> entry,
                                         std::shared_ptr<std::atomic<bool>> request_cancelled,
                                         bool forward_cancel) {
    bool forwarded = false;
    while (entry->event == MrtrEntry::Event::ePending) {
        const bool watching = forward_cancel && !forwarded && request_cancelled;
        if (watching && request_cancelled->load(std::memory_order_relaxed)) {
            entry->cancelled->store(true, std::memory_order_relaxed);
            forwarded = true;
            continue;
        }
        if (watching) {
            entry->event_signal.expires_after(g_cancel_poll_interval);
        } else {
            entry->event_signal.expires_at(std::chrono::steady_clock::time_point::max());
        }
        boost::system::error_code ignored;
        co_await entry->event_signal.async_wait(
            boost::asio::redirect_error(boost::asio::use_awaitable, ignored));
    }

    MrtrEntry::Outcome outcome;
    outcome.event = entry->event;
    if (entry->event == MrtrEntry::Event::eCompleted) {
        outcome.wire = std::move(entry->wire);
    } else if (entry->event == MrtrEntry::Event::eParked) {
        outcome.requests = nlohmann::json::object();
        for (const auto& [key, request] : entry->requests) {
            outcome.requests[key] = request;
        }
    } else {
        outcome.missing_capabilities = entry->missing_capabilities;
    }
    co_return outcome;
}

Task<void> resume_on_strand(std::shared_ptr<MrtrEntry> entry, nlohmann::json responses,
                            MessageSender sink, std::optional<nlohmann::json> progress_token,
                            nlohmann::json client_capabilities) {
    entry->responses = std::move(responses);
    entry->sink = std::move(sink);
    entry->progress_token = std::move(progress_token);
    entry->client_capabilities = std::move(client_capabilities);
    entry->requests.clear();
    entry->event = MrtrEntry::Event::ePending;
    ++entry->round;
    entry->resume_signal.cancel();
    co_return;
}

}  // namespace

MrtrEntry::MrtrEntry(const boost::asio::any_io_executor& executor,
                     std::shared_ptr<std::atomic<bool>> cancelled_flag,
                     std::shared_ptr<MrtrCounters> shared_counters)
    : strand(boost::asio::make_strand(executor)),
      cancelled(std::move(cancelled_flag)),
      counters(std::move(shared_counters)),
      event_signal(strand),
      resume_signal(strand) {
    // Never changed again: changing it would cancel the waits of every request for input.
    resume_signal.expires_at(std::chrono::steady_clock::time_point::max());
}

nlohmann::json missing_client_capabilities(const nlohmann::json& client_capabilities,
                                           const nlohmann::json& request) {
    const auto& method = request.at("method").get_ref<const std::string&>();
    const auto params = request.contains("params") && request["params"].is_object()
                            ? request["params"]
                            : nlohmann::json::object();
    const auto declared = [&client_capabilities](std::string_view name) -> const nlohmann::json* {
        const auto found = client_capabilities.find(std::string(name));
        return found != client_capabilities.end() && found->is_object() ? &*found : nullptr;
    };

    if (method == "elicitation/create") {
        const std::string mode = params.contains("mode") && params["mode"].is_string()
                                     ? params["mode"].get<std::string>()
                                     : "form";
        const auto* elicitation = declared("elicitation");
        // An empty elicitation capability declares form mode only.
        const bool supported = elicitation != nullptr && (elicitation->contains(mode) ||
                                                          (mode == "form" && elicitation->empty()));
        if (!supported) {
            return {{"elicitation", {{mode, nlohmann::json::object()}}}};
        }
    } else if (method == "sampling/createMessage") {
        const auto* sampling = declared("sampling");
        if (sampling == nullptr) {
            return {{"sampling", nlohmann::json::object()}};
        }
        const auto tools = params.find("tools");
        if (tools != params.end() && tools->is_array() && !tools->empty() &&
            !sampling->contains("tools")) {
            return {{"sampling", {{"tools", nlohmann::json::object()}}}};
        }
    } else if (method == "roots/list") {
        if (declared("roots") == nullptr) {
            return {{"roots", nlohmann::json::object()}};
        }
    }
    return nlohmann::json::object();
}

Task<nlohmann::json> mrtr_request_input(std::shared_ptr<MrtrEntry> entry, nlohmann::json method,
                                        std::optional<nlohmann::json> params) {
    auto strand = entry->strand;
    co_return co_await spawn_awaitable(
        strand, request_input_on_strand(std::move(entry), std::move(method), std::move(params)));
}

Task<void> mrtr_notify(std::shared_ptr<MrtrEntry> entry, std::shared_ptr<const std::string> message) {
    auto strand = entry->strand;
    co_await spawn_awaitable(strand, notify_on_strand(std::move(entry), std::move(message)));
}

void mrtr_complete(const std::shared_ptr<MrtrEntry>& entry, std::string wire) {
    if (entry->abandoned) {
        entry->counters->abandoned_completed.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (entry->event != MrtrEntry::Event::ePending) {
        return;
    }
    entry->event = MrtrEntry::Event::eCompleted;
    entry->wire = std::move(wire);
    entry->event_signal.cancel();
}

Task<MrtrEntry::Outcome> mrtr_await(std::shared_ptr<MrtrEntry> entry,
                                    std::shared_ptr<std::atomic<bool>> request_cancelled,
                                    bool forward_cancel) {
    auto strand = entry->strand;
    co_return co_await spawn_awaitable(
        strand, await_on_strand(std::move(entry), std::move(request_cancelled), forward_cancel));
}

Task<void> mrtr_resume(std::shared_ptr<MrtrEntry> entry, nlohmann::json responses, MessageSender sink,
                       std::optional<nlohmann::json> progress_token,
                       nlohmann::json client_capabilities) {
    auto strand = entry->strand;
    co_await spawn_awaitable(
        strand, resume_on_strand(std::move(entry), std::move(responses), std::move(sink),
                                 std::move(progress_token), std::move(client_capabilities)));
}

void mrtr_abandon(const std::shared_ptr<MrtrEntry>& entry) {
    boost::asio::post(entry->strand, [entry] {
        entry->abandoned = true;
        entry->cancelled->store(true, std::memory_order_relaxed);
        entry->resume_signal.cancel();
    });
}

std::string mrtr_params_digest(const nlohmann::json& params) {
    nlohmann::json salient = params.is_object() ? params : nlohmann::json::object();
    salient.erase("_meta");
    salient.erase("inputResponses");
    salient.erase("requestState");
    const auto digest = auth::detail::sha256(salient.dump());
    return {reinterpret_cast<const char*>(digest.data()), digest.size()};
}

}  // namespace mcp::detail

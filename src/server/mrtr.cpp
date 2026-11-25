#include "mrtr.hpp"

#include "mcp/detail/async/spawn_awaitable.hpp"
#include "mcp/detail/constants.hpp"
#include "mcp/detail/protocol/request_meta.hpp"
#include "mcp/detail/text/encoding.hpp"

#include <mcp/core/async.hpp>
#include <mcp/core/constants.hpp>
#include <mcp/detail/json_dump.hpp>

#include <algorithm>
#include <array>
#include <boost/asio/async_result.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <chrono>
#include <utility>

namespace methods = mcp::constants::methods;

namespace mcp::detail {

namespace {

std::string_view key_prefix(std::string_view method) {
    if (method == methods::g_ELICITATION_CREATE) {
        return "elicitation";
    }
    if (method == methods::g_SAMPLING_CREATE_MESSAGE) {
        return "sampling";
    }
    return "roots";
}

/// Whether @p key is one of the generated names: a kind prefix, a dash and a number.
bool is_generated_key(std::string_view key) {
    constexpr std::array<std::string_view, 3> prefixes{"elicitation-", "sampling-", "roots-"};
    return std::ranges::any_of(prefixes, [key](std::string_view prefix) {
        return key.size() > prefix.size() && key.starts_with(prefix) &&
               key.find_first_not_of("0123456789", prefix.size()) == std::string_view::npos;
    });
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

/// Clears the handlers held in an entry when it is destroyed. A posted operation that carries it
/// releases them whether it runs or is destroyed unrun, as asio does with the operations still
/// queued when the io_context shuts down.
struct HeldReleaser {
    explicit HeldReleaser(std::shared_ptr<MrtrEntry> value) : entry(std::move(value)) {}
    HeldReleaser(HeldReleaser&&) noexcept = default;
    HeldReleaser& operator=(HeldReleaser&&) = delete;
    HeldReleaser(const HeldReleaser&) = delete;
    HeldReleaser& operator=(const HeldReleaser&) = delete;
    ~HeldReleaser() {
        if (entry) {
            entry->held.clear();
        }
    }
    std::shared_ptr<MrtrEntry> entry;
};

/// Destroys the handlers held in @p entry, which unwinds the coroutines waiting on them.
void release_held(const std::shared_ptr<MrtrEntry>& entry) {
    boost::asio::post(entry->strand, [releaser = HeldReleaser(entry)] {});
}

/// Suspends the calling handler for good: its completion handler is kept in the entry, and
/// releasing the entry destroys it without invoking it, which destroys the suspended coroutines
/// without running any more of their code.
Task<void> hold_until_released(std::shared_ptr<MrtrEntry> entry) {
    auto hold = [raw = entry.get()](auto handler) { raw->hold(std::move(handler)); };
    co_await boost::asio::async_initiate<decltype(boost::asio::use_awaitable)&,
                                         void(boost::system::error_code)>(std::move(hold),
                                                                          boost::asio::use_awaitable);
    // Not reached: the handler is destroyed, never invoked.
}

Task<nlohmann::json> request_input_on_strand(std::shared_ptr<MrtrEntry> entry, nlohmann::json method,
                                             std::optional<nlohmann::json> params,
                                             nlohmann::json chosen_key) {
    if (entry->abandoned) {
        if (entry->sealed) {
            release_held(entry);
            co_await hold_until_released(entry);
        }
        throw MrtrAbandoned{};
    }
    nlohmann::json request = {{"method", method}};
    if (params) {
        request["params"] = std::move(*params);
        // 2026-07-28 has no elicitation ids: an elicitation's response comes back with its key.
        if (method_is(method, methods::g_ELICITATION_CREATE) && request["params"].is_object()) {
            request["params"].erase("elicitationId");
        }
    }
    const bool named = chosen_key.is_string();
    if (named) {
        const auto& text = chosen_key.get_ref<const std::string&>();
        if (text.empty()) {
            throw std::invalid_argument("the key of a question for the client must not be empty");
        }
        if (is_generated_key(text)) {
            throw std::invalid_argument("the key '" + text +
                                        "' is reserved for the names the SDK generates");
        }
        if (!entry->named_keys.insert(text).second) {
            throw std::invalid_argument("the key '" + text +
                                        "' already names a question of this request");
        }
    }
    // [gcc11-sso: string-in-frame] The key stays a json value, which keeps its text on the heap.
    // A generated name counts the questions that use one, and the counter moves on once the
    // question is taken, so a refused one does not use up a key.
    const nlohmann::json key =
        named ? std::move(chosen_key)
              : nlohmann::json(std::string(key_prefix(method.get_ref<const std::string&>())) + "-" +
                               std::to_string(entry->next_key + 1));
    if (const auto replayed = entry->replay.find(key.get_ref<const std::string&>());
        replayed != entry->replay.end() && replayed->at("m") == method) {
        // An earlier round answered this question: the handler goes on without asking again.
        nlohmann::json response = std::move(replayed->at("r"));
        const nlohmann::json record = {{"m", method}, {"r", response}};
        entry->replay.erase(replayed);
        if (!named) {
            ++entry->next_key;
        }
        entry->answers[key.get_ref<const std::string&>()] = record;
        if (entry->replay.empty()) {
            entry->replaying->store(false, std::memory_order_relaxed);
        }
        co_return response;
    }
    // A question the earlier runs did not answer ends the replay: the handler has gone its own way.
    if (!entry->replay.empty()) {
        entry->replay = nlohmann::json::object();
    }
    entry->replaying->store(false, std::memory_order_relaxed);
    const auto missing = missing_client_capabilities(entry->client_capabilities, request);
    if (!missing.empty()) {
        entry->missing_capabilities.merge_patch(missing);
        if (entry->event == MrtrEntry::Event::ePending) {
            entry->event = MrtrEntry::Event::eMissingCapability;
            entry->event_signal.cancel();
        }
        if (entry->sealed) {
            co_await hold_until_released(entry);
        }
        throw MrtrAbandoned{};
    }
    if (!named) {
        ++entry->next_key;
    }

    if (entry->sealed) {
        // The handler runs again from the start with this question answered, so it is not
        // resumed: it waits here until the server has put the question into the state and
        // released it.
        entry->requests[key.get_ref<const std::string&>()] = request;
        schedule_park(entry);
        co_await hold_until_released(entry);
        throw MrtrAbandoned{};
    }

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
        if (notification.is_object() &&
            notification.value("method", "") == methods::g_NOTIFICATIONS_PROGRESS &&
            notification.contains("params") && notification["params"].is_object()) {
            if (!entry->progress_token) {
                co_return;
            }
            notification["params"]["progressToken"] = *entry->progress_token;
            message = std::make_shared<const std::string>(mcp::detail::dump_json(notification));
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
            entry->event_signal.expires_after(mcp::detail::constants::mrtr::g_CANCEL_POLL_INTERVAL);
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
        outcome.answers = entry->answers;
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
    if (entry->answered_while_parked) {
        // Nothing can use the input now. Questions other coroutines of the handler left open are
        // released rather than left waiting for a round that will not come.
        entry->abandoned = true;
        entry->event = MrtrEntry::Event::eCompleted;
        entry->resume_signal.cancel();
        co_return;
    }
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

bool client_can_answer_input(const nlohmann::json& request_meta) {
    const auto declared = request_meta.find(mcp::constants::meta::g_META_CLIENT_CAPABILITIES);
    if (declared == request_meta.end() || !declared->is_object()) {
        return false;
    }
    constexpr std::array<const char*, 3> names{"elicitation", "sampling", "roots"};
    return std::ranges::any_of(names, [&declared](const char* name) {
        const auto capability = declared->find(name);
        return capability != declared->end() && capability->is_object();
    });
}

bool InlineInput::refuse(const nlohmann::json& request) {
    const auto& meta = *request_meta_->meta;
    const auto declared = meta.find(mcp::constants::meta::g_META_CLIENT_CAPABILITIES);
    const auto missing = missing_client_capabilities(
        declared != meta.end() ? *declared : nlohmann::json::object(), request);
    if (missing.empty()) {
        return false;
    }
    std::scoped_lock lock(mutex_);
    missing_.merge_patch(missing);
    return true;
}

nlohmann::json InlineInput::missing_capabilities() const {
    std::scoped_lock lock(mutex_);
    return missing_;
}

bool asks_for_sampling_context(const nlohmann::json& params) {
    // find() gives end() for a value that is no object.
    const auto include = params.find("includeContext");
    return include != params.end() && include->is_string() && *include != "none";
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

    if (method == methods::g_ELICITATION_CREATE) {
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
    } else if (method == methods::g_SAMPLING_CREATE_MESSAGE) {
        const auto* sampling = declared("sampling");
        if (sampling == nullptr) {
            return {{"sampling", nlohmann::json::object()}};
        }
        const auto tools = params.find("tools");
        if (tools != params.end() && tools->is_array() && !tools->empty() &&
            !sampling->contains("tools")) {
            return {{"sampling", {{"tools", nlohmann::json::object()}}}};
        }
        if (asks_for_sampling_context(params) && !sampling->contains("context")) {
            return {{"sampling", {{"context", nlohmann::json::object()}}}};
        }
    } else if (method == methods::g_ROOTS_LIST) {
        if (declared("roots") == nullptr) {
            return {{"roots", nlohmann::json::object()}};
        }
    }
    return nlohmann::json::object();
}

Task<nlohmann::json> mrtr_request_input(std::shared_ptr<MrtrEntry> entry, nlohmann::json method,
                                        std::optional<nlohmann::json> params,
                                        nlohmann::json chosen_key) {
    auto strand = entry->strand;
    co_return co_await spawn_awaitable(
        strand, request_input_on_strand(std::move(entry), std::move(method), std::move(params),
                                        std::move(chosen_key)));
}

Task<void> mrtr_notify(std::shared_ptr<MrtrEntry> entry, std::shared_ptr<const std::string> message) {
    auto strand = entry->strand;
    co_await spawn_awaitable(strand, notify_on_strand(std::move(entry), std::move(message)));
}

void mrtr_complete(const std::shared_ptr<MrtrEntry>& entry, std::string wire) {
    if (entry->abandoned) {
        if (!entry->sealed) {
            entry->counters->abandoned_completed.fetch_add(1, std::memory_order_relaxed);
        }
        return;
    }
    if (entry->event == MrtrEntry::Event::eParked) {
        entry->wire = std::move(wire);
        entry->answered_while_parked = true;
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
    boost::asio::post(entry->strand, [releaser = HeldReleaser(entry)] {
        const auto& entry = releaser.entry;
        entry->abandoned = true;
        entry->cancelled->store(true, std::memory_order_relaxed);
        entry->resume_signal.cancel();
    });
}

std::string mrtr_principal_digest(std::string_view principal) {
    if (principal.empty()) {
        return {};
    }
    return base64url_encode(digest_bytes(sha256(principal)));
}

std::string mrtr_seal_state(const sealed_state::Codec& codec, const MrtrSealedState& state) {
    const nlohmann::json payload = {
        {"v", 1},
        {"i", state.id},
        {"e", state.expires_ms},
        {"p", state.principal_digest},
        {"m", state.method},
        {"d", base64url_encode(state.params_digest)},
        {"r", state.round},
        {"k", state.asked},
        {"a", state.answers},
    };
    auto sealed = codec.seal(mcp::detail::dump_json(payload));
    return sealed ? std::move(sealed.token) : std::string{};
}

std::optional<MrtrSealedState> mrtr_open_state(const sealed_state::Codec& codec,
                                               std::string_view token) {
    const auto opened = codec.open(token);
    if (!opened) {
        return std::nullopt;
    }
    const auto payload = nlohmann::json::parse(opened.payload, nullptr, false);
    if (!payload.is_object() || payload.value("v", 0) != 1) {
        return std::nullopt;
    }
    try {
        MrtrSealedState state;
        state.id = payload.at("i").get<std::string>();
        state.expires_ms = payload.at("e").get<std::int64_t>();
        state.principal_digest = payload.at("p").get<std::string>();
        state.method = payload.at("m").get<std::string>();
        auto digest = base64url_decode(payload.at("d").get<std::string>());
        if (!digest) {
            return std::nullopt;
        }
        state.params_digest = std::move(*digest);
        state.round = payload.at("r").get<std::uint32_t>();
        state.asked = payload.at("k").get<std::map<std::string, std::string>>();
        state.answers = payload.at("a");
        if (!state.answers.is_object()) {
            return std::nullopt;
        }
        for (const auto& answer : state.answers) {
            if (!answer.is_object() || !answer.contains("r") || !answer.contains("m") ||
                !answer.at("m").is_string()) {
                return std::nullopt;
            }
        }
        return state;
    } catch (const nlohmann::json::exception&) {
        return std::nullopt;
    }
}

std::string mrtr_params_digest(const nlohmann::json& params) {
    nlohmann::json salient = params.is_object() ? params : nlohmann::json::object();
    salient.erase("_meta");
    salient.erase("inputResponses");
    salient.erase("requestState");
    return std::string(digest_bytes(sha256(dump_json(salient))));
}

}  // namespace mcp::detail

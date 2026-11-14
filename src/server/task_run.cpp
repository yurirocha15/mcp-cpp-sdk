#include "task_run.hpp"

#include "mrtr.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <algorithm>
#include <chrono>
#include <sstream>
#include <stdexcept>

namespace mcp::detail {

namespace {

constexpr std::chrono::seconds g_wait_limit{5};

}  // namespace

void TaskRun::signal() {
    version_.fetch_add(1, std::memory_order_acq_rel);
    boost::asio::post(strand, [run = shared_from_this()] {
        for (auto* timer : run->waiters_) {
            timer->cancel();
        }
    });
}

Task<void> TaskRun::abandon_after(std::shared_ptr<TaskRun> run, std::chrono::milliseconds ttl) {
    if (run->expiry_stopped_) {
        co_return;
    }
    // A time to live far beyond any clock's range is capped, so that the deadline stays valid.
    constexpr std::chrono::hours g_longest{24 * 365 * 100};
    boost::asio::steady_timer timer(run->strand);
    run->expiry_ = &timer;
    timer.expires_after(std::min<std::chrono::milliseconds>(ttl, g_longest));
    boost::system::error_code wait_result;
    co_await timer.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, wait_result));
    run->expiry_ = nullptr;
    if (!wait_result) {
        run->abandon();
    }
}

void TaskRun::stop_expiry() {
    expiry_stopped_ = true;
    if (expiry_ != nullptr) {
        expiry_->cancel();
    }
}

void TaskRun::publish_detached(std::shared_ptr<const TaskRecord> record) {
    if (!publish) {
        return;
    }
    boost::asio::co_spawn(strand, publish(std::move(record)), boost::asio::detached);
}

Task<void> TaskRun::wait_after(std::uint64_t seen) {
    co_await boost::asio::co_spawn(strand, task_wait_on_strand(shared_from_this(), seen),
                                   boost::asio::use_awaitable);
}

Task<void> task_wait_on_strand(std::shared_ptr<TaskRun> run, std::uint64_t seen) {
    // A signal increments the version before it posts the wake, so a signal that came since @p seen
    // is seen here, and one that comes later cancels the wait that starts below.
    if (run->version() != seen) {
        co_return;
    }
    boost::asio::steady_timer timer(run->strand);
    timer.expires_after(g_wait_limit);
    run->waiters_.insert(&timer);
    boost::system::error_code ignored;
    co_await timer.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, ignored));
    run->waiters_.erase(&timer);
}

Task<nlohmann::json> task_request_input(std::shared_ptr<TaskRun> run, nlohmann::json method,
                                        std::optional<nlohmann::json> params,
                                        nlohmann::json chosen_key) {
    if (run->cancelled->load(std::memory_order_acquire)) {
        throw TaskAbandoned{};
    }
    nlohmann::json request = {{"method", method}};
    if (params) {
        request["params"] = std::move(*params);
        // 2026-07-28 has no elicitation ids: an elicitation's answer comes back under its key.
        if (method == "elicitation/create" && request["params"].is_object()) {
            request["params"].erase("elicitationId");
        }
    }
    const auto missing = missing_client_capabilities(run->client_capabilities, request);
    if (!missing.empty()) {
        throw std::runtime_error("The client did not declare the capability this request needs: " +
                                 missing.dump());
    }

    const bool named = chosen_key.is_string();
    if (named) {
        const auto& text = chosen_key.get_ref<const std::string&>();
        if (text.empty()) {
            throw std::invalid_argument("the key of a question for the client must not be empty");
        }
        constexpr std::string_view prefix = "input-";
        if (text.size() > prefix.size() && text.compare(0, prefix.size(), prefix) == 0 &&
            text.find_first_not_of("0123456789", prefix.size()) == std::string::npos) {
            throw std::invalid_argument("the key '" + text +
                                        "' is reserved for the names the SDK generates");
        }
        if (!run->claim_key(text)) {
            throw std::invalid_argument("the key '" + text + "' already names a question of this task");
        }
    }
    // [gcc11-sso: string-in-frame] The key stays a json value, which keeps its text on the heap.
    const nlohmann::json key =
        named ? std::move(chosen_key) : nlohmann::json("input-" + std::to_string(run->next_key()));
    std::shared_ptr<const TaskRecord> added;
    if (auto stored = run->store->add_input_request(run->id, key.get_ref<const std::string&>(),
                                                    std::move(request))) {
        added = std::make_shared<const TaskRecord>(std::move(*stored));
    } else {
        throw TaskAbandoned{};
    }
    if (run->publish) {
        co_await run->publish(std::move(added));
    }

    for (;;) {
        // Read before the task is looked at, so that an answer that arrives in between wakes the wait.
        const auto seen = run->version();
        if (auto answer = run->store->take_input_response(run->id, key.get_ref<const std::string&>())) {
            co_return std::move(*answer);
        }
        if (run->cancelled->load(std::memory_order_acquire) || !run->store->get(run->owner, run->id)) {
            throw TaskAbandoned{};
        }
        co_await run->wait_after(seen);
    }
}

std::string task_progress_message(double progress, const std::optional<double>& total,
                                  const std::optional<std::string>& message) {
    if (message) {
        return *message;
    }
    std::ostringstream text;
    text << "Progress: " << progress;
    if (total) {
        text << " of " << *total;
    }
    return text.str();
}

}  // namespace mcp::detail

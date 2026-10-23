#include "../detail/diagnostic_text.hpp"
#include "../detail/peer_json.hpp"
#include "../detail/spawn_awaitable.hpp"

#include <mcp/client/client.hpp>
#include <mcp/detail/serialized_transport_writer.hpp>

#include <algorithm>
#include <atomic>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/system_error.hpp>
#include <chrono>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mcp {

namespace {

std::optional<nlohmann::json> make_paginated_params(const std::optional<std::string>& cursor) {
    if (!cursor) {
        return std::nullopt;
    }

    PaginatedRequestParams params;
    params.cursor = cursor;
    return nlohmann::json(std::move(params));
}

}  // namespace

/// `error.message` is text the peer chose (dispatch_response() deserializes `error` with no
/// validation) and what() is what an application logs, so what() is flattened and bounded. error(),
/// and the message() it exposes, are structured protocol data a caller may compare or re-encode, and
/// stay exactly as the peer sent them; the split matches MetadataPolicyError's.
McpError::McpError(Error error)
    : std::runtime_error("JSON-RPC error " + std::to_string(error.code) + ": " +
                         detail::sanitize_for_diagnostics(error.message)),
      error_(std::move(error)) {}

McpError::McpError(int code, std::string message, std::optional<nlohmann::json> data)
    : McpError(Error{code, std::move(message), std::move(data)}) {}

ClientProtocol ClientProtocol::Auto() { return {Kind::eAuto, std::string()}; }

ClientProtocol ClientProtocol::Legacy() {
    return {Kind::eLegacy, std::string(g_LATEST_LEGACY_PROTOCOL_VERSION)};
}

ClientProtocol ClientProtocol::Version(std::string version) {
    if (!is_supported_protocol_version(version)) {
        throw std::invalid_argument("Unknown protocol version: " +
                                    detail::sanitize_for_diagnostics(version));
    }
    return {Kind::eVersion, std::move(version)};
}

namespace {

/// Checks a tools/call result against the tool's output schema, when the client has one for it.
Task<CallToolResult> checked_call_result(Task<nlohmann::json> request,
                                         std::optional<CompiledSchema> output_schema) {
    auto result_json = co_await std::move(request);
    auto result = result_json.get<CallToolResult>();
    if (output_schema && !result.isError.value_or(false) && result.structuredContent) {
        const auto validation = output_schema->validate(*result.structuredContent);
        if (!validation.valid) {
            std::string message = "structuredContent does not conform to the tool's outputSchema";
            nlohmann::json violations = nlohmann::json::array();
            for (const auto& violation : validation.violations) {
                violations.push_back(
                    {{"instancePath", violation.instance_path}, {"message", violation.message}});
            }
            throw StructuredContentError(g_INTERNAL_ERROR, std::move(message),
                                         nlohmann::json{{"violations", std::move(violations)}});
        }
    }
    co_return result;
}

}  // namespace

namespace detail {

/// The client's side of one subscriptions/listen stream. Every change to it happens on the client
/// strand; the mutex only lets the handle's accessors read it from any thread.
struct SubscriptionState {
    SubscriptionState(const boost::asio::strand<boost::asio::any_io_executor>& strand,
                      std::size_t limit)
        : wake(strand), queue_limit(limit) {}

    /// Wakes whoever waits for the acknowledgment or the next notification.
    boost::asio::steady_timer wake;
    std::size_t queue_limit;
    RequestId id;
    SubscriptionFilter requested;
    /// Sends notifications/cancelled for the listen request, when the connection is still open.
    std::function<Task<void>()> send_cancel;
    /// Takes the stream out of the client's table of open streams.
    std::function<void()> forget;

    mutable std::mutex mutex;
    bool acknowledged_seen{false};
    SubscriptionFilter acknowledged;
    std::deque<SubscriptionEvent> queue;
    std::optional<SubscriptionEnd> ended;
    std::optional<Error> failure;
    std::uint64_t dropped{0};

    void signal() { wake.expires_at(std::chrono::steady_clock::now()); }

    void acknowledge(SubscriptionFilter honored) {
        {
            std::lock_guard lock(mutex);
            if (acknowledged_seen) {
                return;
            }
            acknowledged = std::move(honored);
            acknowledged_seen = true;
        }
        signal();
    }

    void push(SubscriptionEvent event) {
        {
            std::lock_guard lock(mutex);
            if (ended) {
                return;
            }
            if (queue.size() >= queue_limit) {
                queue.pop_front();
                ++dropped;
            }
            queue.push_back(std::move(event));
        }
        signal();
    }

    void finish(SubscriptionEnd how, std::optional<Error> error) {
        {
            std::lock_guard lock(mutex);
            if (ended) {
                return;
            }
            ended = how;
            failure = std::move(error);
        }
        signal();
    }

    /// The state of the wait for the acknowledgment; holds no string, so it is safe to keep
    /// across a suspension point.
    enum class AckWait : std::uint8_t {
        ePending,
        eAcknowledged,
        eEnded
    };

    AckWait ack_wait() const {
        std::lock_guard lock(mutex);
        if (acknowledged_seen) {
            return AckWait::eAcknowledged;
        }
        return ended ? AckWait::eEnded : AckWait::ePending;
    }

    /// Runs on the strand: the next notification, or nothing once the stream has ended.
    static Task<std::optional<SubscriptionEvent>> wait_next(std::shared_ptr<SubscriptionState> state) {
        for (;;) {
            {
                std::lock_guard lock(state->mutex);
                if (!state->queue.empty()) {
                    std::optional<SubscriptionEvent> event(std::move(state->queue.front()));
                    state->queue.pop_front();
                    co_return event;
                }
                if (state->ended) {
                    co_return std::nullopt;
                }
            }
            // Ending the stream and queueing both run on this strand and signal after changing
            // the state, so a change is either seen above or wakes the wait that starts here.
            state->wake.expires_at(std::chrono::steady_clock::time_point::max());
            try {
                co_await state->wake.async_wait(boost::asio::use_awaitable);
            } catch (const boost::system::system_error& error) {
                if (error.code() != boost::asio::error::operation_aborted) {
                    throw;
                }
            }
        }
    }

    /// Runs on the strand: ends the stream locally, then tells the server.
    static Task<void> cancel(std::shared_ptr<SubscriptionState> state) {
        {
            std::lock_guard lock(state->mutex);
            if (state->ended) {
                co_return;
            }
        }
        state->finish(SubscriptionEnd::eCancelled, std::nullopt);
        state->forget();
        co_await state->send_cancel();
    }
};

}  // namespace detail

namespace {

Task<std::optional<SubscriptionEvent>> no_event() { co_return std::nullopt; }

Task<void> nothing_to_send() { co_return; }

}  // namespace

Subscription::Subscription() = default;

Subscription::Subscription(std::shared_ptr<detail::SubscriptionState> state)
    : state_(std::move(state)) {}

RequestId Subscription::id() const { return state_ ? state_->id : RequestId{}; }

SubscriptionFilter Subscription::requested() const {
    return state_ ? state_->requested : SubscriptionFilter{};
}

SubscriptionFilter Subscription::acknowledged() const {
    if (!state_) {
        return {};
    }
    std::lock_guard lock(state_->mutex);
    return state_->acknowledged;
}

SubscriptionFilter Subscription::unhonored() const {
    const auto honored = acknowledged();
    const auto asked = requested();
    SubscriptionFilter missing;
    missing.toolsListChanged = asked.toolsListChanged && !honored.toolsListChanged;
    missing.promptsListChanged = asked.promptsListChanged && !honored.promptsListChanged;
    missing.resourcesListChanged = asked.resourcesListChanged && !honored.resourcesListChanged;
    for (const auto& uri : asked.resourceSubscriptions) {
        if (std::find(honored.resourceSubscriptions.begin(), honored.resourceSubscriptions.end(),
                      uri) == honored.resourceSubscriptions.end()) {
            missing.resourceSubscriptions.push_back(uri);
        }
    }
    return missing;
}

Task<std::optional<SubscriptionEvent>> Subscription::next() {
    if (!state_) {
        return no_event();
    }
    return detail::spawn_awaitable(state_->wake.get_executor(),
                                   detail::SubscriptionState::wait_next(state_));
}

Task<void> Subscription::close() {
    if (!state_) {
        return nothing_to_send();
    }
    return detail::spawn_awaitable(state_->wake.get_executor(),
                                   detail::SubscriptionState::cancel(state_));
}

std::optional<SubscriptionEnd> Subscription::end_reason() const {
    if (!state_) {
        return SubscriptionEnd::eCancelled;
    }
    std::lock_guard lock(state_->mutex);
    return state_->ended;
}

std::optional<Error> Subscription::end_error() const {
    if (!state_) {
        return std::nullopt;
    }
    std::lock_guard lock(state_->mutex);
    return state_->failure;
}

std::uint64_t Subscription::dropped_events() const {
    if (!state_) {
        return 0;
    }
    std::lock_guard lock(state_->mutex);
    return state_->dropped;
}

namespace {

/// The `notifications` object of an acknowledgment, read as leniently as the server's own
/// output allows: a member of the wrong type counts as not honored.
SubscriptionFilter honored_filter(const nlohmann::json& params) {
    SubscriptionFilter honored;
    const auto found = params.find("notifications");
    if (found == params.end() || !found->is_object()) {
        return honored;
    }
    const auto flag = [&found](const char* name) {
        const auto member = found->find(name);
        return member != found->end() && member->is_boolean() && member->get<bool>();
    };
    honored.toolsListChanged = flag("toolsListChanged");
    honored.promptsListChanged = flag("promptsListChanged");
    honored.resourcesListChanged = flag("resourcesListChanged");
    if (const auto uris = found->find("resourceSubscriptions");
        uris != found->end() && uris->is_array()) {
        for (const auto& uri : *uris) {
            if (uri.is_string()) {
                honored.resourceSubscriptions.push_back(uri.get<std::string>());
            }
        }
    }
    return honored;
}

}  // namespace

struct Client::Impl {
    struct PendingRequest {
        PendingRequest(const boost::asio::any_io_executor& executor, std::chrono::milliseconds timeout)
            : timer(executor), deadline(std::chrono::steady_clock::now() + timeout) {
            timer.expires_at(deadline);
        }

        boost::asio::steady_timer timer;
        std::chrono::steady_clock::time_point deadline;
        std::shared_ptr<std::atomic<bool>> cancel_write_before_start =
            std::make_shared<std::atomic<bool>>(false);
        nlohmann::json result;
        std::optional<Error> error;
        std::exception_ptr write_error;
        bool completed{false};
        /// The era probe: a reply that cannot be decoded is an answer to it, see fail_probe().
        bool probe{false};
    };

    Impl(std::shared_ptr<ITransport> client_transport, const boost::asio::any_io_executor& executor,
         ClientOptions client_options)
        : transport(std::move(client_transport)),
          strand(boost::asio::make_strand(executor)),
          options(std::move(client_options)),
          writer(transport, strand) {}

    /// Sends a request of the session's era. A modern session adds the per-request `_meta` and checks
    /// the result type of the answer; a legacy session sends @p params as given.
    static Task<nlohmann::json> request(std::shared_ptr<Impl> state, std::string_view method,
                                        const std::optional<nlohmann::json>& params,
                                        const RequestOptions& request_options) {
        const auto modern_version = state->modern_version();
        if (!modern_version) {
            return request_wire(std::move(state), method, params, request_options, false);
        }
        reject_removed_method(*modern_version, method);
        if (supports_input_required(method)) {
            return resume_input_required(std::move(state), std::make_shared<const std::string>(method),
                                         params, request_options,
                                         std::make_shared<const std::string>(*modern_version));
        }
        auto modern_params =
            state->with_request_meta(*modern_version, params, request_options.log_level);
        return checked_result(
            request_wire(std::move(state), method, modern_params, request_options, false));
    }

    static Task<nlohmann::json> request_wire(std::shared_ptr<Impl> state, std::string_view method,
                                             const std::optional<nlohmann::json>& params,
                                             const RequestOptions& request_options, bool probe) {
        const auto timeout = request_options.timeout.value_or(state->options.request_timeout);
        if (timeout <= std::chrono::milliseconds::zero()) {
            throw std::invalid_argument("Request timeout must be positive");
        }

        const int64_t id = state->next_request_id.fetch_add(1, std::memory_order_relaxed);
        JSONRPCRequest request;
        request.id = RequestId{std::to_string(id)};
        request.method = std::string(method);
        request.params = params;
        auto wire = std::make_shared<const std::string>(nlohmann::json(std::move(request)).dump());
        auto strand = state->strand;
        // Awaiting post(strand) would not rebind a caller-owned coroutine's continuation.
        // Spawn the complete pending-request lifecycle on the client strand instead.
        return boost::asio::co_spawn(
            strand, send_request_wire(std::move(state), std::move(wire), id, timeout, probe),
            boost::asio::use_awaitable);
    }

    /// The methods 2026-07-28 removed. The server answers none of them, so the request is refused
    /// here with the reason instead of costing a round trip for a method-not-found.
    static void reject_removed_method(const std::string& version, std::string_view method) {
        static constexpr std::array<std::string_view, 5> g_removed = {
            "initialize", "ping", "logging/setLevel", "resources/subscribe", "resources/unsubscribe"};
        if (std::find(g_removed.begin(), g_removed.end(), method) != g_removed.end()) {
            throw McpError(g_METHOD_NOT_FOUND, "Method " + std::string(method) +
                                                   " does not exist in protocol version " + version);
        }
    }

    /// A request carrying `resultType` takes part in the 2026-07-28 result rules: absent means
    /// "complete", and any other value is one this client cannot act on.
    static Task<nlohmann::json> checked_result(Task<nlohmann::json> pending) {
        auto result = co_await std::move(pending);
        check_complete(result);
        co_return result;
    }

    static void check_complete(const nlohmann::json& result) {
        if (result.is_object()) {
            const auto found = result.find("resultType");
            if (found != result.end() && !(found->is_string() && *found == "complete")) {
                std::string shown = found->is_string() ? found->get<std::string>() : found->dump();
                throw McpError(g_INVALID_REQUEST, "Unsupported resultType in result: " +
                                                      detail::sanitize_for_diagnostics(shown));
            }
        }
    }

    /// The requests whose answer may be an input_required result.
    static bool supports_input_required(std::string_view method) {
        return method == "tools/call" || method == "prompts/get" || method == "resources/read";
    }

    /// Answers one input_required result: runs the registered handler of each entry in
    /// `inputRequests` and returns the `inputResponses` for the retry, keyed as the server asked.
    static Task<nlohmann::json> answer_input_requests(std::shared_ptr<Impl> state,
                                                      std::shared_ptr<const nlohmann::json> requests) {
        nlohmann::json responses = nlohmann::json::object();
        for (const auto& entry : requests->items()) {
            const auto& request = entry.value();
            if (!request.is_object() || !request.contains("method") || !request["method"].is_string()) {
                throw McpError(g_INVALID_REQUEST, "input_required entry " +
                                                      detail::sanitize_for_diagnostics(entry.key()) +
                                                      " is not a request");
            }
            const auto method =
                std::make_shared<const std::string>(request["method"].get<std::string>());
            RequestHandler handler;
            {
                std::lock_guard lock(state->handlers_mutex);
                const auto found = state->request_handlers.find(*method);
                if (found != state->request_handlers.end()) {
                    handler = found->second;
                }
            }
            const bool answerable = *method == "elicitation/create" ||
                                    *method == "sampling/createMessage" || *method == "roots/list";
            if (!answerable || !handler) {
                throw McpError(g_INVALID_REQUEST,
                               "The server asked for " + detail::sanitize_for_diagnostics(*method) +
                                   " in an input_required result, but " +
                                   (answerable ? "no handler is registered for it"
                                               : "that is not a request a client answers"));
            }
            const nlohmann::json handler_params =
                request.contains("params") ? request["params"] : nlohmann::json::object();
            auto answer = co_await handler(handler_params);
            responses[entry.key()] = std::move(answer);
        }
        co_return responses;
    }

    /// Sends a request that may be answered with an input_required result and resumes it: each
    /// round answers the server's questions with the registered handlers, then sends the original
    /// request again as a new request carrying `inputResponses` and the server's `requestState`
    /// untouched. The timeout of the call bounds all rounds together.
    static Task<nlohmann::json> resume_input_required(std::shared_ptr<Impl> state,
                                                      std::shared_ptr<const std::string> method,
                                                      std::optional<nlohmann::json> params,
                                                      RequestOptions request_options,
                                                      std::shared_ptr<const std::string> version) {
        const auto timeout = request_options.timeout.value_or(state->options.request_timeout);
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        std::shared_ptr<const nlohmann::json> input_responses;
        std::shared_ptr<const nlohmann::json> request_state;
        for (int round = 0;; ++round) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            if (remaining <= std::chrono::milliseconds::zero()) {
                throw McpError(g_REQUEST_TIMEOUT, "Request timed out");
            }
            request_options.timeout = remaining;
            auto round_params = state->with_request_meta(*version, params, request_options.log_level);
            if (input_responses) {
                (*round_params)["inputResponses"] = *input_responses;
            }
            if (request_state) {
                (*round_params)["requestState"] = *request_state;
            }
            auto reply = co_await request_wire(state, *method, round_params, request_options, false);
            const auto type = reply.is_object() ? reply.find("resultType") : reply.end();
            if (!reply.is_object() || type == reply.end() || !type->is_string() ||
                *type != "input_required") {
                check_complete(reply);
                co_return reply;
            }
            if (round >= request_options.max_input_rounds) {
                throw McpError(g_INVALID_REQUEST, "The server still asked for input after " +
                                                      std::to_string(request_options.max_input_rounds) +
                                                      " rounds (RequestOptions::max_input_rounds)");
            }
            const auto requests = reply.find("inputRequests");
            const auto reply_state = reply.find("requestState");
            const bool has_requests = requests != reply.end();
            const bool has_state = reply_state != reply.end();
            if ((has_requests && !requests->is_object()) || (has_state && !reply_state->is_string())) {
                throw McpError(g_INVALID_REQUEST,
                               "input_required result: inputRequests must be an object and "
                               "requestState a string");
            }
            if (!has_requests && !has_state) {
                throw McpError(g_INVALID_REQUEST,
                               "input_required result has neither inputRequests nor requestState");
            }
            request_state = has_state ? std::make_shared<const nlohmann::json>(*reply_state) : nullptr;
            if (has_requests) {
                auto questions = std::make_shared<const nlohmann::json>(*requests);
                auto answers = co_await answer_input_requests(state, std::move(questions));
                input_responses = std::make_shared<const nlohmann::json>(std::move(answers));
            } else {
                input_responses = nullptr;
            }
        }
    }

    /// Opens a subscriptions/listen stream. The request is never answered while the stream is open,
    /// so it bypasses the table of pending requests (and their deadline): the stream has its own
    /// entry, and `timeout` limits only the wait for the acknowledgment.
    static Task<Subscription> listen(std::shared_ptr<Impl> state, const SubscriptionFilter& filter,
                                     const RequestOptions& request_options) {
        const auto modern_version = state->modern_version();
        if (!modern_version) {
            throw McpError(g_METHOD_NOT_FOUND,
                           "subscriptions/listen needs a session of protocol version 2026-07-28 or "
                           "later");
        }
        const auto timeout = request_options.timeout.value_or(state->options.request_timeout);
        if (timeout <= std::chrono::milliseconds::zero()) {
            throw std::invalid_argument("Request timeout must be positive");
        }

        nlohmann::json params = nlohmann::json::object();
        params["notifications"] = filter;
        const auto wire_params =
            state->with_request_meta(*modern_version, params, request_options.log_level);

        const int64_t id = state->next_request_id.fetch_add(1, std::memory_order_relaxed);
        JSONRPCRequest request;
        request.id = RequestId{std::to_string(id)};
        request.method = "subscriptions/listen";
        request.params = wire_params;
        auto wire = std::make_shared<const std::string>(nlohmann::json(std::move(request)).dump());

        auto stream = std::make_shared<detail::SubscriptionState>(
            state->strand, state->options.subscription_queue_limit);
        stream->id = RequestId{std::to_string(id)};
        stream->requested = filter;
        std::weak_ptr<Impl> weak_state = state;
        const auto key = std::make_shared<const std::string>(request_correlation_key(id));
        stream->forget = [weak_state, key] {
            if (auto active = weak_state.lock()) {
                std::lock_guard lock(active->subscriptions_mutex);
                active->subscriptions.erase(*key);
            }
        };
        stream->send_cancel = [weak_state, id]() -> Task<void> {
            auto active = weak_state.lock();
            if (!active || active->closed.load(std::memory_order_acquire)) {
                return nothing_to_send();
            }
            CancelledNotificationParams cancelled;
            cancelled.requestId = RequestId{std::to_string(id)};
            const std::optional<nlohmann::json> cancel_params = nlohmann::json(std::move(cancelled));
            return notification(std::move(active), "notifications/cancelled", cancel_params);
        };

        auto strand = state->strand;
        return detail::spawn_awaitable(
            strand, listen_on_strand(std::move(state), std::move(stream), std::move(wire), timeout));
    }

    static Task<Subscription> listen_on_strand(std::shared_ptr<Impl> state,
                                               std::shared_ptr<detail::SubscriptionState> stream,
                                               std::shared_ptr<const std::string> wire,
                                               std::chrono::milliseconds timeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        {
            std::lock_guard lock(state->subscriptions_mutex);
            state->subscriptions.emplace(stream->id.correlation_key(), stream);
        }
        // The stream is registered before the closed check, so a connection that ends from here on
        // either shows as closed or finds the stream and ends it.
        if (state->closed.load(std::memory_order_acquire)) {
            stream->forget();
            throw McpError(g_CONNECTION_CLOSED, "Client transport is closed");
        }

        try {
            co_await state->writer.write_message(wire);
        } catch (...) {
            stream->forget();
            if (state->closed.load(std::memory_order_acquire)) {
                throw McpError(g_CONNECTION_CLOSED, "Client transport is closed");
            }
            throw;
        }

        for (;;) {
            if (stream->ack_wait() != detail::SubscriptionState::AckWait::ePending ||
                std::chrono::steady_clock::now() >= deadline) {
                break;
            }
            stream->wake.expires_at(deadline);
            try {
                co_await stream->wake.async_wait(boost::asio::use_awaitable);
            } catch (const boost::system::system_error& error) {
                if (error.code() != boost::asio::error::operation_aborted) {
                    stream->forget();
                    throw;
                }
            }
        }

        const auto outcome = stream->ack_wait();
        if (outcome == detail::SubscriptionState::AckWait::eAcknowledged) {
            co_return Subscription(stream);
        }
        if (outcome == detail::SubscriptionState::AckWait::eEnded) {
            std::optional<Error> failure;
            {
                std::lock_guard lock(stream->mutex);
                failure = stream->failure;
            }
            if (failure) {
                throw McpError(std::move(*failure));
            }
            throw McpError(g_INTERNAL_ERROR, "subscriptions/listen ended before it was acknowledged");
        }

        // No acknowledgment in time: end the stream here and tell the server, best effort.
        stream->finish(SubscriptionEnd::eCancelled, std::nullopt);
        stream->forget();
        try {
            co_await stream->send_cancel();
        } catch (...) {
            // The request timeout below is the failure to report.
        }
        throw McpError(g_REQUEST_TIMEOUT, "subscriptions/listen was not acknowledged in time");
    }

    /// Routes a notification that names a subscription to that subscription's queue. The
    /// acknowledgment is consumed by the stream; every notification still reaches the handlers
    /// registered with on_notification().
    static void route_to_subscription(const std::shared_ptr<Impl>& state, const std::string& method,
                                      const nlohmann::json& json_message) {
        if (!detail::has_json_value(json_message, "params")) {
            return;
        }
        const auto& params = json_message.at("params");
        if (!params.is_object()) {
            return;
        }
        const auto meta = params.find("_meta");
        if (meta == params.end() || !meta->is_object()) {
            return;
        }
        const auto named = meta->find(std::string(g_META_SUBSCRIPTION_ID));
        if (named == meta->end() || (!named->is_string() && !named->is_number_integer())) {
            return;
        }
        std::shared_ptr<detail::SubscriptionState> stream;
        {
            std::lock_guard lock(state->subscriptions_mutex);
            const auto found = state->subscriptions.find(named->get<RequestId>().correlation_key());
            if (found == state->subscriptions.end()) {
                return;
            }
            stream = found->second;
        }
        if (method == "notifications/subscriptions/acknowledged") {
            stream->acknowledge(honored_filter(params));
            return;
        }
        stream->push(SubscriptionEvent{method, params});
    }

    /// The answer to a listen request ends its stream: the empty result is the server's graceful
    /// close, an error is a refusal.
    static void end_subscription_from_response(const std::shared_ptr<Impl>& state,
                                               const std::string& id_key,
                                               const nlohmann::json& json_message) {
        std::shared_ptr<detail::SubscriptionState> stream;
        {
            std::lock_guard lock(state->subscriptions_mutex);
            const auto found = state->subscriptions.find(id_key);
            if (found == state->subscriptions.end()) {
                return;
            }
            stream = found->second;
            state->subscriptions.erase(found);
        }
        if (detail::has_json_value(json_message, "error")) {
            try {
                stream->finish(SubscriptionEnd::eFailed, json_message.at("error").get<Error>());
            } catch (const std::exception& decode_error) {
                stream->finish(SubscriptionEnd::eFailed,
                               Error{g_INVALID_REQUEST,
                                     "Malformed JSON-RPC error object: " +
                                         detail::sanitize_for_diagnostics(decode_error.what()),
                                     std::nullopt});
            }
            return;
        }
        stream->finish(SubscriptionEnd::eServerClosed, std::nullopt);
    }

    /// On a transport without per-request streams the server ends a subscription it closes with
    /// notifications/cancelled naming the listen request, after the closing result. Returns whether
    /// the notification named an open stream; that stream ends as closed by the server.
    static bool end_subscription_from_cancel(const std::shared_ptr<Impl>& state,
                                             const nlohmann::json& json_message) {
        if (!detail::has_json_value(json_message, "params")) {
            return false;
        }
        const auto& params = json_message.at("params");
        const auto named = params.is_object() ? params.find("requestId") : params.end();
        if (named == params.end() || (!named->is_string() && !named->is_number_integer())) {
            return false;
        }
        std::shared_ptr<detail::SubscriptionState> stream;
        {
            std::lock_guard lock(state->subscriptions_mutex);
            const auto found = state->subscriptions.find(named->get<RequestId>().correlation_key());
            if (found == state->subscriptions.end()) {
                return false;
            }
            stream = found->second;
            state->subscriptions.erase(found);
        }
        stream->finish(SubscriptionEnd::eServerClosed, std::nullopt);
        return true;
    }

    /// The connection ended: every open stream ends without the server's closing result.
    static void end_subscriptions(const std::shared_ptr<Impl>& state, const Error& error) {
        std::map<std::string, std::shared_ptr<detail::SubscriptionState>, std::less<>> open;
        {
            std::lock_guard lock(state->subscriptions_mutex);
            open.swap(state->subscriptions);
        }
        for (auto& [key, stream] : open) {
            static_cast<void>(key);
            stream->finish(SubscriptionEnd::eDropped, error);
        }
    }

    static Task<void> notification(std::shared_ptr<Impl> state, std::string_view method,
                                   const std::optional<nlohmann::json>& params) {
        JSONRPCNotification notification;
        notification.method = std::string(method);
        notification.params = params;
        auto wire = std::make_shared<const std::string>(nlohmann::json(std::move(notification)).dump());
        auto strand = state->strand;
        // Keep the closed-state check and write initiation on the client strand.
        return boost::asio::co_spawn(strand, send_notification_wire(std::move(state), std::move(wire)),
                                     boost::asio::use_awaitable);
    }

    /// What the probe learned. `discover` is set for eDiscover and `error` for the error kinds; a
    /// probe the transport failed has neither.
    struct ProbeOutcome {
        enum class Kind : std::uint8_t {
            eDiscover,     ///< A DiscoverResult: the server is modern.
            eModernError,  ///< A modern protocol error: the server is modern, the request is not.
            eOtherError,   ///< Any other error, a reply that is not JSON, or a failed write: legacy.
            eTimeout,      ///< No reply in time: legacy.
            eClosed,       ///< The connection ended: neither.
        };
        Kind kind{Kind::eOtherError};
        std::optional<DiscoverResult> discover;
        std::optional<Error> error;
    };

    /// The outcome of one initialize attempt; `error` is the server's refusal of it.
    struct InitializeAttempt {
        std::optional<ConnectResult> result;
        std::optional<Error> error;
    };

    // The outcomes below travel as shared_ptr: they hold strings, and a coroutine frame must not
    // keep a std::string across a suspension point (see the header note on GCC 11).
    using ProbePtr = std::shared_ptr<ProbeOutcome>;
    using AttemptPtr = std::shared_ptr<InitializeAttempt>;
    using VersionPtr = std::shared_ptr<const std::string>;
    using VersionsPtr = std::shared_ptr<const std::vector<std::string>>;

    static bool is_modern_error_code(int code) {
        return code == g_UNSUPPORTED_PROTOCOL_VERSION || code == g_HEADER_MISMATCH ||
               code == g_MISSING_REQUIRED_CLIENT_CAPABILITY;
    }

    static ProbePtr classify_probe_error(const std::shared_ptr<Impl>& state, const Error& error) {
        auto outcome = std::make_shared<ProbeOutcome>();
        outcome->error = error;
        if (state->closed.load(std::memory_order_acquire)) {
            outcome->kind = ProbeOutcome::Kind::eClosed;
        } else if (is_modern_error_code(error.code)) {
            outcome->kind = ProbeOutcome::Kind::eModernError;
        } else if (error.code == g_REQUEST_TIMEOUT) {
            outcome->kind = ProbeOutcome::Kind::eTimeout;
        }
        return outcome;
    }

    static ProbePtr classify_probe_reply(const nlohmann::json& reply) {
        auto outcome = std::make_shared<ProbeOutcome>();
        try {
            auto discover = reply;
            if (discover.is_object() && !discover.contains("resultType")) {
                discover["resultType"] = "complete";
            }
            auto parsed = discover.get<DiscoverResult>();
            if (parsed.resultType == "complete") {
                outcome->kind = ProbeOutcome::Kind::eDiscover;
                outcome->discover = std::move(parsed);
            }
        } catch (const std::exception&) {
            // Not a DiscoverResult: whatever answered is not a modern server.
        }
        return outcome;
    }

    /// Sends server/discover naming @p version and waits at most @p timeout for the answer.
    static Task<ProbePtr> probe(std::shared_ptr<Impl> state, VersionPtr version,
                                std::chrono::milliseconds timeout) {
        ProbePtr outcome;
        nlohmann::json reply;
        bool answered = false;
        try {
            RequestOptions options;
            options.timeout = timeout;
            auto params = state->with_request_meta(*version, std::nullopt, std::nullopt);
            auto pending = request_wire(state, "server/discover", params, options, true);
            reply = co_await std::move(pending);
            answered = true;
        } catch (const McpError& error) {
            outcome = classify_probe_error(state, error.error());
        } catch (...) {
            // The write failed: an HTTP server refusing the POST, or the connection going away.
            outcome = std::make_shared<ProbeOutcome>();
            if (state->closed.load(std::memory_order_acquire)) {
                outcome->kind = ProbeOutcome::Kind::eClosed;
            }
        }
        if (answered) {
            outcome = classify_probe_reply(reply);
        }
        co_return outcome;
    }

    /// The versions a protocol error lists in `data.supported`, empty when it lists none.
    static std::vector<std::string> advertised_versions(const Error& error) {
        std::vector<std::string> versions;
        if (error.data && error.data->is_object()) {
            const auto found = error.data->find("supported");
            if (found != error.data->end() && found->is_array()) {
                for (const auto& version : *found) {
                    if (version.is_string()) {
                        versions.push_back(version.get<std::string>());
                    }
                }
            }
        }
        return versions;
    }

    /// The newest version of @p ours (oldest first) that @p theirs also lists, or null.
    template <std::size_t N>
    static VersionPtr newest_mutual(const std::array<std::string_view, N>& ours,
                                    const std::vector<std::string>& theirs) {
        for (auto version = ours.rbegin(); version != ours.rend(); ++version) {
            if (std::find(theirs.begin(), theirs.end(), *version) != theirs.end()) {
                return std::make_shared<const std::string>(*version);
            }
        }
        return nullptr;
    }

    static bool lists_modern_version(const Error& error) {
        const auto versions = advertised_versions(error);
        return std::any_of(versions.begin(), versions.end(), [](const std::string& version) {
            return is_modern_protocol_version(version);
        });
    }

    void report_era(ProtocolEra era, EraReason reason,
                    std::chrono::steady_clock::time_point started) const {
        if (!options.on_era_decision) {
            return;
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started);
        try {
            options.on_era_decision(EraDecision{era, reason, elapsed});
        } catch (...) {
            // Documented: the hook observes; a failure of its own has no listener.
        }
    }

    static std::optional<nlohmann::json> initialize_params(const std::shared_ptr<Impl>& state,
                                                           const std::string& version) {
        InitializeRequest initialize_request;
        initialize_request.protocolVersion = version;
        std::lock_guard lock(state->handlers_mutex);
        initialize_request.clientInfo = state->client_info;
        initialize_request.capabilities = state->client_capabilities;
        return nlohmann::json(std::move(initialize_request));
    }

    /// Sends initialize naming @p version. The server's refusal is returned, not thrown, so that
    /// the caller can tell a modern server that rejects initialize from a failure.
    static Task<AttemptPtr> attempt_initialize(std::shared_ptr<Impl> state, VersionPtr version,
                                               VersionsPtr advertised) {
        auto attempt = std::make_shared<InitializeAttempt>();
        nlohmann::json result_json;
        bool answered = false;
        try {
            auto params = initialize_params(state, *version);
            auto pending = request(state, "initialize", params, {});
            result_json = co_await std::move(pending);
            answered = true;
        } catch (const McpError& error) {
            if (state->closed.load(std::memory_order_acquire) || error.code() == g_REQUEST_TIMEOUT) {
                throw;
            }
            attempt->error = error.error();
        }
        if (!answered) {
            co_return attempt;
        }

        auto initialize_result =
            std::make_shared<InitializeResult>(result_json.get<InitializeResult>());
        if (state->options.strict_protocol_validation &&
            !is_legacy_protocol_version(initialize_result->protocolVersion)) {
            request_close(state, "Server selected an unsupported protocol version");
            // The version is whatever the remote server put in its `initialize` result, and
            // this message reaches the application's log. Flatten and bound it, or a server
            // answering with CR/LF forges a log line the application did not write.
            throw McpError(g_INVALID_REQUEST,
                           "Server selected unsupported protocol version: " +
                               detail::sanitize_for_diagnostics(initialize_result->protocolVersion));
        }

        co_await notification(state, "notifications/initialized", std::nullopt);

        ConnectResult connected;
        connected.era = ProtocolEra::eLegacy;
        connected.protocolVersion = std::move(initialize_result->protocolVersion);
        connected.capabilities = std::move(initialize_result->capabilities);
        connected.serverInfo = std::move(initialize_result->serverInfo);
        connected.instructions = std::move(initialize_result->instructions);
        connected.supportedVersions = *advertised;
        attempt->result = std::move(connected);
        co_return attempt;
    }

    /// initialize for a caller that has nowhere else to go: a refusal is an error.
    static Task<ConnectResult> initialize_session(std::shared_ptr<Impl> state, VersionPtr version,
                                                  VersionsPtr advertised) {
        auto attempt =
            co_await attempt_initialize(std::move(state), std::move(version), std::move(advertised));
        if (attempt->error) {
            throw McpError(*attempt->error);
        }
        co_return std::move(*attempt->result);
    }

    static ConnectResult modern_result(const DiscoverResult& discover, const std::string& version) {
        ConnectResult connected;
        connected.era = ProtocolEra::eModern;
        connected.protocolVersion = version;
        connected.capabilities = discover.capabilities;
        connected.serverInfo = discover.serverInfo;
        connected.instructions = discover.instructions;
        connected.supportedVersions = discover.supportedVersions;
        return connected;
    }

    /// Settles on the era a DiscoverResult allows: modern when the server lists a modern version
    /// this client speaks, else legacy when it lists a legacy one, else the connect fails.
    static Task<ConnectResult> adopt_discover(std::shared_ptr<Impl> state, ProbePtr outcome,
                                              EraReason reason,
                                              std::chrono::steady_clock::time_point started) {
        const auto modern =
            newest_mutual(g_MODERN_PROTOCOL_VERSIONS, outcome->discover->supportedVersions);
        if (modern) {
            state->become_modern(*modern);
            state->report_era(ProtocolEra::eModern, reason, started);
            co_return modern_result(*outcome->discover, *modern);
        }
        const auto legacy =
            newest_mutual(g_LEGACY_PROTOCOL_VERSIONS, outcome->discover->supportedVersions);
        if (!legacy) {
            throw McpError(g_UNSUPPORTED_PROTOCOL_VERSION,
                           "The server shares no protocol version with this client",
                           nlohmann::json{{"supported", outcome->discover->supportedVersions}});
        }
        state->report_era(ProtocolEra::eLegacy, reason, started);
        co_return co_await initialize_session(
            state, legacy,
            std::make_shared<const std::vector<std::string>>(outcome->discover->supportedVersions));
    }

    /// Explains a probe that did not come back as a DiscoverResult to a caller that needs one.
    [[noreturn]] static void throw_probe_failure(const ProbeOutcome& outcome,
                                                 const std::string& version) {
        if (outcome.kind == ProbeOutcome::Kind::eModernError) {
            const auto advertised = advertised_versions(*outcome.error);
            if (outcome.error->code != g_UNSUPPORTED_PROTOCOL_VERSION || advertised.empty()) {
                throw McpError(*outcome.error);
            }
            std::string listed;
            for (const auto& entry : advertised) {
                listed += (listed.empty() ? "" : ", ") + detail::sanitize_for_diagnostics(entry);
            }
            throw McpError(g_UNSUPPORTED_PROTOCOL_VERSION,
                           "The server does not support protocol version " + version +
                               "; it supports " + listed +
                               ". Use ClientProtocol::Auto() to connect with a version both sides "
                               "speak",
                           outcome.error->data);
        }
        if (outcome.kind == ProbeOutcome::Kind::eClosed) {
            throw McpError(g_CONNECTION_CLOSED,
                           "The server closed the connection while the client probed it with "
                           "server/discover; a server that exits on an unknown request needs "
                           "ClientProtocol::Legacy()");
        }
        const std::string reason = outcome.error
                                       ? ": " + detail::sanitize_for_diagnostics(outcome.error->message)
                                       : " without a usable reply";
        throw McpError(outcome.error ? outcome.error->code : g_INVALID_REQUEST,
                       "The server does not speak protocol version " + version +
                           "; server/discover failed" + reason +
                           ". Use ClientProtocol::Auto() or ClientProtocol::Legacy() for a server "
                           "that predates it",
                       outcome.error ? outcome.error->data : std::nullopt);
    }

    /// ClientProtocol::Version("2026-07-28"): discover once for ConnectResult; never initialize.
    static Task<ConnectResult> connect_pinned_modern(std::shared_ptr<Impl> state,
                                                     std::chrono::steady_clock::time_point started) {
        auto version = std::make_shared<const std::string>(state->options.protocol.version());
        auto outcome = co_await probe(state, version, state->options.request_timeout);
        if (outcome->kind != ProbeOutcome::Kind::eDiscover) {
            throw_probe_failure(*outcome, *version);
        }
        state->become_modern(*version);
        state->report_era(ProtocolEra::eModern, EraReason::ePinned, started);
        co_return modern_result(*outcome->discover, *version);
    }

    /// ClientProtocol::Auto().
    static Task<ConnectResult> connect_auto(std::shared_ptr<Impl> state,
                                            std::chrono::steady_clock::time_point started) {
        const auto no_versions = std::make_shared<const std::vector<std::string>>();
        const auto preferred = std::make_shared<const std::string>(g_MODERN_PROTOCOL_VERSIONS.back());
        auto outcome = co_await probe(state, preferred, state->options.probe_timeout);
        auto reason = EraReason::eDiscoverOk;

        if (outcome->kind == ProbeOutcome::Kind::eModernError &&
            outcome->error->code == g_UNSUPPORTED_PROTOCOL_VERSION) {
            const auto advertised =
                std::make_shared<const std::vector<std::string>>(advertised_versions(*outcome->error));
            const auto modern = newest_mutual(g_MODERN_PROTOCOL_VERSIONS, *advertised);
            const auto legacy = newest_mutual(g_LEGACY_PROTOCOL_VERSIONS, *advertised);
            if (modern) {
                // The server is modern and names a version this client speaks: ask again with it.
                reason = EraReason::eModernErrorRetry;
                outcome = co_await probe(state, modern, state->options.probe_timeout);
            } else if (legacy) {
                state->report_era(ProtocolEra::eLegacy, EraReason::eModernErrorRetry, started);
                co_return co_await initialize_session(state, legacy, advertised);
            } else {
                throw McpError(g_UNSUPPORTED_PROTOCOL_VERSION,
                               "The server shares no protocol version with this client",
                               outcome->error->data);
            }
        }

        if (outcome->kind == ProbeOutcome::Kind::eDiscover) {
            co_return co_await adopt_discover(state, outcome, reason, started);
        }
        if (outcome->kind == ProbeOutcome::Kind::eModernError ||
            outcome->kind == ProbeOutcome::Kind::eClosed) {
            throw_probe_failure(*outcome, *preferred);
        }

        // Legacy: any other error, a reply that is not JSON, or silence.
        state->report_era(ProtocolEra::eLegacy,
                          outcome->kind == ProbeOutcome::Kind::eTimeout
                              ? EraReason::eFallbackTimeout
                              : EraReason::eFallbackNonModernError,
                          started);
        auto attempt = co_await attempt_initialize(
            state, std::make_shared<const std::string>(g_LATEST_LEGACY_PROTOCOL_VERSION), no_versions);
        if (attempt->result) {
            co_return std::move(*attempt->result);
        }

        // A server that answered nothing in time can still be a modern one that was starting up. If
        // it now refuses initialize naming modern versions, probe it again, once.
        if (lists_modern_version(*attempt->error)) {
            auto reprobe = co_await probe(state, preferred, state->options.probe_timeout);
            if (reprobe->kind == ProbeOutcome::Kind::eDiscover) {
                co_return co_await adopt_discover(state, reprobe,
                                                  EraReason::eReprobeAfterInitializeRejected, started);
            }
        }
        throw McpError(*attempt->error);
    }

    static Task<ConnectResult> connect_session(std::shared_ptr<Impl> state) {
        const auto started = std::chrono::steady_clock::now();
        const auto& protocol = state->options.protocol;
        const bool pinned_legacy = protocol.kind() == ClientProtocol::Kind::eLegacy ||
                                   (protocol.kind() == ClientProtocol::Kind::eVersion &&
                                    is_legacy_protocol_version(protocol.version()));
        try {
            if (pinned_legacy) {
                state->report_era(ProtocolEra::eLegacy, EraReason::ePinned, started);
                co_return co_await initialize_session(
                    state, std::make_shared<const std::string>(protocol.version()),
                    std::make_shared<const std::vector<std::string>>());
            }
            if (protocol.kind() == ClientProtocol::Kind::eVersion) {
                co_return co_await connect_pinned_modern(state, started);
            }
            co_return co_await connect_auto(state, started);
        } catch (...) {
            request_close(state, "Client initialization failed");
            throw;
        }
    }

    static Task<void> discard_result(Task<nlohmann::json> request_task) {
        static_cast<void>(co_await std::move(request_task));
    }

    static void start_read_loop(const std::shared_ptr<Impl>& state) {
        boost::asio::co_spawn(state->strand, read_loop(state), boost::asio::detached);
    }

    static void request_close(const std::shared_ptr<Impl>& state, std::string message) {
        if (state->closed.exchange(true, std::memory_order_acq_rel)) {
            return;
        }

        boost::asio::post(state->strand, [state, message = std::move(message)]() mutable {
            fail_pending_requests(state, Error{g_CONNECTION_CLOSED, std::move(message), std::nullopt});
            state->transport->close();
        });
    }

    static std::string request_correlation_key(int64_t id) {
        return RequestId{std::to_string(id)}.correlation_key();
    }

    static Task<nlohmann::json> send_request_wire(std::shared_ptr<Impl> state,
                                                  std::shared_ptr<const std::string> wire, int64_t id,
                                                  std::chrono::milliseconds timeout, bool probe) {
        if (state->closed.load(std::memory_order_acquire)) {
            throw McpError(g_CONNECTION_CLOSED, "Client transport is closed");
        }

        // [gcc11-sso: scope-before-await] The correlation key is a short std::string, so it is
        // built in this scope and rebuilt from `id` after the suspension rather than kept across it.
        PendingRequest* pending_request = nullptr;
        {
            auto id_key = request_correlation_key(id);
            {
                std::lock_guard lock(state->pending_requests_mutex);
                auto [iter, inserted] = state->pending_requests.try_emplace(
                    id_key, std::make_unique<PendingRequest>(state->strand, timeout));
                if (!inserted) {
                    throw McpError(g_INVALID_REQUEST, "Duplicate pending request id: " + id_key);
                }
                pending_request = iter->second.get();
                pending_request->probe = probe;
            }

            boost::asio::co_spawn(state->strand,
                                  state->writer.write_message(
                                      std::move(wire), pending_request->cancel_write_before_start),
                                  [state, id_key = std::move(id_key)](std::exception_ptr write_error) {
                                      complete_request_write(state, id_key, std::move(write_error));
                                  });
        }

        try {
            co_await pending_request->timer.async_wait(boost::asio::use_awaitable);
        } catch (const boost::system::system_error& error) {
            if (error.code() != boost::asio::error::operation_aborted) {
                erase_pending_request(state, request_correlation_key(id));
                throw;
            }
        }

        nlohmann::json result;
        std::optional<Error> error;
        std::exception_ptr write_error;
        bool timed_out = false;
        {
            const auto id_key = request_correlation_key(id);
            std::lock_guard lock(state->pending_requests_mutex);
            auto iter = state->pending_requests.find(id_key);
            if (iter == state->pending_requests.end()) {
                throw McpError(g_CONNECTION_CLOSED, "Pending request was removed: " + id_key);
            }
            if (!iter->second->completed) {
                iter->second->cancel_write_before_start->store(true, std::memory_order_release);
                iter->second->error = Error{g_REQUEST_TIMEOUT, "Request timed out",
                                            nlohmann::json{{"id", std::to_string(id)}}};
                iter->second->completed = true;
                timed_out = true;
            }
            result = std::move(iter->second->result);
            error = std::move(iter->second->error);
            write_error = std::move(iter->second->write_error);
            state->pending_requests.erase(iter);
        }

        // A 2026-07-28 request the client stops waiting for is cancelled, so the server stops
        // working on it and an HTTP transport closes its response stream. The era probe never gets
        // here: the client is not modern until a probe has been answered.
        if (timed_out && state->modern_version() && !state->closed.load(std::memory_order_acquire)) {
            CancelledNotificationParams cancel_params;
            cancel_params.requestId = RequestId{std::to_string(id)};
            cancel_params.reason = "Request timed out";
            JSONRPCNotification notification;
            notification.method = "notifications/cancelled";
            notification.params = nlohmann::json(std::move(cancel_params));
            auto cancel_wire =
                std::make_shared<const std::string>(nlohmann::json(std::move(notification)).dump());
            boost::asio::co_spawn(state->strand, send_notification_wire(state, std::move(cancel_wire)),
                                  [](const std::exception_ptr&) {});
        }

        if (error) {
            throw McpError(std::move(*error));
        }
        if (write_error) {
            std::rethrow_exception(write_error);
        }
        co_return result;
    }

    static Task<void> send_notification_wire(std::shared_ptr<Impl> state,
                                             std::shared_ptr<const std::string> wire) {
        if (state->closed.load(std::memory_order_acquire)) {
            throw McpError(g_CONNECTION_CLOSED, "Client transport is closed");
        }

        try {
            co_await state->writer.write_message(wire);
        } catch (...) {
            if (state->closed.load(std::memory_order_acquire)) {
                throw McpError(g_CONNECTION_CLOSED, "Client transport is closed");
            }
            throw;
        }
    }

    /// Two failure classes meet in this loop and only one is fatal. read_message() failing means the
    /// transport can no longer produce messages, so the session ends: pending requests fail and the
    /// transport closes; it is the only statement inside the outer try for that reason. A failure
    /// after it concerns one message already off the wire: it is reported through on_protocol_error
    /// and dropped, and the loop continues.
    static Task<void> read_loop(std::shared_ptr<Impl> state) {
        try {
            for (;;) {
                auto raw = co_await state->transport->read_message();
                dispatch_message(state, raw);
            }
        } catch (const std::exception& error) {
            state->closed.store(true, std::memory_order_release);
            fail_pending_requests(state, Error{g_CONNECTION_CLOSED,
                                               "Client read loop stopped: " +
                                                   detail::sanitize_for_diagnostics(error.what()),
                                               std::nullopt});
            state->transport->close();
        } catch (...) {
            state->closed.store(true, std::memory_order_release);
            fail_pending_requests(state,
                                  Error{g_CONNECTION_CLOSED, "Client read loop stopped", std::nullopt});
            state->transport->close();
        }
    }

    /// Decodes and routes one received message. Never throws: an unusable message is reported
    /// and dropped so that a single bad message cannot end the session.
    static void dispatch_message(const std::shared_ptr<Impl>& state, const std::string& raw) {
        try {
            if (detail::exceeds_json_depth(raw)) {
                throw McpError(g_PARSE_ERROR, "JSON-RPC message nests deeper than " +
                                                  std::to_string(detail::g_MAX_PEER_JSON_DEPTH) +
                                                  " levels");
            }
            auto json_message = nlohmann::json::parse(raw);

            if (!json_message.is_object()) {
                throw McpError(g_INVALID_REQUEST, "JSON-RPC message must be an object");
            }
            if (state->options.strict_protocol_validation) {
                if (!json_message.contains("jsonrpc") || !json_message.at("jsonrpc").is_string()) {
                    throw McpError(g_INVALID_REQUEST, "JSON-RPC message is missing jsonrpc");
                }
                detail::validate_jsonrpc_version(json_message.at("jsonrpc").get<std::string>());
            }

            const bool has_id = json_message.contains("id");
            const bool has_method = json_message.contains("method");
            if (has_method && !json_message.at("method").is_string()) {
                throw McpError(g_INVALID_REQUEST, "JSON-RPC method must be a string");
            }

            if (has_id && !has_method) {
                dispatch_response(state, json_message);
            } else if (has_id && has_method) {
                // dispatch_incoming_request is detached, so anything that throws inside it is
                // swallowed and the peer never hears back. Reject an unusable id here, where the
                // drop is reported, instead of leaving the request silently unanswered.
                const auto& id = json_message.at("id");
                if (!id.is_string() && !id.is_number_integer()) {
                    throw McpError(g_INVALID_REQUEST,
                                   "JSON-RPC request id must be a string or integer");
                }
                boost::asio::co_spawn(state->strand,
                                      dispatch_incoming_request(state, std::move(json_message)),
                                      boost::asio::detached);
            } else if (!has_id && has_method) {
                dispatch_notification(state, json_message);
            }
        } catch (const std::exception& error) {
            fail_probe(state);
            report_protocol_error(state, g_PARSE_ERROR,
                                  "Dropped an undecodable message from the peer: " +
                                      detail::sanitize_for_diagnostics(error.what()));
        } catch (...) {
            fail_probe(state);
            report_protocol_error(state, g_PARSE_ERROR, "Dropped an undecodable message from the peer");
        }
    }

    /// A line the client cannot decode while it waits for the era probe's answer is that answer: a
    /// server that replies to server/discover with plain text is not a modern one, and waiting out
    /// the probe timeout would only delay the fallback.
    static void fail_probe(const std::shared_ptr<Impl>& state) {
        std::lock_guard lock(state->pending_requests_mutex);
        for (auto& [id, pending_request] : state->pending_requests) {
            static_cast<void>(id);
            if (pending_request->probe && !pending_request->completed) {
                pending_request->error =
                    Error{g_PARSE_ERROR, "Reply to server/discover is not JSON-RPC"};
                pending_request->completed = true;
                signal_pending_request(*pending_request);
            }
        }
    }

    static void report_protocol_error(const std::shared_ptr<Impl>& state, int code,
                                      std::string message) {
        if (!state->options.on_protocol_error) {
            return;
        }
        try {
            state->options.on_protocol_error(Error{code, std::move(message), std::nullopt});
        } catch (...) {
            // The hook exists to report failures; a failure of the hook itself has no listener.
        }
    }

    static void dispatch_response(const std::shared_ptr<Impl>& state,
                                  const nlohmann::json& json_message) {
        const auto& id = json_message.at("id");
        if (!id.is_string() && !id.is_number_integer()) {
            return;
        }
        const auto id_key = id.get<RequestId>().correlation_key();

        std::unique_lock lock(state->pending_requests_mutex);
        auto iter = state->pending_requests.find(id_key);
        if (iter == state->pending_requests.end()) {
            lock.unlock();
            end_subscription_from_response(state, id_key, json_message);
            return;
        }
        if (std::chrono::steady_clock::now() >= iter->second->deadline) {
            return;
        }

        // Asymmetric on purpose: a null `result` is a legitimate empty result, so presence is the
        // right test for it, while a null `error` is the absence of an error and must not count.
        const bool has_error = detail::has_json_value(json_message, "error");
        const bool has_result = json_message.contains("result");
        if (has_error == has_result) {
            iter->second->error =
                Error{g_INVALID_REQUEST,
                      "JSON-RPC response must contain exactly one of result or error", std::nullopt};
        } else if (has_error) {
            // An undecodable `error` member still identifies the request it answers. Fail that
            // request now rather than drop the response and leave the caller waiting out its
            // deadline for an answer that has already arrived.
            try {
                iter->second->error = json_message.at("error").get<Error>();
            } catch (const std::exception& decode_error) {
                iter->second->error = Error{g_INVALID_REQUEST,
                                            "Malformed JSON-RPC error object: " +
                                                detail::sanitize_for_diagnostics(decode_error.what()),
                                            std::nullopt};
            }
        } else {
            iter->second->result = json_message.at("result");
        }

        iter->second->completed = true;
        signal_pending_request(*iter->second);
    }

    static void dispatch_notification(const std::shared_ptr<Impl>& state,
                                      const nlohmann::json& json_message) {
        const auto method = json_message.at("method").get<std::string>();
        if (method == "notifications/cancelled" && end_subscription_from_cancel(state, json_message)) {
            return;
        }
        route_to_subscription(state, method, json_message);
        if (method == "notifications/cancelled") {
            if (detail::has_json_value(json_message, "params")) {
                auto params = json_message.at("params").get<CancelledNotificationParams>();
                const auto id_key = params.requestId.correlation_key();

                std::lock_guard lock(state->pending_requests_mutex);
                auto iter = state->pending_requests.find(id_key);
                if (iter != state->pending_requests.end() &&
                    std::chrono::steady_clock::now() < iter->second->deadline) {
                    iter->second->error = Error{g_REQUEST_CANCELLED, "Request cancelled by server"};
                    iter->second->completed = true;
                    signal_pending_request(*iter->second);
                }
            }
            return;
        }

        NotificationCallback callback;
        {
            std::lock_guard lock(state->handlers_mutex);
            auto iter = state->notification_handlers.find(method);
            if (iter != state->notification_handlers.end()) {
                callback = iter->second;
            }
        }
        if (callback) {
            auto params = detail::has_json_value(json_message, "params") ? json_message.at("params")
                                                                         : nlohmann::json::object();
            // The callback is application code running on the read loop. Letting it throw past
            // here would end the session over a bug the application could otherwise handle, so
            // the failure is reported against its method and the session carries on.
            try {
                callback(params);
            } catch (const std::exception& error) {
                report_protocol_error(state, g_INTERNAL_ERROR,
                                      "Notification callback for " +
                                          detail::sanitize_for_diagnostics(method) +
                                          " threw: " + detail::sanitize_for_diagnostics(error.what()));
            } catch (...) {
                report_protocol_error(
                    state, g_INTERNAL_ERROR,
                    "Notification callback for " + detail::sanitize_for_diagnostics(method) + " threw");
            }
        }
    }

    static Task<void> dispatch_incoming_request(std::shared_ptr<Impl> state,
                                                nlohmann::json json_message) {
        std::string_view method = json_message.at("method").get_ref<const nlohmann::json::string_t&>();

        RequestHandler request_handler;
        {
            std::lock_guard lock(state->handlers_mutex);
            auto iter = state->request_handlers.find(method);
            if (iter != state->request_handlers.end()) {
                request_handler = iter->second;
            }
        }

        if (request_handler) {
            auto params = detail::has_json_value(json_message, "params") ? json_message.at("params")
                                                                         : nlohmann::json::object();
            nlohmann::json error_payload;
            nlohmann::json result;
            try {
                result = co_await request_handler(params);
            } catch (const std::exception& error) {
                error_payload = error.what();
            } catch (...) {
                error_payload = "Request handler failed";
            }

            if (!error_payload.is_null()) {
                co_await state->writer.write_message(
                    make_error_wire(json_message.at("id").get<RequestId>(), g_INTERNAL_ERROR,
                                    error_payload.get<std::string>()));
            } else {
                co_await state->writer.write_message(
                    make_result_wire(json_message.at("id").get<RequestId>(), std::move(result)));
            }
        } else if (method == "ping") {
            co_await state->writer.write_message(
                make_result_wire(json_message.at("id").get<RequestId>(), nlohmann::json::object()));
        } else {
            co_await state->writer.write_message(
                make_error_wire(json_message.at("id").get<RequestId>(), g_METHOD_NOT_FOUND,
                                "Method not found: " + std::string(method)));
        }
    }

    static std::string make_result_wire(const RequestId& id, nlohmann::json result) {
        JSONRPCResultResponse response;
        response.id = id;
        response.result = std::move(result);
        return nlohmann::json(std::move(response)).dump();
    }

    static std::string make_error_wire(const RequestId& id, int code, std::string message) {
        Error error;
        error.code = code;
        error.message = std::move(message);
        JSONRPCErrorResponse response;
        response.id = id;
        response.error = std::move(error);
        return nlohmann::json(std::move(response)).dump();
    }

    static void signal_pending_request(PendingRequest& pending_request) {
        pending_request.timer.expires_at(std::chrono::steady_clock::now());
    }

    static void complete_request_write(const std::shared_ptr<Impl>& state, const std::string& id_key,
                                       std::exception_ptr write_error) {
        if (!write_error) {
            return;
        }

        std::lock_guard lock(state->pending_requests_mutex);
        auto iter = state->pending_requests.find(id_key);
        if (iter == state->pending_requests.end() || iter->second->completed ||
            std::chrono::steady_clock::now() >= iter->second->deadline) {
            return;
        }

        if (state->closed.load(std::memory_order_acquire)) {
            iter->second->error = Error{g_CONNECTION_CLOSED, "Client transport is closed"};
        } else {
            iter->second->write_error = std::move(write_error);
        }
        iter->second->completed = true;
        signal_pending_request(*iter->second);
    }

    static void erase_pending_request(const std::shared_ptr<Impl>& state, const std::string& id_key) {
        std::lock_guard lock(state->pending_requests_mutex);
        state->pending_requests.erase(id_key);
    }

    static void fail_pending_requests(const std::shared_ptr<Impl>& state, const Error& error) {
        end_subscriptions(state, error);
        std::lock_guard lock(state->pending_requests_mutex);
        for (auto& [id, pending_request] : state->pending_requests) {
            static_cast<void>(id);
            if (!pending_request->completed) {
                pending_request->error = error;
                pending_request->completed = true;
            }
            signal_pending_request(*pending_request);
        }
    }

    std::shared_ptr<ITransport> transport;
    boost::asio::strand<boost::asio::any_io_executor> strand;
    ClientOptions options;
    detail::SerializedTransportWriter writer;

    mutable std::mutex pending_requests_mutex;
    std::map<std::string, std::unique_ptr<PendingRequest>, std::less<>> pending_requests;
    std::atomic<int64_t> next_request_id{1};
    std::atomic<bool> read_loop_started{false};
    std::atomic<bool> closed{false};

    /// The open subscriptions/listen streams by request id; the answer to such a request ends the
    /// stream, so these never sit in pending_requests.
    mutable std::mutex subscriptions_mutex;
    std::map<std::string, std::shared_ptr<detail::SubscriptionState>, std::less<>> subscriptions;

    mutable std::mutex handlers_mutex;
    std::map<std::string, NotificationCallback, std::less<>> notification_handlers;
    std::map<std::string, RequestHandler, std::less<>> request_handlers;
    std::vector<Root> roots;
    ClientCapabilities client_capabilities;
    Implementation client_info;

    /// The version every request names once connect() settled on the modern era; empty while the
    /// session is legacy or not yet connected. It is decided once and never changes.
    mutable std::mutex era_mutex;
    std::optional<std::string> modern_protocol_version;

    void become_modern(const std::string& version) {
        std::lock_guard lock(era_mutex);
        modern_protocol_version = version;
    }

    std::optional<std::string> modern_version() const {
        std::lock_guard lock(era_mutex);
        return modern_protocol_version;
    }

    /// What this client tells a modern server it can do: the capabilities given to connect(), plus
    /// what its registered handlers imply. Read afresh for every request, so a handler registered
    /// after connect() counts from the next request on.
    ClientCapabilities effective_capabilities() const {
        std::lock_guard lock(handlers_mutex);
        auto capabilities = client_capabilities;
        if (request_handlers.contains("roots/list") && !capabilities.roots) {
            capabilities.roots = ClientCapabilities::RootsCapability{};
        }
        if (request_handlers.contains("sampling/createMessage") && !capabilities.sampling) {
            capabilities.sampling = ClientCapabilities::SamplingCapability{};
        }
        if (request_handlers.contains("elicitation/create") && !capabilities.elicitation) {
            capabilities.elicitation =
                ClientCapabilities::ElicitationCapability{nlohmann::json::object(), std::nullopt};
        }
        return capabilities;
    }

    /// @p params with the per-request `_meta` of a modern request merged in. The caller's own
    /// `_meta` entries (progressToken, traceparent, ...) stay as they are; the four protocol keys
    /// are the client's to set.
    std::optional<nlohmann::json> with_request_meta(const std::string& version,
                                                    const std::optional<nlohmann::json>& params,
                                                    const std::optional<LoggingLevel>& level) const {
        nlohmann::json merged = params ? *params : nlohmann::json::object();
        if (merged.is_null()) {
            merged = nlohmann::json::object();
        }
        if (!merged.is_object()) {
            throw McpError(g_INVALID_PARAMS, "Request params must be an object");
        }
        nlohmann::json meta = nlohmann::json::object();
        if (const auto found = merged.find("_meta"); found != merged.end() && !found->is_null()) {
            if (!found->is_object()) {
                throw McpError(g_INVALID_PARAMS, "Request _meta must be an object");
            }
            meta = *found;
        }
        meta[std::string(g_META_PROTOCOL_VERSION)] = version;
        meta[std::string(g_META_CLIENT_CAPABILITIES)] = effective_capabilities();
        {
            std::lock_guard lock(handlers_mutex);
            meta[std::string(g_META_CLIENT_INFO)] = client_info;
        }
        if (level) {
            meta[std::string(g_META_LOG_LEVEL)] = *level;
        }
        merged["_meta"] = std::move(meta);
        return merged;
    }

    /// What the client knows of a listed tool's schemas. The serialized schemas let a later
    /// listing reuse the compiled output schema when nothing changed.
    struct ListedTool {
        std::string input_text;
        std::optional<std::string> output_text;
        std::optional<CompiledSchema> output_schema;
    };
    mutable std::mutex listed_tools_mutex;
    std::map<std::string, ListedTool, std::less<>> listed_tools;

    /// The output schema to check a call of @p name against, if the client has one.
    std::optional<CompiledSchema> output_schema_for(std::string_view name) const {
        if (!options.validate_tool_schemas) {
            return std::nullopt;
        }
        std::lock_guard lock(listed_tools_mutex);
        const auto found = listed_tools.find(name);
        if (found == listed_tools.end()) {
            return std::nullopt;
        }
        return found->second.output_schema;
    }

    /// Compiles @p tool's schemas and remembers its output schema; the problem when a schema cannot
    /// be used.
    std::optional<SchemaError> remember_tool(const Tool& tool) {
        ListedTool listed{
            tool.inputSchema.dump(),
            tool.outputSchema ? std::optional<std::string>(tool.outputSchema->dump()) : std::nullopt,
            std::nullopt};
        {
            std::lock_guard lock(listed_tools_mutex);
            const auto found = listed_tools.find(tool.name);
            if (found != listed_tools.end() && found->second.input_text == listed.input_text &&
                found->second.output_text == listed.output_text) {
                return std::nullopt;
            }
        }
        std::string_view field = "inputSchema";
        try {
            (void)CompiledSchema::compile(tool.inputSchema);
            field = "outputSchema";
            if (tool.outputSchema) {
                listed.output_schema = CompiledSchema::compile(*tool.outputSchema);
            }
        } catch (const SchemaError& error) {
            std::lock_guard lock(listed_tools_mutex);
            listed_tools.erase(tool.name);
            return SchemaError(error.category(), std::string(field) + ": " + error.what());
        }
        std::lock_guard lock(listed_tools_mutex);
        listed_tools.insert_or_assign(tool.name, std::move(listed));
        return std::nullopt;
    }

    /// Leaves out of @p result the tools whose schemas cannot be used, reporting each.
    void accept_listed_tools(ListToolsResult& result) {
        if (!options.validate_tool_schemas) {
            return;
        }
        std::vector<Tool> usable;
        usable.reserve(result.tools.size());
        for (auto& tool : result.tools) {
            auto problem = remember_tool(tool);
            if (!problem) {
                usable.push_back(std::move(tool));
                continue;
            }
            if (options.on_tool_schema_error) {
                try {
                    options.on_tool_schema_error(tool, *problem);
                } catch (...) {
                    // Documented: the callback reports; a failure of its own has no listener.
                }
            }
        }
        result.tools = std::move(usable);
    }

    static Task<ListToolsResult> accepted_tools(std::shared_ptr<Impl> state,
                                                Task<ListToolsResult> request) {
        auto result = co_await std::move(request);
        state->accept_listed_tools(result);
        co_return result;
    }
};

Client::Client(std::shared_ptr<ITransport> transport, const boost::asio::any_io_executor& executor,
               ClientOptions options) {
    if (!transport) {
        throw std::invalid_argument("Client transport must not be null");
    }
    if (options.request_timeout <= std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("Client request timeout must be positive");
    }
    if (options.probe_timeout <= std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("Client probe timeout must be positive");
    }
    if (options.subscription_queue_limit == 0) {
        throw std::invalid_argument("Client subscription queue limit must be positive");
    }
    impl_ = std::make_shared<Impl>(std::move(transport), executor, std::move(options));
}

Client::~Client() { close(); }

Task<ConnectResult> Client::connect(std::string_view name, std::string_view version) {
    Implementation info;
    info.name = std::string(name);
    info.version = std::string(version);
    return connect(info, {});
}

Task<ConnectResult> Client::connect(const Implementation& client_info,
                                    const ClientCapabilities& capabilities) {
    auto state = impl_;
    if (state->closed.load(std::memory_order_acquire)) {
        throw McpError(g_CONNECTION_CLOSED, "Client is closed");
    }
    if (state->read_loop_started.exchange(true, std::memory_order_acq_rel)) {
        throw McpError(g_INVALID_REQUEST, "Client has already been initialized");
    }

    Impl::start_read_loop(state);
    {
        std::lock_guard lock(state->handlers_mutex);
        state->client_info = client_info;
        state->client_capabilities = capabilities;
        if (state->request_handlers.contains("roots/list") &&
            !state->client_capabilities.roots.has_value()) {
            state->client_capabilities.roots = ClientCapabilities::RootsCapability{};
        }
    }
    return Impl::connect_session(std::move(state));
}

Task<nlohmann::json> Client::send_request(std::string_view method,
                                          const std::optional<nlohmann::json>& params) {
    return send_request(method, params, {});
}

Task<nlohmann::json> Client::send_request(std::string_view method,
                                          const std::optional<nlohmann::json>& params,
                                          const RequestOptions& request_options) {
    if (!impl_->read_loop_started.load(std::memory_order_acquire)) {
        throw McpError(g_INVALID_REQUEST, "Client must connect before sending requests");
    }
    return Impl::request(impl_, method, params, request_options);
}

Task<Subscription> Client::listen(const SubscriptionFilter& filter) {
    return listen(filter, RequestOptions{});
}

Task<Subscription> Client::listen(const SubscriptionFilter& filter,
                                  const RequestOptions& request_options) {
    if (!impl_->read_loop_started.load(std::memory_order_acquire)) {
        throw McpError(g_INVALID_REQUEST, "Client must connect before sending requests");
    }
    return Impl::listen(impl_, filter, request_options);
}

Task<void> Client::send_notification(std::string_view method,
                                     const std::optional<nlohmann::json>& params) {
    if (!impl_->read_loop_started.load(std::memory_order_acquire)) {
        throw McpError(g_INVALID_REQUEST, "Client must connect before sending notifications");
    }
    return Impl::notification(impl_, method, params);
}

std::size_t Client::pending_request_count() const {
    std::lock_guard lock(impl_->pending_requests_mutex);
    return impl_->pending_requests.size();
}

void Client::close() {
    if (impl_) {
        Impl::request_close(impl_, "Client transport closed");
    }
}

Task<ListToolsResult> Client::list_tools(const std::optional<std::string>& cursor) {
    // [gcc11-sso: not-a-coroutine]
    return Impl::accepted_tools(
        impl_, call_and_parse<ListToolsResult>("tools/list", make_paginated_params(cursor)));
}

Task<CallToolResult> Client::call_tool_with(CallToolParams params) {
    // [gcc11-sso: not-a-coroutine]
    auto output_schema = impl_->output_schema_for(params.name);
    return checked_call_result(send_request("tools/call", nlohmann::json(std::move(params))),
                               std::move(output_schema));
}

Task<ListResourcesResult> Client::list_resources(const std::optional<std::string>& cursor) {
    return call_and_parse<ListResourcesResult>("resources/list", make_paginated_params(cursor));
}

Task<ReadResourceResult> Client::read_resource(const std::string& uri) {
    ReadResourceRequestParams params;
    params.uri = uri;
    return call_and_parse<ReadResourceResult>("resources/read", nlohmann::json(std::move(params)));
}

Task<ListResourceTemplatesResult> Client::list_resource_templates(
    const std::optional<std::string>& cursor) {
    return call_and_parse<ListResourceTemplatesResult>("resources/templates/list",
                                                       make_paginated_params(cursor));
}

Task<ListPromptsResult> Client::list_prompts(const std::optional<std::string>& cursor) {
    return call_and_parse<ListPromptsResult>("prompts/list", make_paginated_params(cursor));
}

Task<GetPromptResult> Client::get_prompt(
    const std::string& name, const std::optional<std::map<std::string, std::string>>& arguments) {
    GetPromptRequestParams params;
    params.name = name;
    params.arguments = arguments;
    return call_and_parse<GetPromptResult>("prompts/get", nlohmann::json(std::move(params)));
}

Task<CompleteResult> Client::complete(const CompleteParams& params) {
    return call_and_parse<CompleteResult>("completion/complete", nlohmann::json(params));
}

Task<void> Client::ping() {
    if (!impl_->read_loop_started.load(std::memory_order_acquire)) {
        throw McpError(g_INVALID_REQUEST, "Client must connect before sending requests");
    }
    return Impl::discard_result(Impl::request(impl_, "ping", std::nullopt, {}));
}

Task<void> Client::cancel(const RequestId& request_id, const std::optional<std::string>& reason) {
    // [gcc11-sso: not-a-coroutine]
    if (impl_->modern_version()) {
        // A 2026-07-28 server sends nothing more for a cancelled request, so the caller stops
        // waiting now, and a response that arrives anyway finds no request and is dropped.
        std::lock_guard lock(impl_->pending_requests_mutex);
        const auto iter = impl_->pending_requests.find(request_id.correlation_key());
        if (iter != impl_->pending_requests.end() && !iter->second->completed) {
            iter->second->cancel_write_before_start->store(true, std::memory_order_release);
            iter->second->error = Error{g_REQUEST_CANCELLED, "Request cancelled"};
            iter->second->completed = true;
            Impl::signal_pending_request(*iter->second);
        }
    }
    CancelledNotificationParams params;
    params.requestId = request_id;
    params.reason = reason;
    return send_notification("notifications/cancelled", nlohmann::json(std::move(params)));
}

void Client::on_notification(const std::string& method, NotificationCallback callback) {
    std::lock_guard lock(impl_->handlers_mutex);
    impl_->notification_handlers[method] = std::move(callback);
}

void Client::on_progress(ProgressCallback callback) {
    on_notification(
        "notifications/progress", [callback = std::move(callback)](const nlohmann::json& params) {
            ProgressNotificationParams progress;
            try {
                params.get_to(progress);
            } catch (const std::exception& error) {
                // Reported through ClientOptions::on_protocol_error by the read loop.
                // Naming the cause here separates a notification the peer sent wrong
                // from a bug in the application's own progress callback.
                throw McpError(g_INVALID_PARAMS, "Malformed progress notification params: " +
                                                     detail::sanitize_for_diagnostics(error.what()));
            }
            callback(progress);
        });
}

void Client::on_request(const std::string& method, RequestHandler handler) {
    std::lock_guard lock(impl_->handlers_mutex);
    impl_->request_handlers[method] = std::move(handler);
}

void Client::on_elicitation(std::function<Task<ElicitResult>(const ElicitRequestParams&)> handler) {
    on_request("elicitation/create",
               [handler = std::move(handler)](const nlohmann::json& params) -> Task<nlohmann::json> {
                   auto result = co_await handler(params.get<ElicitRequestParams>());
                   co_return nlohmann::json(std::move(result));
               });
}

void Client::set_roots(const std::vector<Root>& roots, bool notify) {
    auto state = impl_;
    bool should_notify = false;
    {
        std::lock_guard lock(state->handlers_mutex);
        state->roots = roots;
        if (!state->request_handlers.contains("roots/list")) {
            std::weak_ptr<Impl> weak_state = state;
            state->request_handlers.emplace(
                "roots/list", [weak_state](const nlohmann::json&) -> Task<nlohmann::json> {
                    auto active_state = weak_state.lock();
                    if (!active_state) {
                        throw McpError(g_CONNECTION_CLOSED, "Client is closed");
                    }

                    ListRootsResult result;
                    {
                        std::lock_guard roots_lock(active_state->handlers_mutex);
                        result.roots = active_state->roots;
                    }
                    co_return nlohmann::json(std::move(result));
                });
        }

        should_notify = notify && state->client_capabilities.roots.has_value() &&
                        state->client_capabilities.roots->listChanged.value_or(false);
    }

    // A modern session has no roots notification: the server learns the roots from each request.
    if (should_notify && !state->modern_version() && !state->closed.load(std::memory_order_acquire)) {
        boost::asio::co_spawn(
            state->strand, Impl::notification(state, "notifications/roots/list_changed", std::nullopt),
            boost::asio::detached);
    }
}

void Client::on_roots_list(std::function<Task<ListRootsResult>(const nlohmann::json&)> handler) {
    on_request("roots/list",
               [handler = std::move(handler)](const nlohmann::json& params) -> Task<nlohmann::json> {
                   auto result = co_await handler(params);
                   co_return nlohmann::json(std::move(result));
               });
}

}  // namespace mcp

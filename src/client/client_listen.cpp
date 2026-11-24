/// @file client_listen.cpp
/// @brief Client::Impl's subscriptions/listen streams: opening one and routing what ends it.

#include "client_impl.hpp"

namespace mcp {

Task<Subscription> Client::Impl::listen(std::shared_ptr<Impl> state, const SubscriptionFilter& filter,
                                        const RequestOptions& request_options) {
    const auto modern_version = state->modern_version();
    if (!modern_version) {
        throw McpError(mcp::constants::jsonrpc::g_METHOD_NOT_FOUND,
                       "subscriptions/listen needs a session of protocol version 2026-07-28 or "
                       "later");
    }
    const auto timeout = request_options.timeout.value_or(state->options.request_timeout);
    if (timeout <= std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("Request timeout must be positive");
    }

    nlohmann::json params = nlohmann::json::object();
    params["notifications"] = filter;
    const auto wire_params = state->with_request_meta(
        *modern_version, params, request_options.log_level, !filter.taskIds.empty());

    const int64_t id = state->next_request_id.fetch_add(1, std::memory_order_relaxed);
    JSONRPCRequest request;
    request.id = RequestId{std::to_string(id)};
    request.method = methods::g_SUBSCRIPTIONS_LISTEN;
    request.params = wire_params;
    auto wire =
        std::make_shared<const std::string>(mcp::detail::dump_json(nlohmann::json(std::move(request))));

    auto stream = std::make_shared<detail::SubscriptionState>(state->strand,
                                                              state->options.subscription_queue_limit);
    stream->blocking_loop = state->blocking_loop;
    stream->id = RequestId{std::to_string(id)};
    stream->requested = filter;
    std::weak_ptr<Impl> weak_state = state;
    const auto key = std::make_shared<const std::string>(request_correlation_key(id));
    stream->forget = [weak_state, key] {
        if (auto active = weak_state.lock()) {
            std::scoped_lock lock(active->subscriptions_mutex);
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
        return notification(std::move(active), methods::g_NOTIFICATIONS_CANCELLED, cancel_params);
    };

    auto strand = state->strand;
    return detail::spawn_awaitable(
        strand, listen_on_strand(std::move(state), std::move(stream), std::move(wire), timeout));
}

Task<Subscription> Client::Impl::listen_on_strand(std::shared_ptr<Impl> state,
                                                  std::shared_ptr<detail::SubscriptionState> stream,
                                                  std::shared_ptr<const std::string> wire,
                                                  std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    {
        std::scoped_lock lock(state->subscriptions_mutex);
        state->subscriptions.emplace(stream->id.correlation_key(), stream);
    }
    // The stream is registered before the closed check, so a connection that ends from here on
    // either shows as closed or finds the stream and ends it.
    if (state->closed.load(std::memory_order_acquire)) {
        stream->forget();
        throw McpError(mcp::constants::jsonrpc::g_CONNECTION_CLOSED, "Client transport is closed");
    }

    // Not awaited: on HTTP the write lasts until the answer starts, and the deadline covers that.
    auto write_failure = std::make_shared<std::exception_ptr>();
    boost::asio::co_spawn(state->strand,
                          state->writes_beside_queue() ? state->writer.write_overlapping(wire)
                                                       : state->writer.write_message(wire),
                          [stream, write_failure](std::exception_ptr failure) {
                              if (failure) {
                                  *write_failure = std::move(failure);
                                  stream->wake.cancel();
                              }
                          });

    for (;;) {
        if (*write_failure) {
            stream->forget();
            if (state->closed.load(std::memory_order_acquire)) {
                throw McpError(mcp::constants::jsonrpc::g_CONNECTION_CLOSED,
                               "Client transport is closed");
            }
            throw_transport_failure(*write_failure);
        }
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
            std::scoped_lock lock(stream->mutex);
            failure = stream->failure;
        }
        if (failure) {
            throw McpError(std::move(*failure));
        }
        throw McpError(mcp::constants::jsonrpc::g_INTERNAL_ERROR,
                       "subscriptions/listen ended before it was acknowledged");
    }

    // No acknowledgment in time: end the stream here and tell the server, best effort.
    stream->finish(SubscriptionEnd::eCancelled, std::nullopt);
    stream->forget();
    try {
        co_await stream->send_cancel();
    } catch (...) {
        // The request timeout below is the failure to report.
    }
    throw McpError(mcp::constants::jsonrpc::g_REQUEST_TIMEOUT,
                   "subscriptions/listen was not acknowledged in time");
}

void Client::Impl::route_to_subscription(const std::shared_ptr<Impl>& state, const std::string& method,
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
    const auto named = meta->find(std::string(mcp::constants::meta::g_META_SUBSCRIPTION_ID));
    if (named == meta->end() || !detail::is_request_id(*named)) {
        return;
    }
    std::shared_ptr<detail::SubscriptionState> stream;
    {
        std::scoped_lock lock(state->subscriptions_mutex);
        const auto found = state->subscriptions.find(named->get<RequestId>().correlation_key());
        if (found == state->subscriptions.end()) {
            return;
        }
        stream = found->second;
    }
    if (method == methods::g_NOTIFICATIONS_SUBSCRIPTIONS_ACKNOWLEDGED) {
        stream->acknowledge(honored_filter(params));
        return;
    }
    stream->push(SubscriptionEvent{.method = method, .params = params});
}

void Client::Impl::end_subscription_from_response(const std::shared_ptr<Impl>& state,
                                                  const std::string& id_key,
                                                  const nlohmann::json& json_message) {
    std::shared_ptr<detail::SubscriptionState> stream;
    {
        std::scoped_lock lock(state->subscriptions_mutex);
        const auto found = state->subscriptions.find(id_key);
        if (found == state->subscriptions.end()) {
            return;
        }
        stream = found->second;
        state->subscriptions.erase(found);
    }
    if (detail::has_json_value(json_message, "error")) {
        try {
            // HttpClientTransport reports a stream that broke as an answer with this code.
            auto error = json_message.at("error").get<Error>();
            const auto end = error.code == mcp::constants::jsonrpc::g_CONNECTION_CLOSED
                                 ? SubscriptionEnd::eDropped
                                 : SubscriptionEnd::eFailed;
            stream->finish(end, std::move(error));
        } catch (const std::exception& decode_error) {
            stream->finish(SubscriptionEnd::eFailed,
                           Error{
                               .code = mcp::constants::jsonrpc::g_INVALID_REQUEST,
                               .message = "Malformed JSON-RPC error object: " +
                                          detail::sanitize_for_diagnostics(decode_error.what()),
                               .data = std::nullopt,
                           });
        }
        return;
    }
    stream->finish(SubscriptionEnd::eServerClosed, std::nullopt);
}

bool Client::Impl::end_subscription_from_cancel(const std::shared_ptr<Impl>& state,
                                                const nlohmann::json& json_message) {
    if (!detail::has_json_value(json_message, "params")) {
        return false;
    }
    const auto& params = json_message.at("params");
    const auto named = params.is_object() ? params.find("requestId") : params.end();
    if (named == params.end() || !detail::is_request_id(*named)) {
        return false;
    }
    std::shared_ptr<detail::SubscriptionState> stream;
    {
        std::scoped_lock lock(state->subscriptions_mutex);
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

void Client::Impl::end_subscriptions(const std::shared_ptr<Impl>& state, const Error& error) {
    std::map<std::string, std::shared_ptr<detail::SubscriptionState>, std::less<>> open;
    {
        std::scoped_lock lock(state->subscriptions_mutex);
        open.swap(state->subscriptions);
    }
    for (auto& [key, stream] : open) {
        static_cast<void>(key);
        stream->finish(SubscriptionEnd::eDropped, error);
    }
}

}  // namespace mcp

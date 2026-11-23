/// @file client_session.cpp
/// @brief The request and response path of Client::Impl: sending requests and notifications, the read
/// loop and the dispatch of what the server sends, and the pending requests.

#include "client_impl.hpp"

namespace mcp {

Task<nlohmann::json> Client::Impl::request(std::shared_ptr<Impl> state, std::string_view method,
                                           const std::optional<nlohmann::json>& params,
                                           const RequestOptions& request_options, bool declare_tasks,
                                           std::shared_ptr<bool> answered_input) {
    if (request_options.on_progress) {
        return request_with_progress(std::move(state), std::make_shared<const std::string>(method),
                                     params.value_or(nlohmann::json()), request_options, declare_tasks,
                                     std::move(answered_input));
    }
    const auto modern_version = state->modern_version();
    if (!modern_version) {
        return request_wire(std::move(state), method, params, request_options, false);
    }
    reject_removed_method(*modern_version, method);
    declare_tasks = declare_tasks || method.starts_with("tasks/");
    if (supports_input_required(method)) {
        return resume_input_required(std::move(state), std::make_shared<const std::string>(method),
                                     params, request_options,
                                     std::make_shared<const std::string>(*modern_version),
                                     declare_tasks, std::move(answered_input));
    }
    auto modern_params =
        state->with_request_meta(*modern_version, params, request_options.log_level, declare_tasks);
    return checked_result(
        request_wire(std::move(state), method, modern_params, request_options, false));
}

Task<nlohmann::json> Client::Impl::cacheable_request(std::shared_ptr<Impl> state,
                                                     std::string_view method,
                                                     std::optional<nlohmann::json> params,
                                                     const RequestOptions& request_options) {
    if (!state->read_loop_started.load(std::memory_order_acquire)) {
        throw McpError(mcp::constants::jsonrpc::g_INVALID_REQUEST,
                       "Client must connect before sending requests");
    }
    // A closed client fails the call as it would without the cache.
    if (!state->response_cache.enabled() || request_options.cache_mode == CacheMode::eBypass ||
        state->closed.load(std::memory_order_acquire)) {
        return request(std::move(state), method, params, request_options);
    }
    return cached_request(std::move(state), std::make_shared<const std::string>(method),
                          std::move(params), request_options);
}

Task<nlohmann::json> Client::Impl::cached_request(std::shared_ptr<Impl> state,
                                                  std::shared_ptr<const std::string> method,
                                                  std::optional<nlohmann::json> params,
                                                  RequestOptions request_options) {
    auto& cache = state->response_cache;
    // On the heap: GCC 11 mishandles a std::string that lives across a suspension point.
    const auto key = std::make_shared<const std::string>(detail::ResponseCache::key(*method, params));
    if (request_options.cache_mode == CacheMode::eUse) {
        auto kept = cache.find(*key);
        if (kept) {
            co_return std::move(*kept);
        }
    }
    const auto generation = cache.generation();
    auto answered_input = std::make_shared<bool>(false);
    nlohmann::json reply;
    try {
        reply = co_await request(state, *method, params, request_options, false, answered_input);
    } catch (const McpError& error) {
        // A refused cursor means the pages kept so far may not fit together any more; the
        // next call starts the list again from the first page.
        if (error.code() == mcp::constants::jsonrpc::g_INVALID_PARAMS && params &&
            params->is_object() && params->contains("cursor")) {
            cache.forget(*method);
        }
        throw;
    }
    // A result that depends on the answers to the server's questions is not one the method and
    // parameters identify.
    if (!*answered_input) {
        cache.store(*key, *method, params, reply, generation);
    }
    co_return reply;
}

Client::Impl::ProgressRoute Client::Impl::route_progress(const std::shared_ptr<Impl>& state,
                                                         nlohmann::json& params,
                                                         RequestOptions& request_options) {
    ProgressRoute ended;
    if (!request_options.on_progress) {
        return ended;
    }
    auto route = std::make_shared<ProgressRouteState>(state->options.notification_rate_limit);
    route->callback = std::move(request_options.on_progress);
    request_options.on_progress = nullptr;
    if (params.is_null()) {
        params = nlohmann::json::object();
    }
    if (!params.is_object()) {
        throw McpError(mcp::constants::jsonrpc::g_INVALID_PARAMS, "Request params must be an object");
    }
    auto& meta = params["_meta"];
    if (meta.is_null()) {
        meta = nlohmann::json::object();
    }
    if (!meta.is_object()) {
        throw McpError(mcp::constants::jsonrpc::g_INVALID_PARAMS, "Request _meta must be an object");
    }
    ProgressToken token;
    if (const auto found = meta.find("progressToken"); found != meta.end()) {
        try {
            token = found->get<ProgressToken>();
        } catch (const std::exception&) {
            throw McpError(mcp::constants::jsonrpc::g_INVALID_PARAMS,
                           "Request _meta.progressToken must be a string or an integer");
        }
    } else {
        token = ProgressToken{
            std::string(detail::constants::client_requests::g_PROGRESS_TOKEN_PREFIX) +
            std::to_string(state->next_progress_token.fetch_add(1, std::memory_order_relaxed))};
        meta["progressToken"] = token;
    }
    auto key = std::make_shared<const std::string>(token.correlation_key());
    {
        std::lock_guard lock(state->handlers_mutex);
        if (!state->progress_routes.emplace(*key, route).second) {
            throw std::invalid_argument(
                "The request's progress token is already used by another request in flight");
        }
    }
    ProgressRoute routed;
    routed.state = state;
    routed.key = std::move(key);
    routed.route = std::move(route);
    return routed;
}

Task<nlohmann::json> Client::Impl::request_with_progress(
    std::shared_ptr<Impl> state, std::shared_ptr<const std::string> method, nlohmann::json params,
    RequestOptions request_options, bool declare_tasks, std::shared_ptr<bool> answered_input) {
    const auto route = route_progress(state, params, request_options);
    auto reply = co_await request(state, *method, params, request_options, declare_tasks,
                                  std::move(answered_input));
    co_return reply;
}

bool Client::Impl::deliver_request_progress(const std::shared_ptr<Impl>& state,
                                            const nlohmann::json& json_message) {
    const auto params = json_message.find("params");
    if (params == json_message.end() || !params->is_object()) {
        return false;
    }
    const auto token = params->find("progressToken");
    if (token == params->end() || !(token->is_string() || token->is_number_integer())) {
        return false;
    }
    std::shared_ptr<ProgressRouteState> route;
    {
        const auto key = token->get<ProgressToken>().correlation_key();
        std::lock_guard lock(state->handlers_mutex);
        const auto found = state->progress_routes.find(key);
        if (found == state->progress_routes.end()) {
            return false;
        }
        route = found->second;
    }
    ProgressNotificationParams progress;
    try {
        params->get_to(progress);
    } catch (const std::exception& error) {
        report_protocol_error(state, mcp::constants::jsonrpc::g_INVALID_PARAMS,
                              "Malformed progress notification params: " +
                                  detail::sanitize_for_diagnostics(error.what()));
        return true;
    }
    // As for any notification callback, a throw is reported and the session carries on.
    std::lock_guard lock(route->mutex);
    if (!route->open) {
        return true;
    }
    if (!route->bucket.take()) {
        state->dropped_notifications.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    try {
        route->callback(progress);
    } catch (const std::exception& error) {
        report_protocol_error(
            state, mcp::constants::jsonrpc::g_INTERNAL_ERROR,
            "Progress callback threw: " + detail::sanitize_for_diagnostics(error.what()));
    } catch (...) {
        report_protocol_error(state, mcp::constants::jsonrpc::g_INTERNAL_ERROR,
                              "Progress callback threw");
    }
    return true;
}

Task<nlohmann::json> Client::Impl::request_wire(std::shared_ptr<Impl> state, std::string_view method,
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
    auto wire =
        std::make_shared<const std::string>(mcp::detail::dump_json(nlohmann::json(std::move(request))));
    auto strand = state->strand;
    // Awaiting post(strand) would not rebind a caller-owned coroutine's continuation.
    // Spawn the complete pending-request lifecycle on the client strand instead.
    // A client must not cancel an initialize request.
    const bool cancellable = method != methods::g_INITIALIZE;
    return boost::asio::co_spawn(strand,
                                 send_request_wire(std::move(state), std::move(wire), id, timeout,
                                                   probe, cancellable, request_options.cancellation),
                                 boost::asio::use_awaitable);
}

void Client::Impl::reject_removed_method(const std::string& version, std::string_view method) {
    static constexpr std::array<std::string_view, 5> s_removed = {
        methods::g_INITIALIZE, methods::g_PING, methods::g_LOGGING_SET_LEVEL,
        methods::g_RESOURCES_SUBSCRIBE, methods::g_RESOURCES_UNSUBSCRIBE};
    if (std::find(s_removed.begin(), s_removed.end(), method) != s_removed.end()) {
        throw McpError(
            mcp::constants::jsonrpc::g_METHOD_NOT_FOUND,
            "Method " + std::string(method) + " does not exist in protocol version " + version);
    }
}

Task<nlohmann::json> Client::Impl::checked_result(Task<nlohmann::json> pending) {
    auto result = co_await std::move(pending);
    check_complete(result);
    co_return result;
}

void Client::Impl::check_complete(const nlohmann::json& result) {
    if (result.is_object()) {
        const auto found = result.find("resultType");
        if (found != result.end() && !(found->is_string() && *found == "complete")) {
            std::string shown = found->is_string() ? found->get<std::string>() : found->dump();
            throw McpError(
                mcp::constants::jsonrpc::g_INVALID_REQUEST,
                "Unsupported resultType in result: " + detail::sanitize_for_diagnostics(shown));
        }
    }
}

Task<void> Client::Impl::notification(std::shared_ptr<Impl> state, std::string_view method,
                                      const std::optional<nlohmann::json>& params) {
    JSONRPCNotification notification;
    notification.method = std::string(method);
    notification.params = params;
    auto wire = std::make_shared<const std::string>(
        mcp::detail::dump_json(nlohmann::json(std::move(notification))));
    auto strand = state->strand;
    // Keep the closed-state check and write initiation on the client strand.
    return boost::asio::co_spawn(strand, send_notification_wire(std::move(state), std::move(wire)),
                                 boost::asio::use_awaitable);
}

Task<void> Client::Impl::discard_result(Task<nlohmann::json> request_task) {
    static_cast<void>(co_await std::move(request_task));
}

void Client::Impl::start_read_loop(const std::shared_ptr<Impl>& state) {
    boost::asio::co_spawn(state->strand, read_loop(state), boost::asio::detached);
}

void Client::Impl::request_close(const std::shared_ptr<Impl>& state, std::string message) {
    if (state->closed.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    boost::asio::post(state->strand, [state, message = std::move(message)]() mutable {
        fail_pending_requests(state, Error{mcp::constants::jsonrpc::g_CONNECTION_CLOSED,
                                           std::move(message), std::nullopt});
        state->transport->close();
        state->incoming_requests_wake.cancel();
        for (const auto& pause : state->poll_pauses) {
            if (auto timer = pause.lock()) {
                timer->cancel();
            }
        }
        state->poll_pauses.clear();
    });
}

std::string Client::Impl::request_correlation_key(int64_t id) {
    return RequestId{std::to_string(id)}.correlation_key();
}

Task<nlohmann::json> Client::Impl::send_request_wire(std::shared_ptr<Impl> state,
                                                     std::shared_ptr<const std::string> wire,
                                                     int64_t id, std::chrono::milliseconds timeout,
                                                     bool probe, bool cancellable,
                                                     std::optional<CancellationToken> cancellation) {
    if (state->closed.load(std::memory_order_acquire)) {
        throw McpError(mcp::constants::jsonrpc::g_CONNECTION_CLOSED, "Client transport is closed");
    }

    // [gcc11-sso: scope-before-await] The correlation key is a short std::string, so it is
    // built in this scope and rebuilt from `id` after the suspension rather than kept across it.
    PendingRequest* pending_request = nullptr;
    CancellationRegistration cancellation_registration;
    {
        auto id_key = request_correlation_key(id);
        {
            std::lock_guard lock(state->pending_requests_mutex);
            auto [iter, inserted] = state->pending_requests.try_emplace(
                id_key, std::make_unique<PendingRequest>(state->strand, timeout));
            if (!inserted) {
                throw McpError(mcp::constants::jsonrpc::g_INVALID_REQUEST,
                               "Duplicate pending request id: " + id_key);
            }
            pending_request = iter->second.get();
            pending_request->probe = probe;
        }
        if (cancellation) {
            const auto& token_state = detail::CancellationAccess::state(*cancellation);
            const auto registration = token_state->add([weak_state = std::weak_ptr<Impl>(state),
                                                        id](const std::optional<std::string>& reason) {
                if (auto owner = weak_state.lock()) {
                    auto strand = owner->strand;
                    boost::asio::post(strand, [owner = std::move(owner), id, reason] {
                        cancel_request(owner, id, reason);
                    });
                }
            });
            if (!registration) {
                // Cancelled before it was sent: nothing reaches the server.
                erase_pending_request(state, id_key);
                throw McpError(mcp::constants::jsonrpc::g_REQUEST_CANCELLED, "Request cancelled");
            }
            cancellation_registration = CancellationRegistration(token_state, *registration);
        }

        boost::asio::co_spawn(state->strand,
                              state->writes_beside_queue()
                                  ? state->writer.write_overlapping(std::move(wire))
                                  : state->writer.write_message(
                                        std::move(wire), pending_request->cancel_write_before_start),
                              [state, id_key = std::move(id_key)](std::exception_ptr write_error) {
                                  complete_request_write(state, id_key, std::move(write_error));
                              });
    }

    // The wait ends when the timer fires, when a response or failure signals it, and when the
    // deadline is paused or resumed, which moves the timer. Only the last goes back to waiting.
    do {
        try {
            co_await pending_request->timer.async_wait(boost::asio::use_awaitable);
        } catch (const boost::system::system_error& error) {
            if (error.code() != boost::asio::error::operation_aborted) {
                erase_pending_request(state, request_correlation_key(id));
                throw;
            }
        }
    } while (deadline_moved(state, id));

    nlohmann::json result;
    std::optional<Error> error;
    std::exception_ptr write_error;
    bool timed_out = false;
    {
        const auto id_key = request_correlation_key(id);
        std::lock_guard lock(state->pending_requests_mutex);
        auto iter = state->pending_requests.find(id_key);
        if (iter == state->pending_requests.end()) {
            throw McpError(mcp::constants::jsonrpc::g_CONNECTION_CLOSED,
                           "Pending request was removed: " + id_key);
        }
        if (!iter->second->completed) {
            iter->second->cancel_write_before_start->store(true, std::memory_order_release);
            iter->second->error = Error{mcp::constants::jsonrpc::g_REQUEST_TIMEOUT, "Request timed out",
                                        nlohmann::json{{"id", std::to_string(id)}}};
            iter->second->completed = true;
            timed_out = true;
        }
        result = std::move(iter->second->result);
        error = std::move(iter->second->error);
        write_error = std::move(iter->second->write_error);
        state->pending_requests.erase(iter);
    }

    // A request the client stops waiting for is cancelled, as the specification asks of a
    // timeout, so the server stops working on it and an HTTP transport closes its response
    // stream. An era probe is given up the same way where that closes a connection; elsewhere
    // the server may predate the era.
    const bool cancels =
        probe ? state->modern_version().has_value() || state->overlapping_writes : cancellable;
    if (timed_out && cancels) {
        send_cancelled(state, id, std::string("Request timed out"));
    }

    if (error) {
        throw McpError(std::move(*error));
    }
    if (write_error) {
        // The era probe tells a refused write from an answer, so it gets the failure as it is.
        if (probe) {
            std::rethrow_exception(write_error);
        }
        throw_transport_failure(write_error);
    }
    co_return result;
}

void Client::Impl::send_cancelled(const std::shared_ptr<Impl>& state, int64_t id,
                                  std::optional<std::string> reason) {
    if (state->closed.load(std::memory_order_acquire)) {
        return;
    }
    CancelledNotificationParams cancel_params;
    cancel_params.requestId = RequestId{std::to_string(id)};
    cancel_params.reason = std::move(reason);
    JSONRPCNotification notification;
    notification.method = methods::g_NOTIFICATIONS_CANCELLED;
    notification.params = nlohmann::json(std::move(cancel_params));
    auto cancel_wire = std::make_shared<const std::string>(
        mcp::detail::dump_json(nlohmann::json(std::move(notification))));
    boost::asio::co_spawn(state->strand, send_notification_wire(state, std::move(cancel_wire)),
                          [](const std::exception_ptr&) {});
}

void Client::Impl::cancel_request(const std::shared_ptr<Impl>& state, int64_t id,
                                  const std::optional<std::string>& reason) {
    {
        const auto id_key = request_correlation_key(id);
        std::lock_guard lock(state->pending_requests_mutex);
        const auto iter = state->pending_requests.find(id_key);
        if (iter == state->pending_requests.end() || iter->second->completed) {
            return;
        }
        // A write still queued is dropped, and a response that arrives later finds no request.
        iter->second->cancel_write_before_start->store(true, std::memory_order_release);
        iter->second->error = Error{mcp::constants::jsonrpc::g_REQUEST_CANCELLED, "Request cancelled"};
        iter->second->completed = true;
        signal_pending_request(*iter->second);
    }
    send_cancelled(state, id, reason);
}

Task<void> Client::Impl::pause_unless_cancelled(std::shared_ptr<Impl> state,
                                                std::chrono::milliseconds time,
                                                std::optional<CancellationToken> cancellation) {
    if (state->closed.load(std::memory_order_acquire)) {
        co_return;
    }
    auto timer = std::make_shared<boost::asio::steady_timer>(state->strand, time);
    std::erase_if(state->poll_pauses, [](const auto& pause) { return pause.expired(); });
    state->poll_pauses.push_back(timer);
    CancellationRegistration cancellation_registration;
    if (cancellation) {
        const auto& token_state = detail::CancellationAccess::state(*cancellation);
        const auto registration =
            token_state->add([weak_timer = std::weak_ptr<boost::asio::steady_timer>(timer),
                              strand = state->strand](const std::optional<std::string>&) {
                boost::asio::post(strand, [weak_timer] {
                    if (auto pending = weak_timer.lock()) {
                        pending->cancel();
                    }
                });
            });
        if (!registration) {
            co_return;
        }
        cancellation_registration = CancellationRegistration(token_state, *registration);
    }
    boost::system::error_code ended;
    co_await timer->async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, ended));
}

Task<void> Client::Impl::send_notification_wire(std::shared_ptr<Impl> state,
                                                std::shared_ptr<const std::string> wire) {
    if (state->closed.load(std::memory_order_acquire)) {
        throw McpError(mcp::constants::jsonrpc::g_CONNECTION_CLOSED, "Client transport is closed");
    }

    try {
        if (state->writes_beside_queue()) {
            co_await state->writer.write_overlapping(wire);
        } else {
            co_await state->writer.write_message(wire);
        }
    } catch (...) {
        if (state->closed.load(std::memory_order_acquire)) {
            throw McpError(mcp::constants::jsonrpc::g_CONNECTION_CLOSED, "Client transport is closed");
        }
        throw_transport_failure(std::current_exception());
    }
}

Task<void> Client::Impl::read_loop(std::shared_ptr<Impl> state) {
    try {
        for (;;) {
            auto raw = co_await state->transport->read_message();
            dispatch_message(state, raw);
            // The server's requests run in the background, and each holds memory until its
            // answer is written. Reading stops here until they are fewer.
            while (
                (state->incoming_requests >= mcp::detail::constants::client::g_MAX_INCOMING_REQUESTS ||
                 state->incoming_request_bytes + state->writer.queued_bytes() >=
                     state->options.max_incoming_request_bytes) &&
                state->incoming_requests != 0 && !state->closed.load(std::memory_order_acquire)) {
                state->incoming_requests_wake.expires_at(std::chrono::steady_clock::time_point::max());
                boost::system::error_code ignored;
                co_await state->incoming_requests_wake.async_wait(
                    boost::asio::redirect_error(boost::asio::use_awaitable, ignored));
            }
        }
    } catch (const std::exception& error) {
        state->closed.store(true, std::memory_order_release);
        fail_pending_requests(
            state, Error{mcp::constants::jsonrpc::g_CONNECTION_CLOSED,
                         "Client read loop stopped: " + detail::sanitize_for_diagnostics(error.what()),
                         std::nullopt});
        state->transport->close();
    } catch (...) {
        state->closed.store(true, std::memory_order_release);
        fail_pending_requests(state, Error{mcp::constants::jsonrpc::g_CONNECTION_CLOSED,
                                           "Client read loop stopped", std::nullopt});
        state->transport->close();
    }
}

void Client::Impl::dispatch_message(const std::shared_ptr<Impl>& state, const std::string& raw) {
    try {
        if (detail::exceeds_json_depth(raw)) {
            throw McpError(mcp::constants::jsonrpc::g_PARSE_ERROR,
                           "JSON-RPC message nests deeper than " +
                               std::to_string(mcp::detail::constants::peer_json::g_MAX_DEPTH) +
                               " levels");
        }
        auto json_message = nlohmann::json::parse(raw);

        if (!json_message.is_object()) {
            throw McpError(mcp::constants::jsonrpc::g_INVALID_REQUEST,
                           "JSON-RPC message must be an object");
        }
        if (state->options.strict_protocol_validation) {
            if (!json_message.contains("jsonrpc") || !json_message.at("jsonrpc").is_string()) {
                throw McpError(mcp::constants::jsonrpc::g_INVALID_REQUEST,
                               "JSON-RPC message is missing jsonrpc");
            }
            detail::validate_jsonrpc_version(json_message.at("jsonrpc").get<std::string>());
        }

        const bool has_id = json_message.contains("id");
        const bool has_method = json_message.contains("method");
        if (has_method && !json_message.at("method").is_string()) {
            throw McpError(mcp::constants::jsonrpc::g_INVALID_REQUEST,
                           "JSON-RPC method must be a string");
        }

        if (has_id && !has_method) {
            dispatch_response(state, json_message);
        } else if (has_id && has_method) {
            // dispatch_incoming_request is detached, so anything that throws inside it is
            // swallowed and the peer never hears back. Reject an unusable id here, where the
            // drop is reported, instead of leaving the request silently unanswered.
            const auto& id = json_message.at("id");
            if (!detail::is_request_id(id)) {
                throw McpError(mcp::constants::jsonrpc::g_INVALID_REQUEST,
                               "JSON-RPC request id must be a string or an integer within "
                               "int64_t");
            }
            const auto cost = detail::json_memory_cost(json_message);
            ++state->incoming_requests;
            state->incoming_request_bytes += cost;
            boost::asio::co_spawn(state->strand,
                                  dispatch_incoming_request(state, std::move(json_message)),
                                  [state, cost](const std::exception_ptr&) {
                                      --state->incoming_requests;
                                      state->incoming_request_bytes -= cost;
                                      state->incoming_requests_wake.cancel();
                                  });
        } else if (!has_id && has_method) {
            dispatch_notification(state, json_message);
        }
    } catch (const std::exception& error) {
        fail_probe(state);
        report_protocol_error(state, mcp::constants::jsonrpc::g_PARSE_ERROR,
                              "Dropped an undecodable message from the peer: " +
                                  detail::sanitize_for_diagnostics(error.what()));
    } catch (...) {
        fail_probe(state);
        report_protocol_error(state, mcp::constants::jsonrpc::g_PARSE_ERROR,
                              "Dropped an undecodable message from the peer");
    }
}

void Client::Impl::report_protocol_error(const std::shared_ptr<Impl>& state, int code,
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

void Client::Impl::dispatch_response(const std::shared_ptr<Impl>& state,
                                     const nlohmann::json& json_message) {
    const auto& id = json_message.at("id");
    if (!detail::is_request_id(id)) {
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
            Error{mcp::constants::jsonrpc::g_INVALID_REQUEST,
                  "JSON-RPC response must contain exactly one of result or error", std::nullopt};
    } else if (has_error) {
        // An undecodable `error` member still identifies the request it answers. Fail that
        // request now rather than drop the response and leave the caller waiting out its
        // deadline for an answer that has already arrived.
        try {
            iter->second->error = json_message.at("error").get<Error>();
        } catch (const std::exception& decode_error) {
            iter->second->error = Error{mcp::constants::jsonrpc::g_INVALID_REQUEST,
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

void Client::Impl::dispatch_notification(const std::shared_ptr<Impl>& state,
                                         const nlohmann::json& json_message) {
    const auto method = json_message.at("method").get<std::string>();
    state->response_cache.invalidate(method, json_message);
    if (method == methods::g_NOTIFICATIONS_CANCELLED &&
        end_subscription_from_cancel(state, json_message)) {
        return;
    }
    route_to_subscription(state, method, json_message);
    if (method == methods::g_NOTIFICATIONS_PROGRESS && deliver_request_progress(state, json_message)) {
        return;
    }
    // Progress no request in flight claims goes to Client::on_progress, through one bucket for
    // the client; log messages share another.
    if ((method == methods::g_NOTIFICATIONS_PROGRESS && !state->progress_bucket.take()) ||
        (method == methods::g_NOTIFICATIONS_MESSAGE && !state->log_bucket.take())) {
        state->dropped_notifications.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (method == methods::g_NOTIFICATIONS_CANCELLED) {
        // Unless it ends a listen (above), a server's cancel names a request the server sent. The
        // peers number their requests independently, so its id may also be that of one of the
        // client's own requests, which it does not concern. Request handlers cannot be stopped, so
        // the notification is only checked.
        if (detail::has_json_value(json_message, "params")) {
            static_cast<void>(json_message.at("params").get<CancelledNotificationParams>());
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
            report_protocol_error(state, mcp::constants::jsonrpc::g_INTERNAL_ERROR,
                                  "Notification callback for " +
                                      detail::sanitize_for_diagnostics(method) +
                                      " threw: " + detail::sanitize_for_diagnostics(error.what()));
        } catch (...) {
            report_protocol_error(
                state, mcp::constants::jsonrpc::g_INTERNAL_ERROR,
                "Notification callback for " + detail::sanitize_for_diagnostics(method) + " threw");
        }
    }
}

Task<void> Client::Impl::dispatch_incoming_request(std::shared_ptr<Impl> state,
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

        // An answer whose text is not UTF-8 cannot be encoded. The server is answered anyway,
        // or its request would wait out its whole timeout.
        const auto id = json_message.at("id").get<RequestId>();
        std::string wire;
        try {
            wire = error_payload.is_null()
                       ? make_result_wire(id, std::move(result))
                       : make_error_wire(id, mcp::constants::jsonrpc::g_INTERNAL_ERROR,
                                         error_payload.get<std::string>());
        } catch (const nlohmann::json::exception&) {
            wire = make_error_wire(id, mcp::constants::jsonrpc::g_INTERNAL_ERROR,
                                   "The answer could not be encoded as JSON");
        }
        co_await state->writer.write_message(std::move(wire));
    } else if (method == methods::g_PING) {
        co_await state->writer.write_message(
            make_result_wire(json_message.at("id").get<RequestId>(), nlohmann::json::object()));
    } else {
        co_await state->writer.write_message(make_error_wire(
            json_message.at("id").get<RequestId>(), mcp::constants::jsonrpc::g_METHOD_NOT_FOUND,
            "Method not found: " + std::string(method)));
    }
}

std::string Client::Impl::make_result_wire(const RequestId& id, nlohmann::json result) {
    JSONRPCResultResponse response;
    response.id = id;
    response.result = std::move(result);
    return mcp::detail::dump_json(nlohmann::json(std::move(response)));
}

std::string Client::Impl::make_error_wire(const RequestId& id, int code, std::string message) {
    Error error;
    error.code = code;
    error.message = std::move(message);
    JSONRPCErrorResponse response;
    response.id = id;
    response.error = std::move(error);
    return mcp::detail::dump_json(nlohmann::json(std::move(response)));
}

bool Client::Impl::deadline_moved(const std::shared_ptr<Impl>& state, int64_t id) {
    const auto id_key = request_correlation_key(id);
    std::lock_guard lock(state->pending_requests_mutex);
    const auto iter = state->pending_requests.find(id_key);
    return iter != state->pending_requests.end() && !iter->second->completed &&
           std::chrono::steady_clock::now() < iter->second->deadline;
}

void Client::Impl::on_interaction(const std::shared_ptr<Impl>& state, std::string_view key,
                                  bool waiting) {
    std::lock_guard lock(state->pending_requests_mutex);
    const auto iter = state->pending_requests.find(key);
    if (iter == state->pending_requests.end()) {
        return;
    }
    auto& pending = *iter->second;
    const auto now = std::chrono::steady_clock::now();
    if (waiting && !pending.completed && !pending.paused_remaining) {
        pending.paused_remaining = pending.deadline > now ? pending.deadline - now
                                                          : std::chrono::steady_clock::duration::zero();
        pending.deadline = std::chrono::steady_clock::time_point::max();
        pending.timer.expires_at(pending.deadline);
    } else if (!waiting && pending.paused_remaining) {
        pending.deadline = now + *pending.paused_remaining;
        pending.paused_remaining.reset();
        if (!pending.completed) {
            pending.timer.expires_at(pending.deadline);
        }
    }
}

void Client::Impl::signal_pending_request(PendingRequest& pending_request) {
    pending_request.timer.expires_at(std::chrono::steady_clock::now());
}

void Client::Impl::complete_request_write(const std::shared_ptr<Impl>& state, const std::string& id_key,
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
        iter->second->error =
            Error{mcp::constants::jsonrpc::g_CONNECTION_CLOSED, "Client transport is closed"};
    } else {
        iter->second->write_error = std::move(write_error);
    }
    iter->second->completed = true;
    signal_pending_request(*iter->second);
}

void Client::Impl::erase_pending_request(const std::shared_ptr<Impl>& state,
                                         const std::string& id_key) {
    std::lock_guard lock(state->pending_requests_mutex);
    state->pending_requests.erase(id_key);
}

void Client::Impl::fail_pending_requests(const std::shared_ptr<Impl>& state, const Error& error) {
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

std::optional<nlohmann::json> Client::Impl::with_request_meta(
    const std::string& version, const std::optional<nlohmann::json>& params,
    const std::optional<LoggingLevel>& level, bool declare_tasks) const {
    nlohmann::json merged = params ? *params : nlohmann::json::object();
    if (merged.is_null()) {
        merged = nlohmann::json::object();
    }
    if (!merged.is_object()) {
        throw McpError(mcp::constants::jsonrpc::g_INVALID_PARAMS, "Request params must be an object");
    }
    nlohmann::json meta = nlohmann::json::object();
    if (const auto found = merged.find("_meta"); found != merged.end() && !found->is_null()) {
        if (!found->is_object()) {
            throw McpError(mcp::constants::jsonrpc::g_INVALID_PARAMS,
                           "Request _meta must be an object");
        }
        meta = *found;
    }
    meta[std::string(mcp::constants::meta::g_META_PROTOCOL_VERSION)] = version;
    meta[std::string(mcp::constants::meta::g_META_CLIENT_CAPABILITIES)] = effective_capabilities();
    if (declare_tasks || options.tasks == ClientTaskMode::eAuto) {
        auto& extensions =
            meta[std::string(mcp::constants::meta::g_META_CLIENT_CAPABILITIES)]["extensions"];
        if (!extensions.is_object()) {
            extensions = nlohmann::json::object();
        }
        if (!extensions.contains(std::string(mcp::constants::tasks::g_EXTENSION_ID))) {
            extensions[std::string(mcp::constants::tasks::g_EXTENSION_ID)] = nlohmann::json::object();
        }
    }
    {
        std::lock_guard lock(handlers_mutex);
        meta[std::string(mcp::constants::meta::g_META_CLIENT_INFO)] = client_info;
    }
    if (level) {
        meta[std::string(mcp::constants::meta::g_META_LOG_LEVEL)] = *level;
    }
    merged["_meta"] = std::move(meta);
    return merged;
}

}  // namespace mcp

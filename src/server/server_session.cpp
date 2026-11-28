#include "server_impl.hpp"

namespace methods = mcp::constants::methods;
namespace transport_writer = mcp::detail::constants::transport_writer;

namespace mcp {

// The inverse of the notify_* forwarders below, and safe for the opposite reason: this one returns
// the task instead of awaiting it, which is only sound because Impl::send_request is not a
// coroutine. Its body runs to completion inside this call, so neither reference parameter has to
// outlive the return. Converting Impl::send_request to co_return would leave both dangling here
// with no diagnostic; change this line to co_await in the same commit if you ever do.
Task<nlohmann::json> Server::send_request(const std::string& method,
                                          const std::optional<nlohmann::json>& params) {
    return impl_->send_request(nullptr, method, params);
}

// Deliberately not a coroutine -- see the note on Server::send_request above. It takes both
// parameters by reference and its caller forwards with a plain `return`, so the body must run
// before that return completes.
Task<nlohmann::json> Server::Impl::send_request(std::shared_ptr<std::atomic<bool>> caller_cancelled,
                                                const std::string& method,
                                                const std::optional<nlohmann::json>& params,
                                                std::shared_ptr<const std::string> related_request) {
    // Distinct from the two failures below: a handler still running after its Server was
    // destroyed has to be able to tell that apart from a session that is merely closing and
    // from stateless dispatch, because the right response differs in each case.
    if (server_gone.load(std::memory_order_acquire)) {
        throw std::runtime_error("reverse RPC is unavailable: the Server has been destroyed");
    }
    auto session = session_snapshot();
    if (!session || !session->transport || !session->strand ||
        session->stopping.load(std::memory_order_acquire)) {
        // A handler's Context calls with caller_cancelled; Server::send_request() without.
        if (caller_cancelled) {
            throw std::runtime_error("reverse RPC is unavailable: the client's session has ended");
        }
        throw std::runtime_error(
            "reverse RPC is unavailable: this Server serves no session with a client now; a "
            "handler reaches its client through its Context");
    }

    // [gcc11-sso: int64-id] DO NOT change id to std::string.
    const int64_t id = next_request_id.fetch_add(1, std::memory_order_relaxed);
    JSONRPCRequest request;
    request.id = RequestId{std::to_string(id)};
    request.method = method;
    request.params = params;
    auto wire =
        std::make_shared<const std::string>(mcp::detail::dump_json(nlohmann::json(std::move(request))));
    const std::chrono::milliseconds timeout(reverse_request_timeout_ms.load(std::memory_order_relaxed));
    return await_reverse_response(std::move(session), std::move(wire), id, timeout,
                                  std::move(caller_cancelled), std::move(related_request));
}

Task<void> Server::Impl::write_to_session(const Session& session,
                                          std::shared_ptr<const std::string> wire,
                                          const std::shared_ptr<const std::string>& related_request) {
    if (related_request) {
        return session.writer->write_related(std::move(wire), related_request);
    }
    return session.writer->write_message(std::move(wire));
}

Task<nlohmann::json> Server::Impl::await_reverse_response(
    std::shared_ptr<Session> session, std::shared_ptr<const std::string> wire, int64_t id,
    std::chrono::milliseconds timeout, std::shared_ptr<std::atomic<bool>> cancelled,
    std::shared_ptr<const std::string> related_request) {
    auto strand = *session->strand;
    // A caller's awaitable keeps its original executor across an awaited post. Launch the
    // complete correlation lifecycle on the session strand so map and timer state stay confined.
    return mcp::detail::spawn_awaitable(
        strand, await_reverse_response_on_strand(std::move(session), std::move(wire), id, timeout,
                                                 std::move(cancelled), std::move(related_request)));
}

Task<nlohmann::json> Server::Impl::await_reverse_response_on_strand(
    std::shared_ptr<Session> session, std::shared_ptr<const std::string> wire, int64_t id,
    std::chrono::milliseconds timeout, std::shared_ptr<std::atomic<bool>> cancelled,
    std::shared_ptr<const std::string> related_request) {
    if (session->stopping.load(std::memory_order_acquire)) {
        throw std::runtime_error("server session is closing");
    }
    // A cancel that came before the handler asked has nothing to wake, so it is checked here.
    if (cancelled && cancelled->load(std::memory_order_relaxed)) {
        throw std::runtime_error("JSON-RPC error " +
                                 std::to_string(mcp::constants::jsonrpc::g_REQUEST_CANCELLED) +
                                 ": The request was cancelled");
    }

    auto timer = std::make_shared<boost::asio::steady_timer>(*session->strand);
    timer->expires_after(timeout);
    {
        const auto id_key = RequestId{std::to_string(id)}.correlation_key();
        const auto [pending_it, inserted] =
            session->pending_requests.try_emplace(id_key, PendingRequest{
                                                              .timer = timer,
                                                              .result = {},
                                                              .error = std::nullopt,
                                                              .completed = false,
                                                              .epoch = session_epoch(*session),
                                                              .caller_cancelled = std::move(cancelled),
                                                          });
        static_cast<void>(pending_it);
        if (!inserted) {
            throw std::runtime_error("duplicate pending request id: " + id_key);
        }
    }

    try {
        co_await write_to_session(*session, wire, related_request);
    } catch (...) {
        session->pending_requests.erase(RequestId{std::to_string(id)}.correlation_key());
        throw;
    }

    bool completed = false;
    {
        const auto id_key = RequestId{std::to_string(id)}.correlation_key();
        const auto pending_it = session->pending_requests.find(id_key);
        completed = pending_it != session->pending_requests.end() && pending_it->second.completed;
    }
    if (!completed) {
        try {
            co_await timer->async_wait(boost::asio::use_awaitable);
            // Ran to its end: the client did not answer in time.
            const auto id_key = RequestId{std::to_string(id)}.correlation_key();
            const auto pending_it = session->pending_requests.find(id_key);
            if (pending_it != session->pending_requests.end() && !pending_it->second.completed) {
                pending_it->second.error = Error{
                    .code = mcp::constants::jsonrpc::g_REQUEST_TIMEOUT,
                    .message = "The client did not answer in time",
                    .data = std::nullopt,
                };
                pending_it->second.completed = true;
                pending_it->second.given_up = true;
            }
        } catch (const boost::system::system_error& err) {
            if (err.code() != boost::asio::error::operation_aborted) {
                session->pending_requests.erase(RequestId{std::to_string(id)}.correlation_key());
                throw;
            }
        }
    }

    // [gcc11-sso: int64-id] Rebuild the string only after all suspensions.
    const auto id_key = RequestId{std::to_string(id)}.correlation_key();
    auto it = session->pending_requests.find(id_key);
    if (it == session->pending_requests.end()) {
        throw std::runtime_error("pending request not found for id: " + std::to_string(id));
    }

    auto json_result = std::move(it->second.result);
    auto error = std::move(it->second.error);
    const bool given_up = it->second.given_up;
    session->pending_requests.erase(it);

    if (given_up) {
        // The server gave up on its request, so the client can stop working on it. An error the
        // client answered with, whatever its code, ended the request on its side already.
        std::string notice;
        {
            CancelledNotificationParams params;
            params.requestId = RequestId{std::to_string(id)};
            params.reason = error->message;
            JSONRPCNotification notification;
            notification.method = methods::g_NOTIFICATIONS_CANCELLED;
            notification.params = nlohmann::json(std::move(params));
            notice = mcp::detail::dump_json(nlohmann::json(std::move(notification)));
        }
        try {
            co_await write_to_session(*session, std::make_shared<const std::string>(std::move(notice)),
                                      related_request);
        } catch (const std::exception&) {
            // The session may be ending; the request is over either way.
        }
    }

    if (error) {
        // Same defect as the peer-controlled text in the client's McpError, opposite direction:
        // this is the error a CLIENT returned for a server-initiated request (sampling,
        // elicitation, roots/list), stored verbatim by dispatch_response(). `message` is the
        // client's text and this diagnostic is what the server operator logs, with no
        // authorization step in the way, so it is flattened and bounded before it goes in.
        throw std::runtime_error("JSON-RPC error " + std::to_string(error->code) + ": " +
                                 detail::sanitize_for_diagnostics(error->message));
    }

    co_return json_result;
}

Task<nlohmann::json> Server::invoke_tool(const std::string& tool_name, const nlohmann::json& args) {
    return impl_->invoke_tool_impl(
        CallToolParams{.name = tool_name, .arguments = args, .meta = std::nullopt}, nullptr,
        std::nullopt);
}

Task<void> Server::run(std::shared_ptr<ITransport> transport, const Executor& executor) {
    // Deliberately not a coroutine: reading impl_ here, on the caller's thread, hands the session
    // a reference that outlives this Server rather than one that is read again later.
    return Impl::run(impl_, std::move(transport), executor.get());
}

Task<void> Server::Impl::run(std::shared_ptr<Impl> impl, std::shared_ptr<ITransport> transport,
                             boost::asio::any_io_executor executor) {
    if (!transport) {
        throw std::invalid_argument("Server transport must not be null");
    }
    impl->serving.store(true, std::memory_order_release);

    // stop() closes the transport on the run's executor, which ends the read loop below.
    const auto registration = impl->run_control->add(
        [transport, executor] { boost::asio::post(executor, [transport] { transport->close(); }); });

    auto session = std::make_shared<Session>();
    session->transport = std::move(transport);
    // Binds an HttpServerTransport to this server while it runs, and leaves it unbound however the
    // run ends, so a transport that outlives the run does not dispatch into a server that is not
    // running.
    struct HttpUnbinder {
        std::shared_ptr<ITransport> transport;
        HttpServerTransport* http_transport{nullptr};
        ~HttpUnbinder() {
            if (http_transport != nullptr) {
                detail::bind_server(*http_transport, {});
            }
        }
    } unbinder{.transport = session->transport};
    session->memory_transport = dynamic_cast<MemoryTransport*>(session->transport.get());
    session->session_pipe = dynamic_cast<detail::SessionPipe*>(session->transport.get());
    // HttpServerTransport is the one transport that can end a session (on HTTP DELETE) and start a
    // new one while run() keeps reading from it, so its count of ended sessions tells a new client
    // apart from the one that initialized. Other transports never end a session this way. A
    // transport wrapping one hands it over through http_server_transport().
    if (auto* http_transport = session->transport->http_server_transport()) {
        session->http_transport = http_transport;
        unbinder.http_transport = http_transport;
        detail::bind_server(*http_transport,
                            {
                                .served_versions = impl->supported_versions(),
                                .modern_dispatch =
                                    [weak = std::weak_ptr<Impl>(impl)](nlohmann::json json_msg,
                                                                       RequestContext request_context) {
                                        return Impl::dispatch_for_http(weak, std::move(json_msg),
                                                                       std::move(request_context));
                                    },
                                .dispatch_executor = executor,
                            });
    }
    session->strand =
        std::make_unique<boost::asio::any_io_executor>(detail::make_strand_executor(executor));
    session->writer =
        std::make_shared<detail::SerializedTransportWriter>(session->transport, *session->strand);
    session->drain_timer = std::make_shared<boost::asio::steady_timer>(*session->strand);
    session->drain_timer->expires_at(std::chrono::steady_clock::time_point::max());
    auto strand = *session->strand;
    // The read loop and teardown both own session state, so run both on the session strand.
    co_await mcp::detail::spawn_awaitable(strand, impl->run_session(std::move(session)));
}

Task<void> Server::Impl::run_session(std::shared_ptr<Session> new_session) {
    auto session = std::move(new_session);
    {
        std::scoped_lock lock(session_mutex);
        if (this->session) {
            throw std::runtime_error("Server already has an active session");
        }
        this->session = session;
    }
    // The end below unregisters the session. When the event loop is destroyed while this coroutine
    // waits, as run_stdio() and run_http() destroy theirs once the shutdown deadline passes, the
    // session is unregistered here instead, so that ~Server does not reach the destroyed loop.
    class Unregister {
       public:
        Unregister(Impl& impl, const std::shared_ptr<Session>& session)
            : impl_(impl), session_(session) {}
        Unregister(const Unregister&) = delete;
        Unregister& operator=(const Unregister&) = delete;
        Unregister(Unregister&&) = delete;
        Unregister& operator=(Unregister&&) = delete;
        ~Unregister() {
            std::scoped_lock lock(impl_.session_mutex);
            if (impl_.session == session_) {
                impl_.session.reset();
            }
        }

       private:
        Impl& impl_;
        const std::shared_ptr<Session>& session_;
    };
    const Unregister unregister(*this, session);

    lifecycle.store(Impl::LifecycleState::eUninitialized, std::memory_order_relaxed);
    shutdown_requested.store(false, std::memory_order_relaxed);
    // The next client on a reused Server starts from the default, not from what the last one chose.
    log_level.store(g_default_log_level, std::memory_order_relaxed);
    {
        std::scoped_lock lock(client_capabilities_mutex);
        negotiated_version.clear();
        client_capabilities = nlohmann::json::object();
    }

    co_await read_session(session);
    co_await end_session(session);
}

Task<void> Server::Impl::read_session(std::shared_ptr<Session> session) {
    try {
        for (;;) {
            nlohmann::json json_msg;
            std::shared_ptr<const std::string> principal;
            bool parse_failed = false;
            try {
                if (session->memory_transport != nullptr) {
                    auto message = co_await detail::MemoryTransportAccess::read_json_until_closed(
                        *session->memory_transport);
                    if (!message) {
                        break;
                    }
                    json_msg = std::move(*message);
                } else if (session->session_pipe != nullptr) {
                    auto message = co_await session->session_pipe->next_message();
                    if (!message) {
                        break;
                    }
                    json_msg = std::move(*message);
                } else {
                    auto incoming = co_await session->transport->read_incoming();
                    json_msg = detail::parse_peer_json(incoming.message);
                    parse_failed = json_msg.is_discarded();
                    if (!incoming.principal.empty()) {
                        principal = std::make_shared<const std::string>(std::move(incoming.principal));
                    }
                }
            } catch (const nlohmann::json::exception&) {
                // Text a MemoryTransport parses on the way in, such as a number past the range of
                // a double, fails here; it is the peer's error, not the transport's.
                parse_failed = true;
            }
            if (parse_failed) {
                co_await answer_from_read_loop(
                    *session, std::make_shared<const std::string>(make_parse_error_wire()));
                continue;
            }
            const std::size_t message_bytes = detail::json_memory_cost(json_msg);
            // Past the limit nothing is left running in the background, so reading stops when the
            // peer stops taking answers. Answers to the server's own requests still get through.
            if (session_is_full(*session, message_bytes)) {
                co_await handle_past_limit(*session, std::move(json_msg));
                continue;
            }
            session->active_dispatches.fetch_add(1, std::memory_order_relaxed);
            session->active_dispatch_bytes.fetch_add(message_bytes, std::memory_order_relaxed);
            boost::asio::co_spawn(
                *session->strand,
                [impl = shared_from_this(), session, message_bytes, json_msg = std::move(json_msg),
                 principal = std::move(principal)]() mutable -> boost::asio::awaitable<void> {
                    try {
                        co_await impl->dispatch_on_strand(std::move(json_msg), std::move(principal));
                    } catch (...) {
                        // A failed request must not terminate the detached dispatcher.
                    }

                    session->active_dispatch_bytes.fetch_sub(message_bytes, std::memory_order_relaxed);
                    const auto remaining =
                        session->active_dispatches.fetch_sub(1, std::memory_order_acq_rel) - 1;
                    if (remaining == 0 && session->stopping.load(std::memory_order_acquire)) {
                        session->drain_timer->expires_at(std::chrono::steady_clock::now());
                    }
                    co_return;
                },
                boost::asio::detached);
        }
    } catch (const std::exception&) {
        // Closing a transport is the normal way to stop a session.
    }
}

Task<void> Server::Impl::handle_past_limit(Session& session, nlohmann::json json_msg) {
    if (is_identified_request(json_msg)) {
        co_await answer_from_read_loop(
            session, std::make_shared<const std::string>(make_error_wire(
                         json_msg.at("id").get<RequestId>(), mcp::constants::jsonrpc::g_INTERNAL_ERROR,
                         "Too many requests in flight")));
        co_return;
    }
    try {
        co_await dispatch_on_strand(std::move(json_msg));
    } catch (...) {
        // A failed message must not end the session.
    }
}

Task<void> Server::Impl::end_session(std::shared_ptr<Session> session) {
    // Teardown below can fail — a transport may throw from close, and the drain wait rethrows any
    // error that is not cancellation. Whichever way it ends, the session must be unregistered, or
    // every later run() is refused for the lifetime of this Server.
    try {
        session->stopping.store(true, std::memory_order_release);
        if (session->transport->stdio_transport() != nullptr) {
            // The end of standard input is how a client asks a stdio server to finish, and it may
            // still be reading standard output. The requests already started are told to stop,
            // and the answers of those that end, with every answer already queued, are written
            // before the transport closes. After a signal the transport is closed already, so those
            // writes fail at once. A handler that ignores Context::is_cancelled() is waited for.
            // Answers to the server's own requests and retries of parked handlers can no longer
            // arrive, and an open subscriptions/listen stream would never end on its own, so it
            // is answered with its empty result.
            abandon_pending_requests(*session);
            abandon_all_parked();
            end_listeners(ListenEnd::eClosed, session);
            for (auto& [id, cancelled] : session->in_flight) {
                static_cast<void>(id);
                cancelled->store(true, std::memory_order_relaxed);
            }
            co_await wait_for_dispatches(*session);
            co_await session->writer->written();
        }
        abandon_session_work(session);
        // A retry can only arrive over the transport that is closing, so the handlers waiting
        // for one are given up.
        abandon_all_parked();
        session->transport->close();

        co_await wait_for_dispatches(*session);
    } catch (...) {
        reset_session(session);
        throw;
    }

    reset_session(session);
}

Task<void> Server::Impl::wait_for_dispatches(Session& session) {
    while (session.active_dispatches.load(std::memory_order_acquire) != 0) {
        session.drain_timer->expires_at(std::chrono::steady_clock::time_point::max());
        if (session.active_dispatches.load(std::memory_order_acquire) == 0) {
            break;
        }
        try {
            co_await session.drain_timer->async_wait(boost::asio::use_awaitable);
        } catch (const boost::system::system_error& error) {
            if (error.code() != boost::asio::error::operation_aborted) {
                throw;
            }
        }
    }
}

Task<void> Server::Impl::answer_from_read_loop(Session& session,
                                               std::shared_ptr<const std::string> answer) {
    // Waiting for the write would stop reading, and with it the answers to this server's own
    // requests, which a peer at its own limit may be waiting to write before it reads again.
    // Answers are queued instead while what is queued stays within the byte limit; past it,
    // reading waits for the peer.
    if (session.writer->queued_bytes() + answer->size() + transport_writer::g_QUEUED_MESSAGE_OVERHEAD <=
        max_in_flight_bytes.load(std::memory_order_relaxed)) {
        session.writer->post_message(std::move(answer));
        co_return;
    }
    co_await session.writer->write_message(std::move(answer));
}

bool Server::Impl::session_is_full(const Session& session, std::size_t message_bytes) const {
    return session.active_dispatches.load(std::memory_order_relaxed) >=
               max_in_flight_requests.load(std::memory_order_relaxed) ||
           session.active_dispatch_bytes.load(std::memory_order_relaxed) +
                   session.writer->queued_bytes() + message_bytes >
               max_in_flight_bytes.load(std::memory_order_relaxed);
}

bool Server::Impl::is_identified_request(const nlohmann::json& json_msg) {
    if (!json_msg.is_object() || !json_msg.contains("method")) {
        return false;
    }
    const auto id = json_msg.find("id");
    return id != json_msg.end() && detail::is_request_id(*id);
}

Task<void> Server::dispatch(nlohmann::json json_msg) { return impl_->dispatch(std::move(json_msg)); }

Task<void> Server::Impl::dispatch(nlohmann::json json_msg) {
    auto session = session_snapshot();
    if (session && session->strand) {
        auto strand = *session->strand;
        co_await mcp::detail::spawn_awaitable(strand, dispatch_on_strand(std::move(json_msg)));
        co_return;
    }

    co_await dispatch_on_strand(std::move(json_msg));
}

Task<void> Server::Impl::dispatch_on_strand(nlohmann::json json_msg,
                                            std::shared_ptr<const std::string> principal) {
    if (is_valid_notification_envelope(json_msg)) {
        dispatch_notification(json_msg);
        co_return;
    }

    // A method without an id is notification-shaped. Notifications never receive responses,
    // including when their envelope is malformed.
    if (json_msg.is_object() && json_msg.contains("method") && !json_msg.contains("id")) {
        co_return;
    }

    if (json_msg.is_object() && !json_msg.contains("method") &&
        (json_msg.contains("result") || has_error_member(json_msg))) {
        dispatch_response(json_msg);
        co_return;
    }

    // Anything that is neither a notification nor a response is request-shaped. Route invalid
    // values through request validation so the peer receives -32600 instead of a silent drop.
    co_await dispatch_request(std::move(json_msg), std::move(principal));
}

Task<std::string> Server::dispatch_request_direct(nlohmann::json json_msg) {
    return impl_->dispatch_direct(std::move(json_msg), nullptr, nullptr, {}, {}, nullptr);
}

Task<std::string> Server::Impl::dispatch_direct(
    nlohmann::json json_msg, std::shared_ptr<std::atomic<bool>> cancelled,
    std::shared_ptr<const detail::ParamHeaderValues> param_headers, MessageSender notifications,
    std::function<void(std::function<void()>)> watch_cancel,
    std::shared_ptr<const std::string> principal) {
    const auto bytes = detail::json_memory_cost(json_msg);
    const auto& counters = *direct_counters;
    if (is_identified_request(json_msg) &&
        (counters.requests.load(std::memory_order_relaxed) >=
             max_in_flight_requests.load(std::memory_order_relaxed) ||
         counters.bytes.load(std::memory_order_relaxed) + bytes >
             max_in_flight_bytes.load(std::memory_order_relaxed))) {
        co_return make_error_wire(json_msg.at("id").get<RequestId>(),
                                  mcp::constants::jsonrpc::g_INTERNAL_ERROR,
                                  "Too many requests in flight");
    }
    auto in_flight = std::make_shared<CountedRequest>(direct_counters, bytes);
    CountAgain count_again = [counters = direct_counters](std::size_t size) -> std::shared_ptr<void> {
        return std::make_shared<CountedRequest>(counters, size);
    };
    co_return co_await dispatch_request_wire(std::move(json_msg), false, std::move(cancelled),
                                             std::move(param_headers), std::move(notifications),
                                             std::move(watch_cancel), std::move(principal),
                                             std::move(in_flight), std::move(count_again));
}

Task<std::string> Server::dispatch_request_direct(nlohmann::json json_msg,
                                                  RequestContext request_context) {
    return impl_->dispatch_with_context(std::move(json_msg), std::move(request_context));
}

Task<std::string> Server::Impl::dispatch_for_http(std::weak_ptr<Impl> weak, nlohmann::json json_msg,
                                                  RequestContext request_context) {
    auto impl = weak.lock();
    if (!impl) {
        co_return make_error_wire(json_msg.at("id").get<RequestId>(),
                                  mcp::constants::jsonrpc::g_INTERNAL_ERROR, "The server is gone");
    }
    co_return co_await impl->dispatch_with_context(std::move(json_msg), std::move(request_context));
}

Task<std::string> Server::Impl::dispatch_with_context(nlohmann::json json_msg,
                                                      RequestContext request_context) {
    std::shared_ptr<const detail::ParamHeaderValues> param_headers;
    if (request_context.param_headers) {
        param_headers = std::make_shared<const detail::ParamHeaderValues>(
            std::move(*request_context.param_headers));
    }
    std::shared_ptr<const std::string> principal;
    if (!request_context.principal.empty()) {
        principal = std::make_shared<const std::string>(std::move(request_context.principal));
    }
    co_return co_await dispatch_direct(std::move(json_msg), std::move(request_context.cancelled),
                                       std::move(param_headers),
                                       std::move(request_context.notifications),
                                       std::move(request_context.watch_cancel), std::move(principal));
}

void Server::Impl::dispatch_notification(const nlohmann::json& json_msg) {
    if (!is_valid_notification_envelope(json_msg)) {
        return;
    }
    auto method = json_msg.at("method").get<std::string>();

    if (method == methods::g_NOTIFICATIONS_INITIALIZED) {
        auto expected = Impl::LifecycleState::eAwaitingInitialized;
        lifecycle.compare_exchange_strong(expected, Impl::LifecycleState::eReady,
                                          std::memory_order_relaxed);
        return;
    }

    if (method == methods::g_NOTIFICATIONS_CANCELLED) {
        // A 2026-07-28 request needs no initialize, so cancellation does not wait for one either.
        // Before initialize a legacy client has nothing in flight to cancel.
        auto session = session_snapshot();
        if (!session) {
            return;
        }
        if (!json_msg.contains("params")) {
            return;
        }
        CancelledNotificationParams params;
        try {
            params = json_msg.at("params").get<CancelledNotificationParams>();
        } catch (const std::exception&) {
            return;
        }
        auto id_str = params.requestId.correlation_key();

        auto it = session->in_flight.find(id_str);
        if (it != session->in_flight.end()) {
            it->second->store(true, std::memory_order_relaxed);
            // A handler waiting for the client cannot look at its flag. Without this it would stay
            // parked until the reverse request timed out, counting against the limit on requests.
            for (auto& [pending_id, pending] : session->pending_requests) {
                if (pending.caller_cancelled == it->second && !pending.completed) {
                    pending.error = Error{
                        .code = mcp::constants::jsonrpc::g_REQUEST_CANCELLED,
                        .message = "The request was cancelled",
                        .data = std::nullopt,
                    };
                    pending.completed = true;
                    pending.given_up = true;
                    pending.timer->cancel();
                }
            }
        }
        end_listeners(ListenEnd::eCancelled, session, &id_str);
    }
}

void Server::Impl::dispatch_response(const nlohmann::json& json_msg) const {
    auto session = session_snapshot();
    if (!is_valid_response_envelope(json_msg) || !session) {
        return;
    }

    auto id = json_msg.at("id").get<RequestId>();
    auto id_str = id.correlation_key();

    auto it = session->pending_requests.find(id_str);
    if (it == session->pending_requests.end()) {
        return;
    }
    // Ids are small numbers, so another session's client could name this one.
    if (it->second.epoch != session_epoch(*session)) {
        return;
    }

    if (has_error_member(json_msg)) {
        try {
            it->second.error = json_msg.at("error").get<Error>();
        } catch (const std::exception&) {
            // Still an answer: the request failed, and the handler must not wait on.
            it->second.error = Error{
                .code = mcp::constants::jsonrpc::g_INVALID_REQUEST,
                .message = "Malformed JSON-RPC error object",
                .data = std::nullopt,
            };
        }
    } else if (json_msg.contains("result")) {
        it->second.result = json_msg.at("result");
    }

    it->second.completed = true;
    it->second.timer->cancel();
}

Task<void> Server::Impl::send_notification(const std::string& method,
                                           const std::optional<nlohmann::json>& params) const {
    auto session = session_snapshot();
    if (!session || session->stopping.load(std::memory_order_acquire)) {
        co_return;
    }
    // [gcc11-sso: scope-before-await]
    std::string wire;
    {
        JSONRPCNotification notification;
        notification.method = method;
        notification.params = params;
        wire = mcp::detail::dump_json(nlohmann::json(std::move(notification)));
    }
    co_await session->writer->write_message(wire);
}

nlohmann::json Server::Impl::url_elicitation_error_data(const UrlElicitationRequired& required) {
    nlohmann::json elicitations = nlohmann::json::array();
    const auto session = session_snapshot();
    const auto now = ElicitationRecords::Clock::now();
    for (const auto& elicitation : required.elicitations()) {
        if (session) {
            registry->elicitations.record(*elicitation.elicitationId,
                                          ElicitationTarget{
                                              .server = weak_from_this(),
                                              .session = session,
                                              .epoch = session_epoch(*session),
                                          },
                                          now);
        }
        elicitations.push_back(elicitation);
    }
    return nlohmann::json{{"elicitations", std::move(elicitations)}};
}

Task<bool> Server::Impl::complete_elicitation(std::shared_ptr<Registry> registry,
                                              std::shared_ptr<const std::string> id) {
    auto target = registry->elicitations.take(*id, ElicitationRecords::Clock::now());
    if (!target) {
        co_return false;
    }
    co_return co_await send_elicitation_complete(std::move(*target), std::move(id));
}

Task<bool> Server::Impl::complete_own_elicitation(std::shared_ptr<Registry> registry,
                                                  ElicitationTarget target,
                                                  std::shared_ptr<const std::string> id) {
    if (!registry->elicitations.take_own(*id, target, ElicitationRecords::Clock::now())) {
        co_return false;
    }
    co_return co_await send_elicitation_complete(std::move(target), std::move(id));
}

Task<bool> Server::Impl::send_elicitation_complete(ElicitationTarget target,
                                                   std::shared_ptr<const std::string> id) {
    auto impl = target.server.lock();
    auto session = target.session.lock();
    if (!impl || !session || session->stopping.load(std::memory_order_acquire) ||
        impl->session_snapshot() != session || session_epoch(*session) != target.epoch ||
        !impl->negotiated_at_least(mcp::constants::protocol::g_PROTOCOL_VERSION_2025_11_25) ||
        !impl->client_declares_url_elicitation()) {
        co_return false;
    }
    // [gcc11-sso: scope-before-await]
    std::shared_ptr<const std::string> wire;
    {
        JSONRPCNotification notification;
        notification.method = methods::g_NOTIFICATIONS_ELICITATION_COMPLETE;
        notification.params =
            nlohmann::json(ElicitationCompleteNotificationParams{.elicitationId = *id});
        wire = std::make_shared<const std::string>(
            mcp::detail::dump_json(nlohmann::json(std::move(notification))));
    }
    try {
        co_await session->writer->write_message(*wire);
    } catch (const std::exception&) {
        // A session whose transport cannot take the message ends through that transport.
        co_return false;
    }
    co_return true;
}

Task<void> Server::Impl::send_broadcast(const std::string& method) const {
    try {
        co_await send_notification(method, std::nullopt);
    } catch (const std::exception&) {
        // A session whose transport cannot take the message ends through that transport.
    }
}

void Server::Impl::check_notification(std::string_view method,
                                      const std::optional<nlohmann::json>& params) {
    // Each has its own route: the streams that asked for it, a session or a request.
    static constexpr std::array<std::string_view, 11> s_routed = {
        methods::g_NOTIFICATIONS_TOOLS_LIST_CHANGED,
        methods::g_NOTIFICATIONS_PROMPTS_LIST_CHANGED,
        methods::g_NOTIFICATIONS_RESOURCES_LIST_CHANGED,
        methods::g_NOTIFICATIONS_RESOURCES_UPDATED,
        methods::g_NOTIFICATIONS_TASKS,
        methods::g_NOTIFICATIONS_ELICITATION_COMPLETE,
        methods::g_NOTIFICATIONS_PROGRESS,
        methods::g_NOTIFICATIONS_MESSAGE,
        methods::g_NOTIFICATIONS_CANCELLED,
        methods::g_NOTIFICATIONS_INITIALIZED,
        methods::g_NOTIFICATIONS_SUBSCRIPTIONS_ACKNOWLEDGED,
    };
    if (std::ranges::find(s_routed, method) != s_routed.end()) {
        throw std::invalid_argument("Server::notify() does not send " + std::string(method) +
                                    "; the server sends it through its own route");
    }
    if (params && !params->is_object()) {
        throw std::invalid_argument("The params of a notification must be an object");
    }
}

Task<void> Server::Impl::notify_legacy_sessions(
    std::shared_ptr<Impl> impl, std::shared_ptr<const std::string> method,
    std::shared_ptr<const std::optional<nlohmann::json>> params) {
    for (const auto& member : impl->registry_members()) {
        if (!member->began_with_initialize()) {
            continue;
        }
        try {
            co_await member->send_notification(*method, *params);
        } catch (const std::exception&) {
            // As in send_broadcast().
        }
    }
}

Task<void> Server::Impl::broadcast_resource_updated(const std::string& uri) const {
    try {
        co_await notify_resource_updated(uri);
    } catch (const std::exception&) {
        // As in send_broadcast().
    }
}

Task<void> Server::Impl::notify_resource_updated(const std::string& uri) const {
    auto session = session_snapshot();
    if (!session || session->stopping.load(std::memory_order_acquire)) {
        co_return;
    }

    auto owned_uri = std::make_shared<const std::string>(uri);
    auto strand = *session->strand;
    co_await mcp::detail::spawn_awaitable(
        strand, notify_resource_updated_on_strand(std::move(session), std::move(owned_uri)));
}

Task<void> Server::Impl::notify_resource_updated_on_strand(std::shared_ptr<Session> session,
                                                           std::shared_ptr<const std::string> uri) {
    if (session->stopping.load(std::memory_order_acquire)) {
        co_return;
    }

    auto it = session->subscriptions.find(*uri);
    if (it == session->subscriptions.end()) {
        co_return;
    }
    if (it->second != Impl::session_epoch(*session)) {
        session->subscription_bytes -= it->first.size();
        session->subscriptions.erase(it);
        co_return;
    }

    ResourceUpdatedNotificationParams params;
    params.uri = *uri;

    JSONRPCNotification notification;
    notification.method = methods::g_NOTIFICATIONS_RESOURCES_UPDATED;
    nlohmann::json p = std::move(params);
    notification.params = std::move(p);

    nlohmann::json json_msg = std::move(notification);
    co_await session->writer->write_message(mcp::detail::dump_json(json_msg));
}

}  // namespace mcp

#include "server_impl.hpp"

namespace methods = mcp::constants::methods;

namespace mcp {

Context Server::Impl::make_context(std::shared_ptr<std::atomic<bool>> cancelled,
                                   std::optional<ProgressToken> progress_token,
                                   std::shared_ptr<const RequestMetadata> request_meta,
                                   bool session_request, MessageSender notifications,
                                   std::shared_ptr<const std::string> principal,
                                   const std::shared_ptr<detail::MrtrEntry>& mrtr,
                                   const std::shared_ptr<const std::string>& related_request,
                                   const std::shared_ptr<detail::InlineInput>& inline_input,
                                   const std::shared_ptr<detail::TaskRun>& task_run) {
    ITransport* transport = &null_transport();
    auto session = session_snapshot();
    std::uint64_t epoch = 0;
    MessageSender message_sender;
    if (task_run) {
        // A task's handler has no request to carry its notifications: log messages are dropped
        // and its progress goes to the task's status message.
        message_sender = [](std::string_view) { return Impl::skip_write(); };
    } else if (mrtr) {
        // The handler may outlive the request that started it; its notifications go to whichever
        // request carries it now, with that request's progress token.
        message_sender = [mrtr](std::string_view message) {
            return detail::mrtr_notify(mrtr, std::make_shared<const std::string>(message));
        };
        if (!progress_token) {
            progress_token = ProgressToken{std::string("input-required")};
        }
    } else {
        message_sender =
            notification_route(cancelled, request_meta, session_request, std::move(notifications),
                               session, &transport, &epoch, related_request);
    }

    RequestSender request_sender;
    if (task_run) {
        request_sender = [task_run](std::string method, std::optional<nlohmann::json> params,
                                    std::optional<std::string> key) -> Task<nlohmann::json> {
            return detail::task_request_input(task_run, nlohmann::json(std::move(method)),
                                              std::move(params),
                                              key ? nlohmann::json(std::move(*key)) : nlohmann::json());
        };
    } else if (mrtr) {
        request_sender = [mrtr](std::string method, std::optional<nlohmann::json> params,
                                std::optional<std::string> key) -> Task<nlohmann::json> {
            return detail::mrtr_request_input(mrtr, nlohmann::json(std::move(method)),
                                              std::move(params),
                                              key ? nlohmann::json(std::move(*key)) : nlohmann::json());
        };
    } else if (inline_input) {
        request_sender = [inline_input](std::string method, std::optional<nlohmann::json> params,
                                        const std::optional<std::string>&) -> Task<nlohmann::json> {
            nlohmann::json request = {{"method", std::move(method)}};
            if (params) {
                request["params"] = std::move(*params);
            }
            if (!inline_input->refuse(request)) {
                throw std::runtime_error("the client can answer this request for input");
            }
            throw detail::MrtrAbandoned{};
        };
    } else if (request_meta) {
        // A server never sends a request of its own to a 2026-07-28 client.
        request_sender = [](const std::string&, const std::optional<nlohmann::json>&,
                            const std::optional<std::string>&) -> Task<nlohmann::json> {
            throw std::runtime_error(
                "server-to-client requests are unavailable for a 2026-07-28 request");
        };
    } else {
        request_sender = [impl = shared_from_this(), session, epoch, cancelled, related_request](
                             const std::string& method, const std::optional<nlohmann::json>& params,
                             const std::optional<std::string>&) -> Task<nlohmann::json> {
            if (session && Impl::session_epoch(*session) != epoch) {
                throw std::runtime_error("the client ended the session this request belongs to");
            }
            if (method == methods::g_SAMPLING_CREATE_MESSAGE && params &&
                detail::asks_for_sampling_context(*params) &&
                !impl->client_declares_sampling_context()) {
                throw std::invalid_argument(
                    "includeContext other than \"none\" needs a client that declares the "
                    "sampling.context capability");
            }
            return impl->send_request(cancelled, method, params, related_request);
        };
    }

    Context context{*transport,
                    std::move(request_sender),
                    std::move(cancelled),
                    std::move(progress_token),
                    &log_level,
                    std::move(message_sender),
                    std::move(request_meta),
                    std::move(principal),
                    mrtr ? mrtr->replaying : nullptr};
    if (notification_rate_limit.burst > 0) {
        detail::ContextRouting::set_notification_budget(
            context, std::make_shared<detail::NotificationBudget>(notification_rate_limit,
                                                                  dropped_notifications));
    }
    if (task_run) {
        context.set_progress_sink([task_run](double progress, std::optional<double> total,
                                             const std::optional<std::string>& message) {
            if (auto updated = task_run->store->set_status_message(
                    task_run->id, detail::task_progress_message(progress, total, message))) {
                task_run->publish_detached(std::make_shared<const TaskRecord>(std::move(*updated)));
            }
        });
    }
    return context;
}

MessageSender Server::Impl::notification_route(
    std::shared_ptr<std::atomic<bool>> cancelled,
    const std::shared_ptr<const RequestMetadata>& request_meta, bool session_request,
    MessageSender notifications, const std::shared_ptr<Session>& session, ITransport** transport,
    std::uint64_t* epoch, std::shared_ptr<const std::string> related_request) {
    if (request_meta && notifications) {
        // The request's own stream, supplied by the transport that carried it.
        return notifications;
    }
    // A 2026-07-28 request an HttpServerTransport queued before the server was bound has no stream
    // of its own: written to the session, its notifications would join the legacy session's stream.
    const bool notifications_have_a_route =
        !request_meta || !session || session->http_transport == nullptr;
    if (!session || !session->transport || (!session_request && request_meta) ||
        !notifications_have_a_route) {
        return {};
    }
    const auto session_epoch_now = Impl::session_epoch(*session);
    if (transport != nullptr) {
        *transport = session->transport.get();
    }
    if (epoch != nullptr) {
        *epoch = session_epoch_now;
    }
    // A handler whose session the client has ended sends nothing more: the transport would hand
    // its messages to the next client. 2026-07-28 also sends nothing for a request once it is
    // cancelled.
    std::shared_ptr<std::atomic<bool>> modern_cancelled = request_meta ? std::move(cancelled) : nullptr;
    return [session, session_epoch_now, modern_cancelled,
            related_request = std::move(related_request)](std::string_view message) {
        if (Impl::session_epoch(*session) != session_epoch_now ||
            (modern_cancelled && modern_cancelled->load(std::memory_order_relaxed))) {
            return Impl::skip_write();
        }
        if (related_request) {
            return session->writer->write_related(std::make_shared<const std::string>(message),
                                                  related_request);
        }
        return session->writer->write_message(message);
    };
}

// Invalid parameter decoding is reported as -32602. Exceptions raised after decoding are reported
// as -32603, except from a tool handler: those become a tool result carrying isError.
Task<void> Server::Impl::dispatch_request(nlohmann::json json_msg,
                                          std::shared_ptr<const std::string> principal) {
    auto session = session_snapshot();
    if (!session || !session->writer) {
        throw std::runtime_error("server dispatch requires an active session");
    }
    const auto epoch = Impl::session_epoch(*session);

    // Every request read from the session can be cancelled by notifications/cancelled. A direct
    // caller cancels through its own flag, so its ids never meet the session's.
    auto cancelled = std::make_shared<std::atomic<bool>>(false);
    std::string request_id_str;
    bool modern = false;
    // Set for a valid envelope, which dispatch_request_wire() then neither checks nor looks up the
    // protocol version of again.
    std::optional<bool> checked_modern;
    if (validate_request_envelope(json_msg) == nullptr) {
        request_id_str = json_msg.at("id").get<RequestId>().correlation_key();
        session->in_flight[request_id_str] = cancelled;
        modern = detail::is_modern_request(
            json_msg, json_msg.at("method").get_ref<const nlohmann::json::string_t&>());
        checked_modern = modern;
    }
    const auto forget = [&session, &request_id_str, &cancelled] {
        // A later request reusing the id replaced the entry; that one is not ours to remove.
        auto it = session->in_flight.find(request_id_str);
        if (it != session->in_flight.end() && it->second == cancelled) {
            session->in_flight.erase(it);
        }
    };

    std::string wire;
    try {
        // A handler given up while it waited for client input, which then goes on, counts
        // against this session's limits again.
        CountAgain count_again =
            [weak = std::weak_ptr<Session>(session)](std::size_t size) -> std::shared_ptr<void> {
            auto owner = weak.lock();
            if (!owner) {
                return nullptr;
            }
            return std::make_shared<CountedSessionWork>(std::move(owner), size);
        };
        wire = co_await dispatch_request_wire(std::move(json_msg), true, cancelled, nullptr, {}, {},
                                              std::move(principal), nullptr, std::move(count_again),
                                              checked_modern);
    } catch (...) {
        forget();
        throw;
    }
    forget();
    // A 2026-07-28 request belongs to no session, so a session ending does not orphan it.
    if (!modern && Impl::session_epoch(*session) != epoch) {
        // The client ended this request's session while it ran; the answer is for nobody now.
        co_return;
    }
    if (modern && cancelled->load(std::memory_order_relaxed)) {
        // 2026-07-28 sends nothing more for a cancelled request, not even its response.
        co_return;
    }
    if (wire.empty()) {
        // The handler wrote its answer itself (a subscription the server closed).
        co_return;
    }
    co_await session->writer->write_message(wire);
}

Task<std::string> Server::Impl::dispatch_request_wire(
    nlohmann::json incoming, bool enforce_lifecycle, std::shared_ptr<std::atomic<bool>> cancelled,
    std::shared_ptr<const detail::ParamHeaderValues> param_headers, MessageSender notifications,
    std::function<void(std::function<void()>)> watch_cancel,
    std::shared_ptr<const std::string> principal, std::shared_ptr<void> in_flight,
    CountAgain count_again, std::optional<bool> checked_modern) {
    // A handler that outlives the request (a parked multi round-trip request) keeps the message
    // through this pointer; moving the message into it copies nothing.
    const auto message = std::make_shared<const nlohmann::json>(std::move(incoming));
    const nlohmann::json& json_msg = *message;
    if (!checked_modern) {
        const char* validation_error = validate_request_envelope(json_msg);
        if (validation_error != nullptr) {
            co_return make_invalid_request_wire(json_msg, validation_error);
        }
    }
    serving.store(true, std::memory_order_release);

    // [gcc11-sso: string_view] DO NOT change to std::string.
    std::string_view method = json_msg.at("method").get_ref<const nlohmann::json::string_t&>();

    // [gcc11-sso: scope-before-await] DO NOT use optional<string> for error state.
    nlohmann::json error_payload;
    int error_code = mcp::constants::jsonrpc::g_INTERNAL_ERROR;

    try {
        // A request naming its protocol version in _meta is served on its own, without
        // initialize, and leaves the lifecycle and log level of a session untouched.
        if (checked_modern ? *checked_modern : detail::is_modern_request(json_msg, method)) {
            // [gcc11-sso: named-aggregate-arg] DO NOT pass RequestScope{...} inside the co_await.
            RequestScope scope{
                .meta = nullptr,
                .cancelled = std::move(cancelled),
                .session_request = enforce_lifecycle,
                .param_headers = std::move(param_headers),
                .notifications = std::move(notifications),
                .watch_cancel = std::move(watch_cancel),
                .principal = std::move(principal),
            };
            scope.in_flight = std::move(in_flight);
            scope.count_again = std::move(count_again);
            scope.message = message;
            co_return co_await dispatch_modern_wire(json_msg, method, std::move(scope));
        }

        if (!protocol_policy.serve_legacy) {
            legacy_shaped_rejections.fetch_add(1, std::memory_order_relaxed);
            if (method == methods::g_INITIALIZE) {
                // initialize does not exist in the revisions this server serves; the error still
                // names them, as an initialize error should.
                co_return initialize_not_served_wire(json_msg.at("id").get<RequestId>());
            }
            co_return make_error_wire(
                json_msg.at("id").get<RequestId>(), mcp::constants::jsonrpc::g_INVALID_PARAMS,
                "Missing " + std::string(mcp::constants::meta::g_META_PROTOCOL_VERSION) + " in _meta");
        }

        RequestScope legacy_scope{
            .meta = nullptr,
            .cancelled = std::move(cancelled),
            .session_request = enforce_lifecycle,
            .param_headers = nullptr,
            .notifications = {},
            .watch_cancel = {},
            .principal = std::move(principal),
        };
        if (enforce_lifecycle) {
            legacy_scope.related_request =
                std::make_shared<const std::string>(mcp::detail::dump_json(json_msg.at("id")));
        }

        // A client that arrives after the previous one ended its session starts uninitialized.
        auto session_state = LifecycleState::eReady;
        if (enforce_lifecycle) {
            session_state = transport_started_new_session() ? LifecycleState::eUninitialized
                                                            : lifecycle.load(std::memory_order_relaxed);
        }

        if (method == methods::g_INITIALIZE) {
            if (enforce_lifecycle && session_state != LifecycleState::eUninitialized) {
                co_return make_error_wire(json_msg.at("id").get<RequestId>(),
                                          mcp::constants::jsonrpc::g_INVALID_REQUEST,
                                          "Server has already been initialized");
            }
            co_return co_await handle_initialize_wire(json_msg, enforce_lifecycle);
        }

        if (method == methods::g_PING) {
            co_return co_await handle_ping_wire(json_msg);
        }

        if (method == methods::g_SERVER_DISCOVER) {
            // server/discover belongs to 2026-07-28 and requires its per-request fields.
            legacy_shaped_rejections.fetch_add(1, std::memory_order_relaxed);
            co_return make_error_wire(
                json_msg.at("id").get<RequestId>(), mcp::constants::jsonrpc::g_INVALID_PARAMS,
                "server/discover requires " +
                    std::string(mcp::constants::meta::g_META_PROTOCOL_VERSION) + " in _meta");
        }

        if (session_state != LifecycleState::eReady) {
            co_return make_error_wire(
                json_msg.at("id").get<RequestId>(), mcp::constants::jsonrpc::g_INVALID_REQUEST,
                "Server is not ready; initialize and send notifications/initialized first");
        }

        if (method == "shutdown") {
            co_return co_await handle_shutdown_wire(json_msg);
        } else if (method == methods::g_TOOLS_CALL) {
            co_return co_await handle_tools_call_wire(json_msg, legacy_scope);
        } else if (method == methods::g_TOOLS_LIST) {
            co_return co_await handle_tools_list_wire(json_msg, legacy_scope);
        } else if (method == methods::g_RESOURCES_LIST) {
            co_return co_await handle_resources_list_wire(json_msg, legacy_scope);
        } else if (method == methods::g_RESOURCES_READ) {
            co_return co_await handle_resources_read_wire(json_msg, legacy_scope);
        } else if (method == methods::g_RESOURCES_TEMPLATES_LIST) {
            co_return co_await handle_resource_templates_list_wire(json_msg, legacy_scope);
        } else if (method == methods::g_RESOURCES_SUBSCRIBE) {
            co_return co_await handle_subscribe_wire(json_msg);
        } else if (method == methods::g_RESOURCES_UNSUBSCRIBE) {
            co_return co_await handle_unsubscribe_wire(json_msg);
        } else if (method == methods::g_PROMPTS_LIST) {
            co_return co_await handle_prompts_list_wire(json_msg, legacy_scope);
        } else if (method == methods::g_PROMPTS_GET) {
            co_return co_await handle_prompts_get_wire(json_msg, legacy_scope);
        } else if (method == methods::g_LOGGING_SET_LEVEL) {
            co_return co_await handle_set_level_wire(json_msg);
        } else if (method == methods::g_COMPLETION_COMPLETE) {
            co_return co_await handle_complete_wire(json_msg, legacy_scope);
        } else {
            co_return make_error_wire(json_msg.at("id").get<RequestId>(),
                                      mcp::constants::jsonrpc::g_METHOD_NOT_FOUND,
                                      "Method not found: " + std::string(method));
        }
    } catch (const InvalidParamsError& error) {
        error_code = mcp::constants::jsonrpc::g_INVALID_PARAMS;
        error_payload = error.what();
    } catch (const nlohmann::json::exception&) {
        // Its text describes the library, and quotes bytes of the value that could not be encoded.
        if (json_msg.contains("id")) {
            error_payload = "The result could not be encoded as JSON";
        }
    } catch (const std::exception& error) {
        if (json_msg.contains("id")) {
            // Handler text can carry build paths or third-party library detail, and reaches the
            // peer verbatim from here.
            error_payload = detail::sanitize_for_diagnostics(error.what());
        }
    } catch (...) {
        // Middleware and handlers can throw anything, and an escaping throw leaves the request
        // unanswered. The text is ours rather than the thrower's, so there is nothing to sanitize.
        if (json_msg.contains("id")) {
            error_payload = "Internal error";
        }
    }

    if (!error_payload.is_null()) {
        co_return make_error_wire(json_msg.at("id").get<RequestId>(), error_code,
                                  error_payload.get<std::string>());
    }

    co_return make_error_wire(json_msg.at("id").get<RequestId>(),
                              mcp::constants::jsonrpc::g_INTERNAL_ERROR,
                              "Request produced no response");
}

// Checks run in the order that gives the client the most to act on: a version the server does not
// serve first (the other fields may differ in that version), then the method, then the fields.
// Exceptions propagate to dispatch_request_wire, which maps them as for any request.
Task<std::string> Server::Impl::dispatch_modern_wire(const nlohmann::json& json_msg,
                                                     std::string_view method, RequestScope scope) {
    const auto* version = detail::requested_protocol_version(json_msg);
    if (version == nullptr) {
        invalid_metadata_rejections.fetch_add(1, std::memory_order_relaxed);
        co_return make_error_wire(
            json_msg.at("id").get<RequestId>(), mcp::constants::jsonrpc::g_INVALID_PARAMS,
            "Invalid " + std::string(mcp::constants::meta::g_META_PROTOCOL_VERSION) + " in _meta");
    }
    if (!protocol_policy.serve_modern || !is_modern_protocol_version(*version)) {
        unsupported_version_rejections.fetch_add(1, std::memory_order_relaxed);
        co_return unsupported_version_wire(json_msg.at("id").get<RequestId>(), *version);
    }
    const bool task_method = tasks_enabled.load(std::memory_order_acquire) &&
                             (method == methods::g_TASKS_GET || method == methods::g_TASKS_UPDATE ||
                              method == methods::g_TASKS_CANCEL);
    const auto extension = extension_methods.find(method);
    if (!task_method && extension == extension_methods.end() && !detail::is_modern_method(method)) {
        co_return make_error_wire(json_msg.at("id").get<RequestId>(),
                                  mcp::constants::jsonrpc::g_METHOD_NOT_FOUND,
                                  "Method not found: " + std::string(method));
    }
    {
        auto parsed = detail::parse_request_meta(
            scope.message ? scope.message : std::make_shared<const nlohmann::json>(json_msg));
        if (!parsed.metadata) {
            invalid_metadata_rejections.fetch_add(1, std::memory_order_relaxed);
            co_return make_error_wire(json_msg.at("id").get<RequestId>(),
                                      mcp::constants::jsonrpc::g_INVALID_PARAMS,
                                      std::move(parsed.error));
        }
        scope.meta = std::move(parsed.metadata);
    }

    if (task_method) {
        co_return co_await handle_task_wire(json_msg, method, scope);
    }
    if (extension != extension_methods.end()) {
        co_return co_await handle_extension_wire(json_msg, scope, extension->second);
    }
    if (method == methods::g_TOOLS_CALL || method == methods::g_RESOURCES_READ ||
        method == methods::g_PROMPTS_GET) {
        co_return co_await dispatch_mrtr(json_msg, method, std::move(scope));
    }
    if (method == methods::g_SERVER_DISCOVER) {
        co_return co_await handle_discover_wire(json_msg, scope);
    } else if (method == methods::g_TOOLS_LIST) {
        co_return co_await handle_tools_list_wire(json_msg, scope);
    } else if (method == methods::g_RESOURCES_LIST) {
        co_return co_await handle_resources_list_wire(json_msg, scope);
    } else if (method == methods::g_RESOURCES_TEMPLATES_LIST) {
        co_return co_await handle_resource_templates_list_wire(json_msg, scope);
    } else if (method == methods::g_PROMPTS_LIST) {
        co_return co_await handle_prompts_list_wire(json_msg, scope);
    } else if (method == methods::g_SUBSCRIPTIONS_LISTEN) {
        co_return co_await handle_listen_wire(json_msg, scope);
    }
    co_return co_await handle_complete_wire(json_msg, scope);
}

namespace {

/// The JSON text @p result serializes to. Each thread keeps the last result it serialized and
/// reuses the text while the next result equals it: a session manager's factory and run_http()
/// build the same server for every session, whose initialize results are then alike. Equal JSON
/// values in the capabilities may write a number differently (1 and 1.0), which is still the same
/// JSON number.
const std::string& initialize_result_text(InitializeResult result) {
    struct LastResult {
        std::optional<InitializeResult> result;
        std::string text;
    };
    thread_local LastResult s_last;
    if (!s_last.result || *s_last.result != result) {
        std::string text;
        detail::append_json(text, nlohmann::json(result));
        s_last.text = std::move(text);
        s_last.result = std::move(result);
    }
    return s_last.text;
}

}  // namespace

Task<std::string> Server::Impl::handle_initialize_wire(const nlohmann::json& json_msg,
                                                       bool update_lifecycle) {
    auto initialize_request =
        deserialize_request_params<InitializeRequest>(json_msg, methods::g_INITIALIZE);

    InitializeResult init_result;
    init_result.protocolVersion =
        negotiate_protocol_version(initialize_request.protocolVersion, protocol_policy);
    init_result.capabilities = capabilities;
    init_result.serverInfo = server_info;
    init_result.instructions = instructions;

    if (update_lifecycle) {
        {
            const auto& params = json_msg.at("params");
            const auto declared = params.find("capabilities");
            std::scoped_lock lock(client_capabilities_mutex);
            client_capabilities = declared != params.end() && declared->is_object()
                                      ? *declared
                                      : nlohmann::json::object();
        }
        if (auto current = session_snapshot(); current && current->http_transport != nullptr) {
            const auto epoch = session_epoch(*current);
            if (epoch != terminated_sessions_at_initialize.load(std::memory_order_relaxed)) {
                // A client starting a new session gets the server's default level, not the one the
                // client before it chose.
                log_level.store(g_default_log_level, std::memory_order_relaxed);
            }
            terminated_sessions_at_initialize.store(epoch, std::memory_order_relaxed);
        }
        lifecycle.store(Impl::LifecycleState::eAwaitingInitialized, std::memory_order_relaxed);
    }
    auto wire = make_result_text_wire(json_msg.at("id").get<RequestId>(),
                                      initialize_result_text(std::move(init_result)));
    co_return wire;
}

Task<std::string> Server::Impl::handle_shutdown_wire(const nlohmann::json& json_msg) {
    shutdown_requested.store(true, std::memory_order_relaxed);
    co_return make_result_wire(json_msg.at("id").get<RequestId>(), nlohmann::json::object());
}

Task<std::string> Server::Impl::handle_ping_wire(const nlohmann::json& json_msg) {
    co_return make_result_wire(json_msg.at("id").get<RequestId>(), nlohmann::json::object());
}

// Reached only as a 2026-07-28 request whose `_meta` dispatch_modern_wire has already checked.
// It needs no prior state and is idempotent: it reads nothing else from the request and never
// changes a session's lifecycle.
Task<std::string> Server::Impl::handle_discover_wire(const nlohmann::json& json_msg,
                                                     const RequestScope& scope) const {
    DiscoverResult discover_result;
    discover_result.supportedVersions = supported_versions();
    discover_result.capabilities = capabilities;
    if (tasks_enabled.load(std::memory_order_acquire)) {
        mcp::declare_extension(discover_result.capabilities, mcp::constants::tasks::g_EXTENSION_ID);
    }
    discover_result.instructions = instructions;
    co_return make_result_wire(json_msg.at("id").get<RequestId>(), scope,
                               nlohmann::json(discover_result), CacheTarget::eDiscover);
}

Task<std::string> Server::Impl::handle_extension_wire(const nlohmann::json& json_msg,
                                                      const RequestScope& scope,
                                                      const TypeErasedHandler& handler) {
    const auto& params = json_msg.at("params");
    auto ctx = make_context(scope.cancelled, inline_progress_token(scope), scope.meta,
                            scope.session_request, scope.notifications, scope.principal, scope.mrtr,
                            scope.related_request, scope.inline_input);
    std::string invalid_params;
    nlohmann::json handler_result;
    try {
        handler_result = co_await call_through_middleware(handler, ctx, params);
    } catch (const std::invalid_argument& error) {
        invalid_params = detail::sanitize_for_diagnostics(error.what());
    }
    if (!invalid_params.empty()) {
        co_return make_error_wire(json_msg.at("id").get<RequestId>(),
                                  mcp::constants::jsonrpc::g_INVALID_PARAMS, std::move(invalid_params));
    }
    co_return make_result_wire(json_msg.at("id").get<RequestId>(), scope, std::move(handler_result));
}

namespace {

/// Calls @p handler. It is a named coroutine, not a lambda handed to co_spawn, so what it holds
/// lives in its own frame; the caller keeps @p context and @p arguments alive until it returns.
Task<nlohmann::json> call_handler(const TypeErasedHandler& handler, Context& context,
                                  const nlohmann::json& arguments) {
    co_return co_await handler(context, arguments);
}

}  // namespace

Task<nlohmann::json> Server::Impl::call_on_handler_strand(const TypeErasedHandler& handler,
                                                          Context& ctx,
                                                          const nlohmann::json& arguments) {
    if (handler_executor) {
        const auto loop = co_await boost::asio::this_coro::executor;
        detail::ContextRouting::through_loop(ctx, loop);
        co_return co_await detail::run_on_pool(*handler_executor,
                                               call_handler(handler, ctx, arguments));
    }
    auto executor = co_await boost::asio::this_coro::executor;
    if (detail::is_strand_executor(executor)) {
        co_return co_await handler(ctx, arguments);
    }
    co_return co_await detail::spawn_awaitable(detail::make_strand_executor(executor),
                                               call_handler(handler, ctx, arguments));
}

Task<nlohmann::json> Server::Impl::call_tool_handler(const ToolRegistration& tool, Context& ctx,
                                                     const nlohmann::json& arguments) {
    // Arguments that do not conform to the input schema are a tool execution error too, so the
    // check runs here, in place of the handler.
    if (tool_validation.validate_arguments) {
        if (auto rejection = argument_rejection(tool.schemas.input, arguments)) {
            argument_rejections.fetch_add(1, std::memory_order_relaxed);
            co_return std::move(*rejection);
        }
    }
    try {
        // A sync handler is called in place when the call runs on its strand already; otherwise it
        // reaches the strand through its lifted coroutine, as an async handler does.
        if (tool.sync_handler && !handler_executor) {
            const auto executor = co_await boost::asio::this_coro::executor;
            if (detail::is_strand_executor(executor)) {
                co_return tool.sync_handler(ctx, arguments);
            }
        }
        co_return co_await call_on_handler_strand(tool.handler, ctx, arguments);
    } catch (const std::exception& error) {
        co_return nlohmann::json(
            make_tool_error_result(detail::sanitize_for_diagnostics(error.what())));
    } catch (...) {
        // A tool that fails reports a tool error whatever it threw, rather than the -32603
        // "Internal error" that dispatch_request_wire and run_mrtr_handler answer with for
        // everything else. The text is ours rather than the thrower's, so there is nothing to
        // sanitize.
        co_return nlohmann::json(make_tool_error_result("Tool handler failed"));
    }
}

void Server::Impl::finish_tool_result(const ToolRegistration& tool, const std::string& name,
                                      nlohmann::json& handler_result, bool modern) {
    if (tool.result_mode == detail::ToolResultMode::eValidated) {
        validate_call_tool_result(handler_result, modern);
    } else if (tool.result_mode == detail::ToolResultMode::eTyped) {
        validate_call_tool_result(handler_result, modern, false);
    } else {
        handler_result = normalize_structured_tool_result(std::move(handler_result), modern);
    }

    const bool is_error_result = handler_result.value("isError", false);
    if (tool.schemas.output && !is_error_result && !handler_result.contains("structuredContent")) {
        throw std::invalid_argument("Tool with outputSchema must return structuredContent: " + name);
    }
    if (tool.schemas.output && !is_error_result && tool_validation.validate_structured_content) {
        try {
            check_structured_content(*tool.schemas.output, name, handler_result);
        } catch (const std::runtime_error&) {
            structured_content_rejections.fetch_add(1, std::memory_order_relaxed);
            throw;
        }
    }
}

Task<nlohmann::json> Server::Impl::invoke_tool_direct(
    const std::string& name, const nlohmann::json& arguments,
    std::shared_ptr<std::atomic<bool>> cancelled, std::optional<ProgressToken> progress_token,
    std::shared_ptr<const RequestMetadata> request_meta, bool session_request,
    MessageSender notifications, std::shared_ptr<const std::string> principal,
    std::shared_ptr<detail::MrtrEntry> mrtr, std::shared_ptr<const std::string> related_request,
    std::shared_ptr<detail::InlineInput> inline_input, std::shared_ptr<detail::TaskRun> task_run) {
    const auto iter = tool_handlers.find(name);
    if (iter == tool_handlers.end()) {
        throw std::runtime_error("Unknown tool: " + detail::sanitize_for_diagnostics(name));
    }
    auto ctx =
        make_context(std::move(cancelled), std::move(progress_token), std::move(request_meta),
                     session_request, std::move(notifications), std::move(principal), std::move(mrtr),
                     std::move(related_request), std::move(inline_input), std::move(task_run));
    const bool modern = ctx.request_meta() != nullptr;
    nlohmann::json handler_result = co_await call_tool_handler(iter->second, ctx, arguments);
    finish_tool_result(iter->second, name, handler_result, modern);
    co_return std::move(handler_result);
}

Task<nlohmann::json> Server::Impl::invoke_tool_impl(
    CallToolParams params, std::shared_ptr<std::atomic<bool>> cancelled,
    std::optional<ProgressToken> progress_token, std::shared_ptr<const RequestMetadata> request_meta,
    bool session_request, MessageSender notifications, std::shared_ptr<const std::string> principal,
    std::shared_ptr<detail::MrtrEntry> mrtr, std::shared_ptr<const std::string> related_request,
    std::shared_ptr<detail::InlineInput> inline_input, std::shared_ptr<detail::TaskRun> task_run) {
    if (middlewares.empty()) {
        co_return co_await invoke_tool_direct(
            params.name, params.arguments, std::move(cancelled), std::move(progress_token),
            std::move(request_meta), session_request, std::move(notifications), std::move(principal),
            std::move(mrtr), std::move(related_request), std::move(inline_input), std::move(task_run));
    }

    auto iter = tool_handlers.find(params.name);
    if (iter == tool_handlers.end()) {
        // The name is chosen by whoever called: over JSON-RPC that is the client, and through the
        // public `invoke_tool` entry point it is whatever text the embedding passed in. Flatten and
        // bound it, or a name carrying CR/LF forges a line in the operator's log.
        throw std::runtime_error("Unknown tool: " + detail::sanitize_for_diagnostics(params.name));
    }

    auto ctx =
        make_context(std::move(cancelled), std::move(progress_token), std::move(request_meta),
                     session_request, std::move(notifications), std::move(principal), std::move(mrtr),
                     std::move(related_request), std::move(inline_input), std::move(task_run));
    const bool modern = ctx.request_meta() != nullptr;
    nlohmann::json handler_result;

    // Middleware is not the tool: it decides whether the call may proceed at all, so it runs
    // outside the catch of call_tool_handler and a middleware failure surfaces as a JSON-RPC error
    // instead of an isError result.
    TypeErasedHandler wrapped_handler = [this, tool = &iter->second](
                                            Context& inner_ctx,
                                            const nlohmann::json& full_params) -> Task<nlohmann::json> {
        auto call_params = full_params.get<CallToolParams>();
        co_return co_await call_tool_handler(*tool, inner_ctx, call_params.arguments);
    };
    auto handler = build_middleware_chain(std::move(wrapped_handler));
    nlohmann::json params_json = params;
    try {
        handler_result = co_await handler(ctx, params_json);
    } catch (const detail::MiddlewareRejection& rejection) {
        handler_result = nlohmann::json(make_tool_error_result(rejection.what()));
    }

    finish_tool_result(iter->second, params.name, handler_result, modern);
    co_return std::move(handler_result);
}

Task<nlohmann::json> Server::Impl::call_task_preparation(const ToolRegistration& tool, Context& ctx,
                                                         const nlohmann::json& arguments,
                                                         std::optional<TaskPreparation>& prepared) {
    // The preparation sees only arguments that conform to the input schema, as a handler does.
    if (tool_validation.validate_arguments) {
        if (auto rejection = argument_rejection(tool.schemas.input, arguments)) {
            argument_rejections.fetch_add(1, std::memory_order_relaxed);
            co_return std::move(*rejection);
        }
    }
    const TypeErasedHandler run = [&tool, &prepared](
                                      Context& inner_ctx,
                                      const nlohmann::json& inner_arguments) -> Task<nlohmann::json> {
        auto preparation = co_await tool.task_preparation(inner_ctx, inner_arguments);
        // Arguments, not a result, leave the middleware an empty but well-formed result.
        nlohmann::json answer =
            nlohmann::json(preparation.result ? *preparation.result : CallToolResult{});
        prepared = std::move(preparation);
        co_return std::move(answer);
    };
    try {
        co_return co_await call_on_handler_strand(run, ctx, arguments);
    } catch (const std::exception& error) {
        co_return nlohmann::json(
            make_tool_error_result(detail::sanitize_for_diagnostics(error.what())));
    } catch (...) {
        co_return nlohmann::json(make_tool_error_result("Tool handler failed"));
    }
}

Task<nlohmann::json> Server::Impl::prepare_task_call(const ToolRegistration& tool,
                                                     CallToolParams params,
                                                     std::shared_ptr<std::atomic<bool>> cancelled,
                                                     std::optional<ProgressToken> progress_token,
                                                     const RequestScope& scope,
                                                     std::optional<TaskPreparation>& prepared) {
    auto ctx = make_context(std::move(cancelled), std::move(progress_token), scope.meta,
                            scope.session_request, scope.notifications, scope.principal, scope.mrtr,
                            scope.related_request, scope.inline_input);
    if (middlewares.empty()) {
        co_return co_await call_task_preparation(tool, ctx, params.arguments, prepared);
    }

    // Like the middleware around a tool's handler, this runs outside the catch of the preparation,
    // so a middleware failure surfaces as a JSON-RPC error and a rejection as an isError result.
    TypeErasedHandler wrapped = [this, &tool, &prepared](
                                    Context& inner_ctx,
                                    const nlohmann::json& full_params) -> Task<nlohmann::json> {
        auto call_params = full_params.get<CallToolParams>();
        co_return co_await call_task_preparation(tool, inner_ctx, call_params.arguments, prepared);
    };
    auto handler = build_middleware_chain(std::move(wrapped));
    nlohmann::json params_json = params;
    nlohmann::json answer;
    try {
        answer = co_await handler(ctx, params_json);
    } catch (const detail::MiddlewareRejection& rejection) {
        prepared.reset();
        answer = nlohmann::json(make_tool_error_result(rejection.what()));
    }
    co_return std::move(answer);
}

Task<std::string> Server::Impl::handle_tools_call_wire(const nlohmann::json& json_msg,
                                                       const RequestScope& scope) {
    const auto call = view_call_tool_params(json_msg);
    const auto tool = tool_handlers.find(*call.name);
    if (tool == tool_handlers.end()) {
        // This, not the throw in invoke_tool_impl, is the site a remote client actually reaches:
        // the RPC path refuses before dispatch. The tools spec lists an unknown tool as -32602
        // (Invalid params). The name is entirely the client's, so flatten and bound it before it
        // reaches the operator's log.
        co_return make_error_wire(json_msg.at("id").get<RequestId>(),
                                  mcp::constants::jsonrpc::g_INVALID_PARAMS,
                                  "Unknown tool: " + detail::sanitize_for_diagnostics(*call.name));
    }
    // An HTTP request mirrors the arguments the tool's x-mcp-header annotations name into
    // Mcp-Param-* headers, and those must match the body it is executed from.
    if (scope.meta != nullptr && scope.param_headers != nullptr) {
        if (auto mismatch = detail::param_header_mismatch(tool->second.schemas.param_headers->headers,
                                                          *call.arguments, *scope.param_headers)) {
            co_return make_error_wire(json_msg.at("id").get<RequestId>(),
                                      mcp::constants::jsonrpc::g_HEADER_MISMATCH,
                                      "Header mismatch: " + *mismatch);
        }
    }

    const auto progress_token_of = [&call]() -> std::optional<ProgressToken> {
        try {
            if (call.meta != nullptr && call.meta->contains("progressToken")) {
                return call.meta->at("progressToken").get<ProgressToken>();
            }
        } catch (const nlohmann::json::exception& error) {
            throw InvalidParamsError("Invalid tools/call params: " + std::string(error.what()));
        } catch (const std::invalid_argument& error) {
            throw InvalidParamsError("Invalid tools/call params: " + std::string(error.what()));
        }
        return std::nullopt;
    };

    if (tasks_enabled.load(std::memory_order_acquire) &&
        tool->second.task_mode != ToolTaskMode::eNever) {
        if (declares_extension(scope.meta.get(), mcp::constants::tasks::g_EXTENSION_ID)) {
            auto executor = co_await boost::asio::this_coro::executor;
            if (!tool->second.task_preparation) {
                co_return start_task(json_msg, scope, call, *call.arguments, executor);
            }
            {
                // A client at its limit is told before it answers the preparation's questions.
                std::scoped_lock runs_lock(task_runs_mutex);
                if (task_limit_reached_locked(scope.principal ? *scope.principal : std::string())) {
                    co_return make_error_wire(json_msg.at("id").get<RequestId>(),
                                              mcp::constants::jsonrpc::g_INTERNAL_ERROR,
                                              "Too many tasks for this principal");
                }
            }
            auto prepared_progress = progress_token_of();
            auto prepared_cancelled =
                scope.cancelled ? scope.cancelled : std::make_shared<std::atomic<bool>>(false);
            CallToolParams prepared_params{
                .name = *call.name,
                .arguments = *call.arguments,
                .meta = call.meta != nullptr ? std::optional<nlohmann::json>(*call.meta) : std::nullopt,
            };
            std::optional<TaskPreparation> prepared;
            nlohmann::json prepared_answer =
                co_await prepare_task_call(tool->second, std::move(prepared_params), prepared_cancelled,
                                           std::move(prepared_progress), scope, prepared);
            if (prepared && !prepared->result) {
                if (prepared_cancelled->load(std::memory_order_relaxed)) {
                    // A cancelled request creates no task.
                    co_return make_error_wire(json_msg.at("id").get<RequestId>(),
                                              mcp::constants::jsonrpc::g_INTERNAL_ERROR,
                                              "Request cancelled");
                }
                co_return start_task(json_msg, scope, call, prepared->arguments, executor);
            }
            finish_tool_result(tool->second, *call.name, prepared_answer, true);
            co_return make_result_wire(json_msg.at("id").get<RequestId>(), scope,
                                       std::move(prepared_answer));
        }
        if (tool->second.task_mode == ToolTaskMode::eRequired) {
            co_return missing_tasks_extension_wire(json_msg.at("id").get<RequestId>());
        }
    }

    auto progress_token = progress_token_of();

    auto cancelled = scope.cancelled ? scope.cancelled : std::make_shared<std::atomic<bool>>(false);
    nlohmann::json handler_result;
    if (middlewares.empty()) {
        handler_result = co_await invoke_tool_direct(
            *call.name, *call.arguments, std::move(cancelled), std::move(progress_token), scope.meta,
            scope.session_request, scope.notifications, scope.principal, scope.mrtr,
            scope.related_request, scope.inline_input);
    } else {
        // Middleware sees, and may rewrite, the whole parameters object.
        CallToolParams params{
            .name = *call.name,
            .arguments = *call.arguments,
            .meta = call.meta != nullptr ? std::optional<nlohmann::json>(*call.meta) : std::nullopt,
        };
        handler_result = co_await invoke_tool_impl(
            std::move(params), std::move(cancelled), std::move(progress_token), scope.meta,
            scope.session_request, scope.notifications, scope.principal, scope.mrtr,
            scope.related_request, scope.inline_input);
    }
    co_return make_result_wire(json_msg.at("id").get<RequestId>(), scope, std::move(handler_result));
}

Task<std::string> Server::Impl::handle_tools_list_wire(const nlohmann::json& json_msg,
                                                       const RequestScope& scope) {
    auto page = paginate(tools.size(), json_msg);
    if (!page) {
        co_return make_error_wire(json_msg.at("id").get<RequestId>(),
                                  mcp::constants::jsonrpc::g_INVALID_PARAMS,
                                  "Invalid pagination cursor");
    }

    auto [begin, end, next_cursor] = *page;
    const auto listed_text =
        begin == 0 && end == tools.size() && !next_cursor ? listed_tools_text() : nullptr;
    if (listed_text) {
        // Every tool is listed: the tools are spliced into the answer as the text they serialize
        // to, in place of a copy of each. The placeholder is one string no other member of the
        // result can equal, and the splice is made only if it is found exactly once.
        constexpr std::string_view placeholder = R"("\u0001mcp-tools-list")";
        nlohmann::json list_result = nlohmann::json::object();
        list_result["tools"] = "\x01mcp-tools-list";
        auto wire = make_result_wire(json_msg.at("id").get<RequestId>(), scope, std::move(list_result),
                                     CacheTarget::eToolsList);
        const auto first = wire.find(placeholder);
        if (first != std::string::npos && wire.rfind(placeholder) == first) {
            std::string spliced;
            spliced.reserve(wire.size() + listed_text->size() + 2);
            spliced.append(wire, 0, first);
            spliced += '[';
            spliced += *listed_text;
            spliced += ']';
            spliced.append(wire, first + placeholder.size(), std::string::npos);
            co_return spliced;
        }
    }

    // The members ListToolsResult serializes, built from the registered tools rather than a copy.
    nlohmann::json tools_json = nlohmann::json::array();
    for (auto index = begin; index < end; ++index) {
        tools_json.push_back(tools[index].listed_json());
    }
    nlohmann::json list_result = nlohmann::json::object();
    list_result["tools"] = std::move(tools_json);
    if (next_cursor) {
        list_result["nextCursor"] = std::move(*next_cursor);
    }

    co_return make_result_wire(json_msg.at("id").get<RequestId>(), scope, std::move(list_result),
                               CacheTarget::eToolsList);
}

Task<std::string> Server::Impl::handle_resources_list_wire(const nlohmann::json& json_msg,
                                                           const RequestScope& scope) {
    auto page = paginate(resources.size(), json_msg);
    if (!page) {
        co_return make_error_wire(json_msg.at("id").get<RequestId>(),
                                  mcp::constants::jsonrpc::g_INVALID_PARAMS,
                                  "Invalid pagination cursor");
    }

    ListResourcesResult list_result;
    auto [begin, end, next_cursor] = *page;
    list_result.resources.assign(resources.begin() + static_cast<std::ptrdiff_t>(begin),
                                 resources.begin() + static_cast<std::ptrdiff_t>(end));
    list_result.nextCursor = std::move(next_cursor);

    co_return make_result_wire(json_msg.at("id").get<RequestId>(), scope,
                               nlohmann::json(std::move(list_result)), CacheTarget::eResourcesList);
}

Task<std::string> Server::Impl::handle_resources_read_wire(const nlohmann::json& json_msg,
                                                           const RequestScope& scope) {
    auto params =
        deserialize_request_params<ReadResourceRequestParams>(json_msg, methods::g_RESOURCES_READ);
    auto iter = resource_handlers.find(params.uri);
    if (iter != resource_handlers.end()) {
        nlohmann::json params_json = params;
        auto ctx = make_context(scope.cancelled, inline_progress_token(scope), scope.meta,
                                scope.session_request, scope.notifications, scope.principal, scope.mrtr,
                                scope.related_request, scope.inline_input);
        nlohmann::json handler_result =
            co_await call_through_middleware(iter->second, ctx, params_json);
        co_return make_result_wire(json_msg.at("id").get<RequestId>(), scope, std::move(handler_result),
                                   CacheTarget::eResourcesRead);
    }

    // Exact resources are found by lookup and so may be any length; only template matching is
    // length-sensitive. This reports the limit rather than "Unknown resource", so that a caller
    // whose URI is merely too long can tell that from one that names nothing.
    if (params.uri.size() > mcp::detail::constants::server::g_MAX_TEMPLATE_MATCH_URI_LENGTH) {
        co_return make_error_wire(
            json_msg.at("id").get<RequestId>(), mcp::constants::jsonrpc::g_INVALID_PARAMS,
            "Resource URI exceeds the " +
                std::to_string(mcp::detail::constants::server::g_MAX_TEMPLATE_MATCH_URI_LENGTH) +
                " character limit for template matching");
    }

    const Impl::ResourceTemplateRegistration* match = nullptr;
    for (const auto& resource_template : resource_templates) {
        if (!resource_template.handler) {
            continue;
        }
        if (!uri_template_matches(resource_template.matcher, params.uri)) {
            continue;
        }
        if (match != nullptr) {
            co_return make_error_wire(json_msg.at("id").get<RequestId>(),
                                      mcp::constants::jsonrpc::g_INVALID_PARAMS,
                                      "Ambiguous resource template match: " + params.uri);
        }
        match = &resource_template;
    }

    if (match == nullptr || !match->handler) {
        // 2025-11-25 and earlier name this case with its own code, which 2026-07-28 retired. Both
        // name the URI in data.
        co_return make_error_wire(json_msg.at("id").get<RequestId>(),
                                  scope.meta ? mcp::constants::jsonrpc::g_INVALID_PARAMS
                                             : mcp::constants::jsonrpc::g_RESOURCE_NOT_FOUND,
                                  "Unknown resource: " + params.uri,
                                  nlohmann::json{{"uri", params.uri}});
    }

    nlohmann::json params_json = params;
    auto ctx = make_context(scope.cancelled, inline_progress_token(scope), scope.meta,
                            scope.session_request, scope.notifications, scope.principal, scope.mrtr,
                            scope.related_request, scope.inline_input);
    nlohmann::json handler_result = co_await call_through_middleware(match->handler, ctx, params_json);
    co_return make_result_wire(json_msg.at("id").get<RequestId>(), scope, std::move(handler_result),
                               CacheTarget::eResourcesRead);
}

Task<std::string> Server::Impl::handle_resource_templates_list_wire(const nlohmann::json& json_msg,
                                                                    const RequestScope& scope) {
    auto page = paginate(resource_templates.size(), json_msg);
    if (!page) {
        co_return make_error_wire(json_msg.at("id").get<RequestId>(),
                                  mcp::constants::jsonrpc::g_INVALID_PARAMS,
                                  "Invalid pagination cursor");
    }

    ListResourceTemplatesResult list_result;
    auto [begin, end, next_cursor] = *page;
    list_result.resourceTemplates.reserve(end - begin);
    for (std::size_t index = begin; index < end; ++index) {
        list_result.resourceTemplates.push_back(resource_templates[index].metadata);
    }
    list_result.nextCursor = std::move(next_cursor);

    co_return make_result_wire(json_msg.at("id").get<RequestId>(), scope,
                               nlohmann::json(std::move(list_result)),
                               CacheTarget::eResourceTemplatesList);
}

Task<std::string> Server::Impl::handle_subscribe_wire(const nlohmann::json& json_msg) {
    auto params =
        deserialize_request_params<ResourceSubscribeParams>(json_msg, methods::g_RESOURCES_SUBSCRIBE);
    auto handler = build_middleware_chain(
        [this, uri = std::move(params.uri)](Context&, const nlohmann::json&) -> Task<nlohmann::json> {
            auto session = session_snapshot();
            if (session) {
                if (!session->subscriptions.contains(uri)) {
                    const auto epoch = Impl::session_epoch(*session);
                    const auto full = [&session, &uri] {
                        return session->subscriptions.size() >=
                                   mcp::detail::constants::server::g_MAX_SUBSCRIPTIONS ||
                               session->subscription_bytes + uri.size() >
                                   mcp::detail::constants::server::g_MAX_SUBSCRIPTION_BYTES;
                    };
                    if (full()) {
                        // Subscriptions of a session that ended are only dropped lazily.
                        for (auto it = session->subscriptions.begin();
                             it != session->subscriptions.end();) {
                            if (it->second != epoch) {
                                session->subscription_bytes -= it->first.size();
                                it = session->subscriptions.erase(it);
                            } else {
                                ++it;
                            }
                        }
                    }
                    if (full()) {
                        throw InvalidParamsError("Too many resource subscriptions");
                    }
                    session->subscription_bytes += uri.size();
                }
                session->subscriptions[uri] = Impl::session_epoch(*session);
            }
            if (subscribe_handler) {
                subscribe_handler(uri);
            }
            co_return nlohmann::json::object();
        });
    auto ctx = make_context();
    nlohmann::json handler_result = co_await handler(ctx, json_msg.at("params"));
    co_return make_result_wire(json_msg.at("id").get<RequestId>(), std::move(handler_result));
}

Task<std::string> Server::Impl::handle_unsubscribe_wire(const nlohmann::json& json_msg) {
    auto params = deserialize_request_params<ResourceUnsubscribeParams>(
        json_msg, methods::g_RESOURCES_UNSUBSCRIBE);
    auto handler = build_middleware_chain(
        [this, uri = std::move(params.uri)](Context&, const nlohmann::json&) -> Task<nlohmann::json> {
            auto session = session_snapshot();
            if (session && session->subscriptions.erase(uri) != 0) {
                session->subscription_bytes -= uri.size();
            }
            if (unsubscribe_handler) {
                unsubscribe_handler(uri);
            }
            co_return nlohmann::json::object();
        });
    auto ctx = make_context();
    nlohmann::json handler_result = co_await handler(ctx, json_msg.at("params"));
    co_return make_result_wire(json_msg.at("id").get<RequestId>(), std::move(handler_result));
}

Task<std::string> Server::Impl::handle_prompts_list_wire(const nlohmann::json& json_msg,
                                                         const RequestScope& scope) {
    auto page = paginate(prompts.size(), json_msg);
    if (!page) {
        co_return make_error_wire(json_msg.at("id").get<RequestId>(),
                                  mcp::constants::jsonrpc::g_INVALID_PARAMS,
                                  "Invalid pagination cursor");
    }

    ListPromptsResult list_result;
    auto [begin, end, next_cursor] = *page;
    list_result.prompts.assign(prompts.begin() + static_cast<std::ptrdiff_t>(begin),
                               prompts.begin() + static_cast<std::ptrdiff_t>(end));
    list_result.nextCursor = std::move(next_cursor);

    co_return make_result_wire(json_msg.at("id").get<RequestId>(), scope,
                               nlohmann::json(std::move(list_result)), CacheTarget::ePromptsList);
}

Task<std::string> Server::Impl::handle_prompts_get_wire(const nlohmann::json& json_msg,
                                                        const RequestScope& scope) {
    auto params = deserialize_request_params<GetPromptRequestParams>(json_msg, methods::g_PROMPTS_GET);
    auto iter = prompt_handlers.find(params.name);
    if (iter == prompt_handlers.end()) {
        // The prompts spec lists an invalid prompt name as -32602 (Invalid params).
        co_return make_error_wire(json_msg.at("id").get<RequestId>(),
                                  mcp::constants::jsonrpc::g_INVALID_PARAMS,
                                  "Unknown prompt: " + params.name);
    }

    nlohmann::json params_json = std::move(params);
    auto ctx = make_context(scope.cancelled, inline_progress_token(scope), scope.meta,
                            scope.session_request, scope.notifications, scope.principal, scope.mrtr,
                            scope.related_request, scope.inline_input);
    nlohmann::json handler_result = co_await call_through_middleware(iter->second, ctx, params_json);
    co_return make_result_wire(json_msg.at("id").get<RequestId>(), scope, std::move(handler_result));
}

Task<std::string> Server::Impl::handle_set_level_wire(const nlohmann::json& json_msg) {
    auto params =
        deserialize_request_params<SetLevelRequestParams>(json_msg, methods::g_LOGGING_SET_LEVEL);
    log_level.store(params.level, std::memory_order_relaxed);
    co_return make_result_wire(json_msg.at("id").get<RequestId>(), nlohmann::json::object());
}

Task<std::string> Server::Impl::handle_complete_wire(const nlohmann::json& json_msg,
                                                     const RequestScope& scope) {
    auto params = deserialize_request_params<CompleteParams>(json_msg, methods::g_COMPLETION_COMPLETE);

    if (!completion_handler) {
        co_return make_error_wire(json_msg.at("id").get<RequestId>(),
                                  mcp::constants::jsonrpc::g_METHOD_NOT_FOUND,
                                  "No completion handler registered");
    }

    auto handler =
        build_middleware_chain([this, params](Context&, const nlohmann::json&) -> Task<nlohmann::json> {
            auto complete_result = co_await completion_handler(params);
            co_return nlohmann::json(std::move(complete_result));
        });
    auto ctx = make_context(scope.cancelled, std::nullopt, scope.meta, scope.session_request,
                            scope.notifications, scope.principal, scope.mrtr, scope.related_request);
    nlohmann::json handler_result = co_await handler(ctx, json_msg.at("params"));
    co_return make_result_wire(json_msg.at("id").get<RequestId>(), scope, std::move(handler_result));
}

std::string Server::Impl::make_result_wire(const RequestId& id, const nlohmann::json& result) {
    // The text JSONRPCResultResponse serializes to, members in the order nlohmann::json sorts them,
    // written around the result rather than moving it into a response object.
    std::string wire = R"({"id":)";
    detail::append_json(wire, nlohmann::json(id));
    wire += R"(,"jsonrpc":"2.0","result":)";
    detail::append_json(wire, result);
    wire += '}';
    return wire;
}

std::string Server::Impl::make_result_text_wire(const RequestId& id, std::string_view result_text) {
    std::string wire = R"({"id":)";
    detail::append_json(wire, nlohmann::json(id));
    wire += R"(,"jsonrpc":"2.0","result":)";
    wire += result_text;
    wire += '}';
    return wire;
}

std::string Server::Impl::make_result_wire(const RequestId& id, const RequestScope& scope,
                                           nlohmann::json result,
                                           std::optional<CacheTarget> target) const {
    return make_result_wire(id, scope.meta, std::move(result), target);
}

const std::string& Server::Impl::server_info_meta() const {
    std::call_once(server_info_meta_once, [this] {
        try {
            nlohmann::json meta = nlohmann::json::object();
            meta[std::string(mcp::constants::meta::g_META_SERVER_INFO)] = server_info;
            server_info_meta_text = detail::dump_json(meta);
        } catch (const nlohmann::json::exception&) {
            // Left empty: each result then has the identity inserted, and fails, as before.
        }
    });
    return server_info_meta_text;
}

std::string Server::Impl::make_result_wire(const RequestId& id,
                                           const std::shared_ptr<const RequestMetadata>& request_meta,
                                           nlohmann::json result,
                                           std::optional<CacheTarget> target) const {
    if (request_meta == nullptr || !result.is_object()) {
        return make_result_wire(id, std::move(result));
    }
    if (!report_server_info || (!server_info_meta().empty() && !result.contains("_meta"))) {
        // The members this revision adds are written around the result rather than inserted
        // into it: each is added only when the result does not hold it, as emplace() does.
        const CacheHint* const hint =
            target ? &cache_hints.at(static_cast<std::size_t>(*target)) : nullptr;
        std::string scope_text;
        std::string ttl_text;
        std::array<detail::JsonMemberText, 4> extra{};
        std::size_t count = 0;
        if (report_server_info) {
            extra[count++] = {.key = "_meta", .value_text = server_info_meta()};
        }
        if (hint != nullptr) {
            scope_text = detail::dump_json(nlohmann::json(hint->scope));
            extra[count++] = {.key = "cacheScope", .value_text = scope_text};
        }
        extra[count++] = {.key = "resultType", .value_text = R"("complete")"};
        if (hint != nullptr) {
            ttl_text = std::to_string(hint->ttlMs);
            extra[count++] = {.key = "ttlMs", .value_text = ttl_text};
        }
        std::string wire = R"({"id":)";
        detail::append_json(wire, nlohmann::json(id));
        wire += R"(,"jsonrpc":"2.0","result":)";
        detail::append_json_object_with(wire, result, std::span(extra.data(), count));
        wire += '}';
        return wire;
    }
    result.emplace("resultType", "complete");
    if (report_server_info) {
        auto& meta = result["_meta"];
        if (meta.is_null()) {
            meta = nlohmann::json::object();
        }
        if (meta.is_object()) {
            meta.emplace(mcp::constants::meta::g_META_SERVER_INFO, nlohmann::json(server_info));
        }
    }
    if (target) {
        const auto& hint = cache_hints.at(static_cast<std::size_t>(*target));
        result.emplace("ttlMs", hint.ttlMs);
        result.emplace("cacheScope", hint.scope);
    }
    return make_result_wire(id, std::move(result));
}

std::string Server::Impl::make_error_wire(const RequestId& id, int code, std::string message) {
    Error error;
    error.code = code;
    error.message = std::move(message);
    JSONRPCErrorResponse response;
    response.id = id;
    response.error = std::move(error);
    return mcp::detail::dump_json(nlohmann::json(std::move(response)));
}

std::string Server::Impl::make_error_wire(const RequestId& id, int code, std::string message,
                                          nlohmann::json data) {
    Error error;
    error.code = code;
    error.message = std::move(message);
    error.data = std::move(data);
    JSONRPCErrorResponse response;
    response.id = id;
    response.error = std::move(error);
    return mcp::detail::dump_json(nlohmann::json(std::move(response)));
}

std::vector<std::string> Server::Impl::supported_versions() const {
    return served_protocol_versions(protocol_policy);
}

std::string Server::Impl::initialize_not_served_wire(const RequestId& id) const {
    auto supported = supported_versions();
    auto message = detail::initialize_not_served_message(supported);
    return make_error_wire(id, mcp::constants::jsonrpc::g_METHOD_NOT_FOUND, std::move(message),
                           nlohmann::json{{"supported", std::move(supported)}});
}

std::string Server::Impl::unsupported_version_wire(const RequestId& id, std::string requested) const {
    UnsupportedProtocolVersionData data;
    data.supported = supported_versions();
    data.requested = std::move(requested);
    return make_error_wire(id, mcp::constants::jsonrpc::g_UNSUPPORTED_PROTOCOL_VERSION,
                           "Unsupported protocol version", nlohmann::json(std::move(data)));
}

Task<nlohmann::json> Server::Impl::call_through_middleware(const TypeErasedHandler& final_handler,
                                                           Context& ctx, const nlohmann::json& params) {
    TypeErasedHandler on_strand = [this, &final_handler](
                                      Context& inner_ctx,
                                      const nlohmann::json& inner_params) -> Task<nlohmann::json> {
        co_return co_await call_on_handler_strand(final_handler, inner_ctx, inner_params);
    };
    if (middlewares.empty()) {
        co_return co_await on_strand(ctx, params);
    }
    auto handler = build_middleware_chain(std::move(on_strand));
    co_return co_await handler(ctx, params);
}

namespace {

// The links of a middleware chain. Each returns a coroutine that owns what it calls, so that a
// middleware may return next(ctx, params) unawaited, which destroys its copy of next before the
// coroutine runs.
Task<nlohmann::json> call_owned_handler(std::shared_ptr<const TypeErasedHandler> handler, Context& ctx,
                                        const nlohmann::json& params) {
    co_return co_await (*handler)(ctx, params);
}

Task<nlohmann::json> call_owned_middleware(std::shared_ptr<const Middleware> middleware,
                                           TypeErasedHandler next, Context& ctx,
                                           const nlohmann::json& params) {
    co_return co_await (*middleware)(ctx, params, std::move(next));
}

}  // namespace

TypeErasedHandler Server::Impl::build_middleware_chain(TypeErasedHandler final_handler) {
    if (middlewares.empty()) {
        return final_handler;
    }
    TypeErasedHandler handler = [owned = std::make_shared<const TypeErasedHandler>(std::move(
                                     final_handler))](Context& ctx, const nlohmann::json& params) {
        return call_owned_handler(owned, ctx, params);
    };
    for (const auto& mw : std::ranges::reverse_view(middlewares)) {
        handler = [owned = std::make_shared<const Middleware>(mw), next = std::move(handler)](
                      Context& ctx, const nlohmann::json& params) {
            return call_owned_middleware(owned, next, ctx, params);
        };
    }
    return handler;
}

std::optional<Server::PaginationSlice> Server::Impl::paginate(std::size_t total,
                                                              const nlohmann::json& json_msg) const {
    if (page_size == 0 || total == 0) {
        return PaginationSlice{.begin = 0, .end = total, .next_cursor = std::nullopt};
    }

    // A cursor is the decimal offset of the page's first entry, as next_cursor below writes it.
    // Anything else, or an offset past the end of the list, is not a cursor this server issued.
    std::size_t offset = 0;
    if (json_msg.contains("params") && detail::has_json_value(json_msg.at("params"), "cursor")) {
        const auto& cursor = json_msg.at("params").at("cursor");
        if (!cursor.is_string()) {
            return std::nullopt;
        }
        const auto& text = cursor.get_ref<const std::string&>();
        const char* const text_end = text.data() + text.size();
        const auto [parsed_end, error] = std::from_chars(text.data(), text_end, offset);
        if (error != std::errc{} || parsed_end != text_end || offset >= total) {
            return std::nullopt;
        }
    }

    auto end = std::min(offset + page_size, total);
    std::optional<std::string> next_cursor;
    if (end < total) {
        next_cursor = std::to_string(end);
    }

    return PaginationSlice{.begin = offset, .end = end, .next_cursor = std::move(next_cursor)};
}

}  // namespace mcp

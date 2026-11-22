#include "client_impl.hpp"

namespace mcp {

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

ClientProtocol ClientProtocol::automatic() { return {Kind::eAuto, std::string()}; }

ClientProtocol ClientProtocol::legacy() {
    return {Kind::eLegacy, std::string(mcp::constants::protocol::g_LATEST_LEGACY_PROTOCOL_VERSION)};
}

ClientProtocol ClientProtocol::pinned(std::string version) {
    if (!is_supported_protocol_version(version)) {
        throw std::invalid_argument("Unknown protocol version: " +
                                    detail::sanitize_for_diagnostics(version));
    }
    return {Kind::eVersion, std::move(version)};
}

void to_json(nlohmann::json& json_obj, const SubscriptionFilter& filter) {
    json_obj = nlohmann::json::object();
    if (filter.toolsListChanged) {
        json_obj["toolsListChanged"] = true;
    }
    if (filter.promptsListChanged) {
        json_obj["promptsListChanged"] = true;
    }
    if (filter.resourcesListChanged) {
        json_obj["resourcesListChanged"] = true;
    }
    if (!filter.resourceSubscriptions.empty()) {
        json_obj["resourceSubscriptions"] = filter.resourceSubscriptions;
    }
    if (!filter.taskIds.empty()) {
        json_obj["taskIds"] = filter.taskIds;
    }
}

void from_json(const nlohmann::json& json_obj, SubscriptionFilter& filter) {
    filter = SubscriptionFilter{};
    filter.toolsListChanged = json_obj.value("toolsListChanged", false);
    filter.promptsListChanged = json_obj.value("promptsListChanged", false);
    filter.resourcesListChanged = json_obj.value("resourcesListChanged", false);
    if (const auto found = json_obj.find("resourceSubscriptions");
        found != json_obj.end() && !found->is_null()) {
        found->get_to(filter.resourceSubscriptions);
    }
    if (const auto found = json_obj.find("taskIds"); found != json_obj.end() && !found->is_null()) {
        found->get_to(filter.taskIds);
    }
}

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
    for (const auto& id : asked.taskIds) {
        if (std::find(honored.taskIds.begin(), honored.taskIds.end(), id) == honored.taskIds.end()) {
            missing.taskIds.push_back(id);
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

CancellationToken::CancellationToken() : state_(std::make_shared<detail::CancellationState>()) {}

void CancellationToken::cancel(std::optional<std::string> reason) const {
    std::map<std::uint64_t, detail::CancellationState::Callback> callbacks;
    {
        std::lock_guard lock(state_->mutex);
        if (state_->cancelled.load(std::memory_order_relaxed)) {
            return;
        }
        state_->reason = std::move(reason);
        state_->cancelled.store(true, std::memory_order_release);
        callbacks.swap(state_->callbacks);
    }
    // Run outside the lock: a callback may end a wait that removes its own registration.
    for (auto& [registration, callback] : callbacks) {
        callback(state_->reason);
    }
}

bool CancellationToken::is_cancelled() const noexcept {
    return state_->cancelled.load(std::memory_order_acquire);
}

struct Client::OwnedLoop {
    /// An io_context whose pending handlers can be destroyed before it is.
    class Context final : public boost::asio::io_context {
       public:
        using boost::asio::execution_context::shutdown;
    };

    Context io;
    /// How long the destructor waits for the loop to finish.
    std::chrono::milliseconds close_wait{mcp::constants::client::g_CLOSE_WAIT_MS};
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work{io.get_executor()};
    std::promise<void> finished;
    std::future<void> finished_future = finished.get_future();
    std::thread thread;

    OwnedLoop() {
        thread = std::thread([this] {
            for (;;) {
                try {
                    io.run();
                    break;
                } catch (...) {
                    // A handler threw: the client's own work catches its errors, so this is one of
                    // the application's callbacks. The loop keeps running for the others.
                }
            }
            finished.set_value();
        });
    }

    OwnedLoop(const OwnedLoop&) = delete;
    OwnedLoop& operator=(const OwnedLoop&) = delete;
    OwnedLoop(OwnedLoop&&) = delete;
    OwnedLoop& operator=(OwnedLoop&&) = delete;
    ~OwnedLoop() = default;
};

namespace {

/// @throws std::invalid_argument When @p options holds a value a client cannot work with.
void check_options(const ClientOptions& options) {
    if (options.request_timeout <= std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("Client request timeout must be positive");
    }
    if (options.probe_timeout <= std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("Client probe timeout must be positive");
    }
    if (options.subscription_queue_limit == 0) {
        throw std::invalid_argument("Client subscription queue limit must be positive");
    }
    if (options.task_poll_interval <= std::chrono::milliseconds::zero() ||
        options.task_timeout <= std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("Client task poll interval and task timeout must be positive");
    }
    if (options.max_incoming_request_bytes == 0) {
        throw std::invalid_argument("Client incoming request memory limit must be positive");
    }
    detail::check_rate_limit(options.notification_rate_limit);
    if (options.response_cache.enabled &&
        (options.response_cache.max_entries == 0 ||
         options.response_cache.max_ttl <= std::chrono::milliseconds::zero())) {
        throw std::invalid_argument("Client response cache entry limit and lifetime must be positive");
    }
}

}  // namespace

Client::Client(std::shared_ptr<ITransport> transport, const Executor& executor, ClientOptions options) {
    start(std::move(transport), executor, std::move(options));
}

Client::Client(const Endpoint& endpoint, ClientOptions options) : loop_(std::make_shared<OwnedLoop>()) {
    if (endpoint.close_wait_ > std::chrono::milliseconds::zero()) {
        loop_->close_wait = std::max(
            loop_->close_wait, endpoint.close_wait_ + detail::constants::stdio_client::g_KILL_WAIT);
    }
    try {
        // Before the transport, which may start the server's process.
        check_options(options);
        const Executor executor = loop_->io.get_executor();
        start(endpoint.make_transport(executor), executor, std::move(options));
        impl_->blocking_loop = std::make_shared<const detail::BlockingLoop>(
            loop_, loop_->io.get_executor(), loop_->thread.get_id());
    } catch (...) {
        loop_->work.reset();
        loop_->io.stop();
        loop_->thread.join();
        throw;
    }
}

void Client::start(std::shared_ptr<ITransport> transport, const Executor& executor,
                   ClientOptions options) {
    if (!transport) {
        throw std::invalid_argument("Client transport must not be null");
    }
    check_options(options);
    impl_ = std::make_shared<Impl>(std::move(transport), executor.get(), std::move(options));
    impl_->executor = executor;
    impl_->transport->set_interaction_observer(
        [state = std::weak_ptr<Impl>(impl_)](std::string_view key, bool waiting) {
            if (const auto alive = state.lock()) {
                // A transport may report from any thread; the request's timer belongs to the strand.
                boost::asio::dispatch(alive->strand, [alive, key = std::string(key), waiting] {
                    Impl::on_interaction(alive, key, waiting);
                });
            }
        });
}

Executor Client::executor() const { return impl_->executor; }

namespace {

/// The params of a tools/call with JSON arguments; `{}` makes null ones, which are sent as an empty
/// object.
CallToolParams json_call_params(const std::string& name, const nlohmann::json& arguments) {
    CallToolParams call_params;
    call_params.name = name;
    call_params.arguments = arguments.is_null() ? nlohmann::json::object() : arguments;
    return call_params;
}

}  // namespace

Task<CallToolResult> Client::call_tool(const std::string& name, const nlohmann::json& arguments,
                                       const RequestOptions& request_options) {
    // [gcc11-sso: not-a-coroutine]
    return call_tool_with(json_call_params(name, arguments), request_options);
}

Task<std::variant<CallToolResult, TaskHandle>> Client::start_tool(
    const std::string& name, const nlohmann::json& arguments, const RequestOptions& request_options) {
    // [gcc11-sso: not-a-coroutine]
    return start_tool_with(json_call_params(name, arguments), request_options);
}

Client::~Client() {
    close();
    if (!loop_) {
        return;
    }
    if (loop_->thread.get_id() == std::this_thread::get_id()) {
        // The thread cannot join itself, and the loop it would wait for is the one it runs.
        std::fputs(
            "mcp::Client: a client with its own event loop was destroyed on that loop's "
            "thread\n",
            stderr);
        std::terminate();
    }
    loop_->work.reset();
    if (loop_->finished_future.wait_for(loop_->close_wait) != std::future_status::ready) {
        loop_->io.stop();
    }
    loop_->thread.join();
    if (impl_->blocking_loop) {
        impl_->blocking_loop->mark_ended();
    }
    // The handlers a stopped loop did not run hold the client's state, which holds the loop:
    // destroying them frees both.
    loop_->io.shutdown();
    impl_.reset();
}

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
        throw McpError(mcp::constants::jsonrpc::g_CONNECTION_CLOSED, "Client is closed");
    }
    if (state->read_loop_started.exchange(true, std::memory_order_acq_rel)) {
        throw McpError(mcp::constants::jsonrpc::g_INVALID_REQUEST,
                       "Client has already been initialized");
    }

    Impl::start_read_loop(state);
    {
        std::lock_guard lock(state->handlers_mutex);
        state->client_info = client_info;
        state->client_capabilities = capabilities;
    }
    return Impl::connect_session(std::move(state));
}

Task<nlohmann::json> Client::send_request(std::string_view method,
                                          const std::optional<nlohmann::json>& params,
                                          const RequestOptions& request_options) {
    if (!impl_->read_loop_started.load(std::memory_order_acquire)) {
        throw McpError(mcp::constants::jsonrpc::g_INVALID_REQUEST,
                       "Client must connect before sending requests");
    }
    return Impl::request(impl_, method, params, request_options);
}

Task<Subscription> Client::listen(const SubscriptionFilter& filter,
                                  const RequestOptions& request_options) {
    if (!impl_->read_loop_started.load(std::memory_order_acquire)) {
        throw McpError(mcp::constants::jsonrpc::g_INVALID_REQUEST,
                       "Client must connect before sending requests");
    }
    return Impl::listen(impl_, filter, request_options);
}

Task<void> Client::send_notification(std::string_view method,
                                     const std::optional<nlohmann::json>& params) {
    if (!impl_->read_loop_started.load(std::memory_order_acquire)) {
        throw McpError(mcp::constants::jsonrpc::g_INVALID_REQUEST,
                       "Client must connect before sending notifications");
    }
    return Impl::notification(impl_, method, params);
}

void Client::clear_response_cache() { impl_->response_cache.clear(); }

std::uint64_t Client::dropped_notifications() const {
    return impl_->dropped_notifications.load(std::memory_order_relaxed);
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

Task<ListToolsResult> Client::list_tools(const std::optional<std::string>& cursor,
                                         const RequestOptions& request_options) {
    // [gcc11-sso: not-a-coroutine]
    return Impl::accepted_tools(
        impl_, parse_result<ListToolsResult>(Impl::cacheable_request(
                   impl_, methods::g_TOOLS_LIST, make_paginated_params(cursor), request_options)));
}

Task<CallToolResult> Client::call_tool_with(CallToolParams params,
                                            const RequestOptions& request_options) {
    // [gcc11-sso: not-a-coroutine]
    auto check = impl_->output_check_for(params.name);
    if (impl_->options.tasks == ClientTaskMode::eAuto && impl_->modern_version()) {
        if (!impl_->read_loop_started.load(std::memory_order_acquire)) {
            throw McpError(mcp::constants::jsonrpc::g_INVALID_REQUEST,
                           "Client must connect before sending requests");
        }
        return Impl::call_tool_following(impl_, nlohmann::json(std::move(params)), std::move(check),
                                         request_options);
    }
    return checked_call_result(
        send_request(methods::g_TOOLS_CALL, nlohmann::json(std::move(params)), request_options),
        std::move(check));
}

Task<std::variant<CallToolResult, TaskHandle>> Client::start_tool_with(
    CallToolParams params, const RequestOptions& request_options) {
    // [gcc11-sso: not-a-coroutine]
    auto check = impl_->output_check_for(params.name);
    if (!impl_->read_loop_started.load(std::memory_order_acquire)) {
        throw McpError(mcp::constants::jsonrpc::g_INVALID_REQUEST,
                       "Client must connect before sending requests");
    }
    return Impl::start_tool(impl_, nlohmann::json(std::move(params)), std::move(check),
                            request_options);
}

TaskHandle Client::task_handle(std::string id) {
    TaskData created;
    created.taskId = std::move(id);
    return TaskHandle(impl_, std::move(created), std::nullopt);
}

TaskHandle::TaskHandle(std::shared_ptr<void> client, TaskData created, std::optional<std::string> tool)
    : client_(std::move(client)), created_(std::move(created)), tool_(std::move(tool)) {}

namespace {

std::shared_ptr<void> attached_client(const std::shared_ptr<void>& client) {
    if (!client) {
        throw McpError(mcp::constants::jsonrpc::g_INVALID_REQUEST,
                       "The task handle is not attached to a task");
    }
    return client;
}

Task<DetailedTask> detailed_task(Task<nlohmann::json> finished) {
    auto reply = co_await std::move(finished);
    co_return decode_task_state(reply);
}

Task<CallToolResult> task_result(Task<nlohmann::json> finished, OutputCheck check) {
    auto reply = co_await std::move(finished);
    co_return finish_call_result(task_outcome(reply), check);
}

}  // namespace

Task<DetailedTask> TaskHandle::get(const RequestOptions& request_options) const {
    // [gcc11-sso: not-a-coroutine]
    auto state = std::static_pointer_cast<Client::Impl>(attached_client(client_));
    nlohmann::json params{{"taskId", created_.taskId}};
    return Client::Impl::task_state(std::move(state), std::move(params), request_options);
}

Task<void> TaskHandle::update(const nlohmann::json& input_responses,
                              const RequestOptions& request_options) const {
    auto state = std::static_pointer_cast<Client::Impl>(attached_client(client_));
    nlohmann::json params{{"taskId", created_.taskId}, {"inputResponses", input_responses}};
    return Client::Impl::task_ack(std::move(state), methods::g_TASKS_UPDATE, std::move(params),
                                  request_options);
}

Task<void> TaskHandle::cancel(const RequestOptions& request_options) const {
    auto state = std::static_pointer_cast<Client::Impl>(attached_client(client_));
    nlohmann::json params{{"taskId", created_.taskId}};
    return Client::Impl::task_ack(std::move(state), methods::g_TASKS_CANCEL, std::move(params),
                                  request_options);
}

Task<DetailedTask> TaskHandle::wait(const RequestOptions& request_options) const {
    auto state = std::static_pointer_cast<Client::Impl>(attached_client(client_));
    nlohmann::json params{{"taskId", created_.taskId}};
    auto finished = Client::Impl::wait_for_task(std::move(state), std::move(params), request_options);
    return detailed_task(std::move(finished));
}

Task<CallToolResult> TaskHandle::result(const RequestOptions& request_options) const {
    auto state = std::static_pointer_cast<Client::Impl>(attached_client(client_));
    auto check = tool_ ? state->output_check_for(*tool_) : OutputCheck{};
    if (check.refusal) {
        throw *check.refusal;
    }
    nlohmann::json params{{"taskId", created_.taskId}};
    auto finished = Client::Impl::wait_for_task(std::move(state), std::move(params), request_options);
    return task_result(std::move(finished), std::move(check));
}

Task<ListResourcesResult> Client::list_resources(const std::optional<std::string>& cursor,
                                                 const RequestOptions& request_options) {
    return parse_result<ListResourcesResult>(Impl::cacheable_request(
        impl_, methods::g_RESOURCES_LIST, make_paginated_params(cursor), request_options));
}

Task<ReadResourceResult> Client::read_resource(const std::string& uri,
                                               const RequestOptions& request_options) {
    ReadResourceRequestParams params;
    params.uri = uri;
    return parse_result<ReadResourceResult>(Impl::cacheable_request(
        impl_, methods::g_RESOURCES_READ, nlohmann::json(std::move(params)), request_options));
}

Task<ListResourceTemplatesResult> Client::list_resource_templates(
    const std::optional<std::string>& cursor, const RequestOptions& request_options) {
    return parse_result<ListResourceTemplatesResult>(Impl::cacheable_request(
        impl_, methods::g_RESOURCES_TEMPLATES_LIST, make_paginated_params(cursor), request_options));
}

Task<ListPromptsResult> Client::list_prompts(const std::optional<std::string>& cursor,
                                             const RequestOptions& request_options) {
    return parse_result<ListPromptsResult>(Impl::cacheable_request(
        impl_, methods::g_PROMPTS_LIST, make_paginated_params(cursor), request_options));
}

Task<GetPromptResult> Client::get_prompt(const std::string& name,
                                         const std::map<std::string, std::string>& arguments,
                                         const RequestOptions& request_options) {
    GetPromptRequestParams params;
    params.name = name;
    if (!arguments.empty()) {
        params.arguments = arguments;
    }
    return parse_result<GetPromptResult>(
        send_request(methods::g_PROMPTS_GET, nlohmann::json(std::move(params)), request_options));
}

Task<CompleteResult> Client::complete(const CompleteParams& params,
                                      const RequestOptions& request_options) {
    return parse_result<CompleteResult>(
        send_request(methods::g_COMPLETION_COMPLETE, nlohmann::json(params), request_options));
}

Task<void> Client::ping() {
    if (!impl_->read_loop_started.load(std::memory_order_acquire)) {
        throw McpError(mcp::constants::jsonrpc::g_INVALID_REQUEST,
                       "Client must connect before sending requests");
    }
    return Impl::discard_result(Impl::request(impl_, methods::g_PING, std::nullopt, {}));
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
            iter->second->error =
                Error{mcp::constants::jsonrpc::g_REQUEST_CANCELLED, "Request cancelled"};
            iter->second->completed = true;
            Impl::signal_pending_request(*iter->second);
        }
    }
    CancelledNotificationParams params;
    params.requestId = request_id;
    params.reason = reason;
    return send_notification(methods::g_NOTIFICATIONS_CANCELLED, nlohmann::json(std::move(params)));
}

void Client::on_notification(const std::string& method, NotificationCallback callback) {
    std::lock_guard lock(impl_->handlers_mutex);
    impl_->notification_handlers[method] = std::move(callback);
}

void Client::on_progress(ProgressCallback callback) {
    on_notification(std::string(methods::g_NOTIFICATIONS_PROGRESS),
                    [callback = std::move(callback)](const nlohmann::json& params) {
                        ProgressNotificationParams progress;
                        try {
                            params.get_to(progress);
                        } catch (const std::exception& error) {
                            // Reported through ClientOptions::on_protocol_error by the read loop.
                            // Naming the cause here separates a notification the peer sent wrong
                            // from a bug in the application's own progress callback.
                            throw McpError(mcp::constants::jsonrpc::g_INVALID_PARAMS,
                                           "Malformed progress notification params: " +
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
    const bool validate = impl_->options.validate_elicitation_content;
    on_request(
        std::string(methods::g_ELICITATION_CREATE),
        [handler = std::move(handler), validate](const nlohmann::json& params) -> Task<nlohmann::json> {
            auto result = co_await handler(params.get<ElicitRequestParams>());
            if (validate) {
                check_elicitation_content(params, result);
            }
            co_return nlohmann::json(std::move(result));
        });
}

void Client::on_sampling(
    std::function<Task<CreateMessageResult>(const CreateMessageRequestParams&)> handler) {
    on_request(std::string(methods::g_SAMPLING_CREATE_MESSAGE),
               [handler = std::move(handler)](const nlohmann::json& params) -> Task<nlohmann::json> {
                   auto result = co_await handler(params.get<CreateMessageRequestParams>());
                   co_return nlohmann::json(std::move(result));
               });
}

void Client::set_roots(const std::vector<Root>& roots, bool notify) {
    auto state = impl_;
    bool should_notify = false;
    {
        std::lock_guard lock(state->handlers_mutex);
        state->roots = roots;
        if (!state->request_handlers.contains(methods::g_ROOTS_LIST)) {
            std::weak_ptr<Impl> weak_state = state;
            state->request_handlers.emplace(
                methods::g_ROOTS_LIST, [weak_state](const nlohmann::json&) -> Task<nlohmann::json> {
                    auto active_state = weak_state.lock();
                    if (!active_state) {
                        throw McpError(mcp::constants::jsonrpc::g_CONNECTION_CLOSED,
                                       "Client is closed");
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
            state->strand,
            Impl::notification(state, methods::g_NOTIFICATIONS_ROOTS_LIST_CHANGED, std::nullopt),
            boost::asio::detached);
    }
}

void Client::on_roots_list(std::function<Task<ListRootsResult>(const nlohmann::json&)> handler) {
    on_request(std::string(methods::g_ROOTS_LIST),
               [handler = std::move(handler)](const nlohmann::json& params) -> Task<nlohmann::json> {
                   auto result = co_await handler(params);
                   co_return nlohmann::json(std::move(result));
               });
}

SyncClient Client::sync() {
    if (!impl_->blocking_loop) {
        throw std::logic_error(
            "mcp::Client::sync() needs a client constructed from an Endpoint; a client on the "
            "application's executor is used through its coroutines");
    }
    return SyncClient(*this);
}

void Client::run(std::function<Task<void>()> body) {
    if (!body) {
        throw std::invalid_argument("mcp::Client::run() needs a coroutine");
    }
    detail::run_blocking<void>(impl_->blocking_loop, [body = std::move(body)]() -> Task<void> {
        Task<void> task = body();
        if (!task.valid()) {
            throw std::invalid_argument(
                "mcp::Client::run() needs a coroutine; the function returned an empty Task");
        }
        co_await std::move(task);
    });
}

SyncSubscription Subscription::sync() const {
    if (!state_ || !state_->blocking_loop) {
        throw std::logic_error(
            "mcp::Subscription::sync() needs a subscription of a client constructed from an "
            "Endpoint");
    }
    return SyncSubscription(*this);
}

SyncSubscription::SyncSubscription(Subscription subscription)
    : subscription_(std::move(subscription)) {}

std::optional<SubscriptionEvent> SyncSubscription::next() {
    return detail::run_blocking<std::optional<SubscriptionEvent>>(
        subscription_.state_->blocking_loop, [this] { return subscription_.next(); });
}

void SyncSubscription::close() {
    detail::run_blocking<void>(subscription_.state_->blocking_loop,
                               [this] { return subscription_.close(); });
}

std::shared_ptr<const detail::BlockingLoop> TaskHandle::blocking_loop() const {
    return std::static_pointer_cast<Client::Impl>(attached_client(client_))->blocking_loop;
}

SyncTaskHandle TaskHandle::sync() const {
    if (!blocking_loop()) {
        throw std::logic_error(
            "mcp::TaskHandle::sync() needs a task of a client constructed from an Endpoint");
    }
    return SyncTaskHandle(*this);
}

SyncTaskHandle::SyncTaskHandle(TaskHandle handle) : handle_(std::move(handle)) {}

DetailedTask SyncTaskHandle::get(const RequestOptions& request_options) const {
    return detail::run_blocking<DetailedTask>(handle_.blocking_loop(),
                                              [&] { return handle_.get(request_options); });
}

void SyncTaskHandle::update(const nlohmann::json& input_responses,
                            const RequestOptions& request_options) const {
    detail::run_blocking<void>(handle_.blocking_loop(),
                               [&] { return handle_.update(input_responses, request_options); });
}

void SyncTaskHandle::cancel(const RequestOptions& request_options) const {
    detail::run_blocking<void>(handle_.blocking_loop(),
                               [&] { return handle_.cancel(request_options); });
}

DetailedTask SyncTaskHandle::wait(const RequestOptions& request_options) const {
    return detail::run_blocking<DetailedTask>(handle_.blocking_loop(),
                                              [&] { return handle_.wait(request_options); });
}

CallToolResult SyncTaskHandle::result(const RequestOptions& request_options) const {
    return detail::run_blocking<CallToolResult>(handle_.blocking_loop(),
                                                [&] { return handle_.result(request_options); });
}

SyncClient::SyncClient(Client& client) : client_(&client) {}

ConnectResult SyncClient::connect(std::string_view name, std::string_view version) {
    return detail::run_blocking<ConnectResult>(client_->impl_->blocking_loop,
                                               [&] { return client_->connect(name, version); });
}

ConnectResult SyncClient::connect(const Implementation& client_info,
                                  const ClientCapabilities& capabilities) {
    return detail::run_blocking<ConnectResult>(
        client_->impl_->blocking_loop, [&] { return client_->connect(client_info, capabilities); });
}

nlohmann::json SyncClient::send_request(std::string_view method,
                                        const std::optional<nlohmann::json>& params,
                                        const RequestOptions& request_options) {
    return detail::run_blocking<nlohmann::json>(client_->impl_->blocking_loop, [&] {
        return client_->send_request(method, params, request_options);
    });
}

Subscription SyncClient::listen(const SubscriptionFilter& filter,
                                const RequestOptions& request_options) {
    return detail::run_blocking<Subscription>(client_->impl_->blocking_loop,
                                              [&] { return client_->listen(filter, request_options); });
}

void SyncClient::send_notification(std::string_view method,
                                   const std::optional<nlohmann::json>& params) {
    detail::run_blocking<void>(client_->impl_->blocking_loop,
                               [&] { return client_->send_notification(method, params); });
}

void SyncClient::close() { client_->close(); }

CallToolResult SyncClient::call_tool(const std::string& name, const nlohmann::json& arguments,
                                     const RequestOptions& request_options) {
    return detail::run_blocking<CallToolResult>(client_->impl_->blocking_loop, [&] {
        return client_->call_tool(name, arguments, request_options);
    });
}

std::variant<CallToolResult, TaskHandle> SyncClient::start_tool(const std::string& name,
                                                                const nlohmann::json& arguments,
                                                                const RequestOptions& request_options) {
    return detail::run_blocking<std::variant<CallToolResult, TaskHandle>>(
        client_->impl_->blocking_loop,
        [&] { return client_->start_tool(name, arguments, request_options); });
}

ListToolsResult SyncClient::list_tools(const std::optional<std::string>& cursor,
                                       const RequestOptions& request_options) {
    return detail::run_blocking<ListToolsResult>(
        client_->impl_->blocking_loop, [&] { return client_->list_tools(cursor, request_options); });
}

ListResourcesResult SyncClient::list_resources(const std::optional<std::string>& cursor,
                                               const RequestOptions& request_options) {
    return detail::run_blocking<ListResourcesResult>(client_->impl_->blocking_loop, [&] {
        return client_->list_resources(cursor, request_options);
    });
}

ReadResourceResult SyncClient::read_resource(const std::string& uri,
                                             const RequestOptions& request_options) {
    return detail::run_blocking<ReadResourceResult>(
        client_->impl_->blocking_loop, [&] { return client_->read_resource(uri, request_options); });
}

ListResourceTemplatesResult SyncClient::list_resource_templates(
    const std::optional<std::string>& cursor, const RequestOptions& request_options) {
    return detail::run_blocking<ListResourceTemplatesResult>(client_->impl_->blocking_loop, [&] {
        return client_->list_resource_templates(cursor, request_options);
    });
}

ListPromptsResult SyncClient::list_prompts(const std::optional<std::string>& cursor,
                                           const RequestOptions& request_options) {
    return detail::run_blocking<ListPromptsResult>(
        client_->impl_->blocking_loop, [&] { return client_->list_prompts(cursor, request_options); });
}

GetPromptResult SyncClient::get_prompt(const std::string& name,
                                       const std::map<std::string, std::string>& arguments,
                                       const RequestOptions& request_options) {
    return detail::run_blocking<GetPromptResult>(client_->impl_->blocking_loop, [&] {
        return client_->get_prompt(name, arguments, request_options);
    });
}

CompleteResult SyncClient::complete(const CompleteParams& params,
                                    const RequestOptions& request_options) {
    return detail::run_blocking<CompleteResult>(
        client_->impl_->blocking_loop, [&] { return client_->complete(params, request_options); });
}

void SyncClient::ping() {
    detail::run_blocking<void>(client_->impl_->blocking_loop, [&] { return client_->ping(); });
}

void SyncClient::cancel(const RequestId& request_id, const std::optional<std::string>& reason) {
    detail::run_blocking<void>(client_->impl_->blocking_loop,
                               [&] { return client_->cancel(request_id, reason); });
}

}  // namespace mcp

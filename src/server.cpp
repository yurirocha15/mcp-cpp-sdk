#include <mcp/server.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace mcp {

struct Server::PendingRequest {
    std::unique_ptr<boost::asio::steady_timer> timer;
    nlohmann::json result;
    std::optional<Error> error;
};

struct Server::Session {
    std::unique_ptr<ITransport> transport;
    std::unique_ptr<boost::asio::strand<boost::asio::any_io_executor>> strand;
    std::map<std::string, PendingRequest> pending_requests;
    std::map<std::string, std::shared_ptr<std::atomic<bool>>> in_flight;
    std::map<std::string, bool> subscriptions;
};

struct Server::Impl {
    Implementation server_info;
    ServerCapabilities capabilities;
    bool initialized{false};
    bool shutdown_requested{false};

    std::unique_ptr<Session> session;
    std::atomic<int64_t> next_request_id{1};
    std::atomic<LoggingLevel> log_level{LoggingLevel::Debug};
    std::size_t page_size{0};

    CompletionHandler completion_handler;

    std::vector<Tool> tools;
    std::map<std::string, TypeErasedHandler, std::less<>> tool_handlers;

    std::vector<Resource> resources;
    std::map<std::string, TypeErasedHandler, std::less<>> resource_handlers;

    std::vector<ResourceTemplate> resource_templates;

    std::vector<Prompt> prompts;
    std::map<std::string, TypeErasedHandler, std::less<>> prompt_handlers;

    std::vector<Middleware> middlewares;

    SubscriptionHandler subscribe_handler;
    SubscriptionHandler unsubscribe_handler;
};

Server::Server(Implementation server_info, ServerCapabilities capabilities)
    : impl_(std::make_unique<Impl>()) {
    impl_->server_info = std::move(server_info);
    impl_->capabilities = std::move(capabilities);
}

Server::~Server() { reset_session(); }

void Server::register_tool(Tool tool, std::string name, TypeErasedHandler handler) {
    impl_->tools.push_back(std::move(tool));
    impl_->tool_handlers.emplace(std::move(name), std::move(handler));
}

void Server::register_resource(Resource resource, TypeErasedHandler handler) {
    auto uri = resource.uri;
    impl_->resources.push_back(std::move(resource));
    impl_->resource_handlers.emplace(std::move(uri), std::move(handler));
}

void Server::register_prompt(Prompt prompt, TypeErasedHandler handler) {
    auto name = prompt.name;
    impl_->prompts.push_back(std::move(prompt));
    impl_->prompt_handlers.emplace(std::move(name), std::move(handler));
}

void Server::add_resource_template(ResourceTemplate tmpl) {
    impl_->resource_templates.push_back(std::move(tmpl));
}

void Server::use(Middleware mw) { impl_->middlewares.push_back(std::move(mw)); }

void Server::set_completion_provider(CompletionHandler handler) {
    impl_->completion_handler = std::move(handler);
}

void Server::set_page_size(std::size_t size) { impl_->page_size = size; }

void Server::on_subscribe(SubscriptionHandler handler) {
    impl_->subscribe_handler = std::move(handler);
}

void Server::on_unsubscribe(SubscriptionHandler handler) {
    impl_->unsubscribe_handler = std::move(handler);
}

bool Server::is_initialized() const { return impl_->initialized; }

bool Server::is_shutdown_requested() const { return impl_->shutdown_requested; }

LoggingLevel Server::get_log_level() const { return impl_->log_level.load(std::memory_order_relaxed); }

Task<nlohmann::json> Server::send_request(std::string method, std::optional<nlohmann::json> params) {
    auto id = impl_->next_request_id.fetch_add(1, std::memory_order_relaxed);
    auto id_str = std::to_string(id);

    JSONRPCRequest request;
    request.id = RequestId{id_str};
    request.method = std::move(method);
    request.params = std::move(params);

    auto& pending = impl_->session->pending_requests[id_str];
    pending.timer = std::make_unique<boost::asio::steady_timer>(*impl_->session->strand);
    pending.timer->expires_at(std::chrono::steady_clock::time_point::max());

    nlohmann::json msg = request;
    co_await impl_->session->transport->write_message(msg.dump());

    try {
        co_await pending.timer->async_wait(boost::asio::use_awaitable);
    } catch (const boost::system::system_error& err) {
        if (err.code() != boost::asio::error::operation_aborted) {
            throw;
        }
    }

    auto it = impl_->session->pending_requests.find(id_str);
    if (it == impl_->session->pending_requests.end()) {
        throw std::runtime_error("pending request not found for id: " + id_str);
    }

    auto json_result = std::move(it->second.result);
    auto error = std::move(it->second.error);
    impl_->session->pending_requests.erase(it);

    if (error) {
        throw std::runtime_error("JSON-RPC error " + std::to_string(error->code) + ": " +
                                 error->message);
    }

    co_return json_result;
}

Task<void> Server::run(std::unique_ptr<ITransport> transport,
                       const boost::asio::any_io_executor& executor) {
    impl_->session = std::make_unique<Session>();
    impl_->session->transport = std::move(transport);
    impl_->session->strand = std::make_unique<boost::asio::strand<boost::asio::any_io_executor>>(
        boost::asio::make_strand(executor));

    try {
        for (;;) {
            auto raw = co_await impl_->session->transport->read_message();
            auto json_msg = nlohmann::json::parse(raw);
            dispatch(std::move(json_msg));
        }
    } catch (const std::exception& e) {
        (void)e;
    }

    reset_session();
}

void Server::dispatch(nlohmann::json json_msg) {
    if (!json_msg.contains("id")) {
        dispatch_notification(json_msg);
        return;
    }

    if (!json_msg.contains("method")) {
        dispatch_response(json_msg);
        return;
    }

    boost::asio::co_spawn(*impl_->session->strand, dispatch_request(std::move(json_msg)),
                          boost::asio::detached);
}

Context Server::make_context(std::shared_ptr<std::atomic<bool>> cancelled,
                             std::optional<ProgressToken> progress_token) {
    return Context(
        *impl_->session->transport,
        [this](std::string method, std::optional<nlohmann::json> params) -> Task<nlohmann::json> {
            co_return co_await send_request(std::move(method), std::move(params));
        },
        std::move(cancelled), std::move(progress_token), &impl_->log_level);
}

// Exceptions from handlers are caught and reported as INTERNAL_ERROR (-32603) responses.
Task<void> Server::dispatch_request(nlohmann::json json_msg) {
    RequestId req_id;
    std::optional<std::string> internal_error_message;

    try {
        req_id = json_msg.at("id").get<RequestId>();
        auto request = json_msg.get<JSONRPCRequest>();

        if (request.method == "initialize") {
            co_await handle_initialize(request);
        } else if (request.method == "ping") {
            co_await handle_ping(request);
        } else if (request.method == "shutdown") {
            co_await handle_shutdown(request);
        } else if (request.method == "tools/call") {
            co_await handle_tools_call(request);
        } else if (request.method == "tools/list") {
            co_await handle_tools_list(request);
        } else if (request.method == "resources/list") {
            co_await handle_resources_list(request);
        } else if (request.method == "resources/read") {
            co_await handle_resources_read(request);
        } else if (request.method == "resources/templates/list") {
            co_await handle_resource_templates_list(request);
        } else if (request.method == "resources/subscribe") {
            co_await handle_subscribe(request);
        } else if (request.method == "resources/unsubscribe") {
            co_await handle_unsubscribe(request);
        } else if (request.method == "prompts/list") {
            co_await handle_prompts_list(request);
        } else if (request.method == "prompts/get") {
            co_await handle_prompts_get(request);
        } else if (request.method == "logging/setLevel") {
            co_await handle_set_level(request);
        } else if (request.method == "completion/complete") {
            co_await handle_complete(request);
        } else {
            co_await send_error(request.id, METHOD_NOT_FOUND, "Method not found: " + request.method);
        }
    } catch (const std::exception& e) {
        if (json_msg.contains("id")) {
            internal_error_message = e.what();
        }
    }

    if (internal_error_message) {
        co_await send_error(req_id, INTERNAL_ERROR, *internal_error_message);
    }
}

void Server::dispatch_notification(const nlohmann::json& json_msg) {
    if (!json_msg.contains("method")) {
        return;
    }
    auto method = json_msg.at("method").get<std::string>();

    if (method == "notifications/cancelled") {
        if (!json_msg.contains("params")) {
            return;
        }
        auto params = json_msg.at("params").get<CancelledNotificationParams>();
        auto id_str = params.requestId.to_string();

        auto it = impl_->session->in_flight.find(id_str);
        if (it != impl_->session->in_flight.end()) {
            it->second->store(true, std::memory_order_relaxed);
        }
    }
}

void Server::dispatch_response(const nlohmann::json& json_msg) {
    auto id = json_msg.at("id").get<RequestId>();
    auto id_str = id.to_string();

    auto it = impl_->session->pending_requests.find(id_str);
    if (it == impl_->session->pending_requests.end()) {
        return;
    }

    if (json_msg.contains("error")) {
        it->second.error = json_msg.at("error").get<Error>();
    } else if (json_msg.contains("result")) {
        it->second.result = json_msg.at("result");
    }

    it->second.timer->cancel();
}

Task<void> Server::handle_initialize(const JSONRPCRequest& request) {
    InitializeResult init_result;
    init_result.protocolVersion = std::string(LATEST_PROTOCOL_VERSION);
    init_result.capabilities = impl_->capabilities;
    init_result.serverInfo = impl_->server_info;

    nlohmann::json result_json = std::move(init_result);
    co_await send_result(request.id, std::move(result_json));
    impl_->initialized = true;
}

Task<void> Server::handle_shutdown(const JSONRPCRequest& request) {
    impl_->shutdown_requested = true;
    co_await send_result(request.id, nlohmann::json::object());
}

Task<void> Server::handle_ping(const JSONRPCRequest& request) {
    co_await send_result(request.id, nlohmann::json::object());
}

Task<void> Server::handle_tools_call(const JSONRPCRequest& request) {
    auto params = request.params.value().get<CallToolParams>();
    auto iter = impl_->tool_handlers.find(params.name);
    if (iter == impl_->tool_handlers.end()) {
        co_await send_error(request.id, METHOD_NOT_FOUND, "Unknown tool: " + params.name);
        co_return;
    }

    auto request_id_str = request.id.to_string();

    auto cancelled = std::make_shared<std::atomic<bool>>(false);
    impl_->session->in_flight[request_id_str] = cancelled;

    std::optional<ProgressToken> progress_token;
    if (params.meta) {
        auto& meta = *params.meta;
        if (meta.contains("progressToken")) {
            progress_token = meta.at("progressToken").get<ProgressToken>();
        }
    }

    TypeErasedHandler wrapped_handler = [original_handler = iter->second](
                                            Context& ctx,
                                            const nlohmann::json& full_params) -> Task<nlohmann::json> {
        auto call_params = full_params.get<CallToolParams>();
        co_return co_await original_handler(ctx, call_params.arguments);
    };
    auto handler = build_middleware_chain(std::move(wrapped_handler));

    auto ctx = make_context(cancelled, std::move(progress_token));
    nlohmann::json params_json = std::move(params);
    nlohmann::json handler_result = co_await handler(ctx, params_json);

    auto has_output_schema =
        has_tool_output_schema(request.params.value().at("name").get<std::string>());
    if (has_output_schema) {
        nlohmann::json structured = handler_result;
        handler_result["structuredContent"] = std::move(structured);
    }

    impl_->session->in_flight.erase(request_id_str);

    co_await send_result(request.id, std::move(handler_result));
}

Task<void> Server::handle_tools_list(const JSONRPCRequest& request) {
    auto page = paginate(impl_->tools.size(), request);
    if (!page) {
        co_await send_error(request.id, INVALID_PARAMS, "Invalid pagination cursor");
        co_return;
    }

    ListToolsResult list_result;
    auto [begin, end, next_cursor] = *page;
    list_result.tools.assign(impl_->tools.begin() + static_cast<std::ptrdiff_t>(begin),
                             impl_->tools.begin() + static_cast<std::ptrdiff_t>(end));
    list_result.nextCursor = std::move(next_cursor);

    nlohmann::json result_json = std::move(list_result);
    co_await send_result(request.id, std::move(result_json));
}

Task<void> Server::handle_resources_list(const JSONRPCRequest& request) {
    auto page = paginate(impl_->resources.size(), request);
    if (!page) {
        co_await send_error(request.id, INVALID_PARAMS, "Invalid pagination cursor");
        co_return;
    }

    ListResourcesResult list_result;
    auto [begin, end, next_cursor] = *page;
    list_result.resources.assign(impl_->resources.begin() + static_cast<std::ptrdiff_t>(begin),
                                 impl_->resources.begin() + static_cast<std::ptrdiff_t>(end));
    list_result.nextCursor = std::move(next_cursor);

    nlohmann::json result_json = std::move(list_result);
    co_await send_result(request.id, std::move(result_json));
}

Task<void> Server::handle_resources_read(const JSONRPCRequest& request) {
    auto params = request.params.value().get<ReadResourceRequestParams>();
    auto iter = impl_->resource_handlers.find(params.uri);
    if (iter == impl_->resource_handlers.end()) {
        co_await send_error(request.id, INVALID_PARAMS, "Unknown resource: " + params.uri);
        co_return;
    }

    auto handler = build_middleware_chain(iter->second);

    nlohmann::json params_json = std::move(params);
    auto ctx = make_context();
    nlohmann::json handler_result = co_await handler(ctx, params_json);
    co_await send_result(request.id, std::move(handler_result));
}

Task<void> Server::handle_resource_templates_list(const JSONRPCRequest& request) {
    auto page = paginate(impl_->resource_templates.size(), request);
    if (!page) {
        co_await send_error(request.id, INVALID_PARAMS, "Invalid pagination cursor");
        co_return;
    }

    ListResourceTemplatesResult list_result;
    auto [begin, end, next_cursor] = *page;
    list_result.resourceTemplates.assign(
        impl_->resource_templates.begin() + static_cast<std::ptrdiff_t>(begin),
        impl_->resource_templates.begin() + static_cast<std::ptrdiff_t>(end));
    list_result.nextCursor = std::move(next_cursor);

    nlohmann::json result_json = std::move(list_result);
    co_await send_result(request.id, std::move(result_json));
}

Task<void> Server::handle_subscribe(const JSONRPCRequest& request) {
    auto params = request.params.value().get<ResourceSubscribeParams>();
    impl_->session->subscriptions[params.uri] = true;
    co_await send_result(request.id, nlohmann::json::object());
    if (impl_->subscribe_handler) {
        impl_->subscribe_handler(params.uri);
    }
}

Task<void> Server::handle_unsubscribe(const JSONRPCRequest& request) {
    auto params = request.params.value().get<ResourceUnsubscribeParams>();
    impl_->session->subscriptions.erase(params.uri);
    co_await send_result(request.id, nlohmann::json::object());
    if (impl_->unsubscribe_handler) {
        impl_->unsubscribe_handler(params.uri);
    }
}

Task<void> Server::handle_prompts_list(const JSONRPCRequest& request) {
    auto page = paginate(impl_->prompts.size(), request);
    if (!page) {
        co_await send_error(request.id, INVALID_PARAMS, "Invalid pagination cursor");
        co_return;
    }

    ListPromptsResult list_result;
    auto [begin, end, next_cursor] = *page;
    list_result.prompts.assign(impl_->prompts.begin() + static_cast<std::ptrdiff_t>(begin),
                               impl_->prompts.begin() + static_cast<std::ptrdiff_t>(end));
    list_result.nextCursor = std::move(next_cursor);

    nlohmann::json result_json = std::move(list_result);
    co_await send_result(request.id, std::move(result_json));
}

Task<void> Server::handle_prompts_get(const JSONRPCRequest& request) {
    auto params = request.params.value().get<GetPromptRequestParams>();
    auto iter = impl_->prompt_handlers.find(params.name);
    if (iter == impl_->prompt_handlers.end()) {
        co_await send_error(request.id, METHOD_NOT_FOUND, "Unknown prompt: " + params.name);
        co_return;
    }

    auto handler = build_middleware_chain(iter->second);

    nlohmann::json params_json = std::move(params);
    auto ctx = make_context();
    nlohmann::json handler_result = co_await handler(ctx, params_json);
    co_await send_result(request.id, std::move(handler_result));
}

Task<void> Server::handle_set_level(const JSONRPCRequest& request) {
    auto params = request.params.value().get<SetLevelRequestParams>();
    impl_->log_level.store(params.level, std::memory_order_relaxed);
    co_await send_result(request.id, nlohmann::json::object());
}

Task<void> Server::handle_complete(const JSONRPCRequest& request) {
    if (!impl_->completion_handler) {
        co_await send_error(request.id, METHOD_NOT_FOUND, "No completion handler registered");
        co_return;
    }

    auto params = request.params.value().get<CompleteParams>();
    auto complete_result = co_await impl_->completion_handler(params);

    nlohmann::json result_json = std::move(complete_result);
    co_await send_result(request.id, std::move(result_json));
}

Task<void> Server::send_result(const RequestId& id, nlohmann::json result) {
    JSONRPCResultResponse response;
    response.id = id;
    response.result = std::move(result);

    nlohmann::json json_msg = std::move(response);
    co_await impl_->session->transport->write_message(json_msg.dump());
}

Task<void> Server::send_error(const RequestId& id, int code, std::string message) {
    Error error;
    error.code = code;
    error.message = std::move(message);

    JSONRPCErrorResponse response;
    response.id = id;
    response.error = std::move(error);

    nlohmann::json json_msg = std::move(response);
    co_await impl_->session->transport->write_message(json_msg.dump());
}

Task<void> Server::send_notification(std::string method, std::optional<nlohmann::json> params) {
    if (!impl_->session) {
        co_return;
    }

    JSONRPCNotification notification;
    notification.method = std::move(method);
    notification.params = std::move(params);

    nlohmann::json json_msg = std::move(notification);
    co_await impl_->session->transport->write_message(json_msg.dump());
}

Task<void> Server::notify_tools_list_changed() {
    co_await send_notification("notifications/tools/list_changed", std::nullopt);
}

Task<void> Server::notify_resources_list_changed() {
    co_await send_notification("notifications/resources/list_changed", std::nullopt);
}

Task<void> Server::notify_prompts_list_changed() {
    co_await send_notification("notifications/prompts/list_changed", std::nullopt);
}

Task<void> Server::notify_resource_updated(const std::string& uri) {
    if (!impl_->session) {
        co_return;
    }
    auto it = impl_->session->subscriptions.find(uri);
    if (it == impl_->session->subscriptions.end() || !it->second) {
        co_return;
    }

    ResourceUpdatedNotificationParams params;
    params.uri = uri;

    JSONRPCNotification notification;
    notification.method = "notifications/resources/updated";
    nlohmann::json p = std::move(params);
    notification.params = std::move(p);

    nlohmann::json json_msg = std::move(notification);
    co_await impl_->session->transport->write_message(json_msg.dump());
}

TypeErasedHandler Server::build_middleware_chain(TypeErasedHandler final_handler) {
    auto handler = std::move(final_handler);
    for (auto it = impl_->middlewares.rbegin(); it != impl_->middlewares.rend(); ++it) {
        auto mw = *it;
        handler = [mw, next = std::move(handler)](
                      Context& ctx, const nlohmann::json& params) -> Task<nlohmann::json> {
            co_return co_await mw(ctx, params, next);
        };
    }
    return handler;
}

std::optional<Server::PaginationSlice> Server::paginate(std::size_t total,
                                                        const JSONRPCRequest& request) {
    if (impl_->page_size == 0 || total == 0) {
        return PaginationSlice{0, total, std::nullopt};
    }

    std::size_t offset = 0;
    if (request.params && request.params->contains("cursor")) {
        auto cursor_str = request.params->at("cursor").get<std::string>();
        try {
            offset = std::stoull(cursor_str);
        } catch (...) {
            return std::nullopt;
        }
    }

    if (offset >= total) {
        offset = 0;
    }

    auto end = std::min(offset + impl_->page_size, total);
    std::optional<std::string> next_cursor;
    if (end < total) {
        next_cursor = std::to_string(end);
    }

    return PaginationSlice{offset, end, std::move(next_cursor)};
}

bool Server::has_tool_output_schema(const std::string& name) const {
    for (const auto& tool : impl_->tools) {
        if (tool.name == name) {
            return tool.outputSchema.has_value();
        }
    }
    return false;
}

void Server::reset_session() {
    if (impl_ && impl_->session) {
        if (impl_->session->transport) {
            impl_->session->transport->close();
        }
        impl_->session->pending_requests.clear();
        impl_->session->in_flight.clear();
        impl_->session->subscriptions.clear();
        impl_->session.reset();
    }
}

}  // namespace mcp

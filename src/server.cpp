#include <mcp/server.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

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

Server::Server(Implementation server_info, ServerCapabilities capabilities)
    : server_info_(std::move(server_info)), capabilities_(std::move(capabilities)) {}

Server::~Server() { reset_session(); }

Task<nlohmann::json> Server::send_request(std::string method, std::optional<nlohmann::json> params) {
    auto id = next_request_id_.fetch_add(1, std::memory_order_relaxed);
    auto id_str = std::to_string(id);

    JSONRPCRequest request;
    request.id = RequestId{id_str};
    request.method = std::move(method);
    request.params = std::move(params);

    auto& pending = session_->pending_requests[id_str];
    pending.timer = std::make_unique<boost::asio::steady_timer>(*session_->strand);
    pending.timer->expires_at(std::chrono::steady_clock::time_point::max());

    nlohmann::json msg = request;
    co_await session_->transport->write_message(msg.dump());

    try {
        co_await pending.timer->async_wait(boost::asio::use_awaitable);
    } catch (const boost::system::system_error& err) {
        if (err.code() != boost::asio::error::operation_aborted) {
            throw;
        }
    }

    auto it = session_->pending_requests.find(id_str);
    if (it == session_->pending_requests.end()) {
        throw std::runtime_error("pending request not found for id: " + id_str);
    }

    auto result = std::move(it->second.result);
    auto error = std::move(it->second.error);
    session_->pending_requests.erase(it);

    if (error) {
        throw std::runtime_error("JSON-RPC error " + std::to_string(error->code) + ": " +
                                 error->message);
    }

    co_return result;
}

Task<void> Server::run(std::unique_ptr<ITransport> transport,
                       const boost::asio::any_io_executor& executor) {
    session_ = std::make_unique<Session>();
    session_->transport = std::move(transport);
    session_->strand = std::make_unique<boost::asio::strand<boost::asio::any_io_executor>>(
        boost::asio::make_strand(executor));

    try {
        for (;;) {
            auto raw = co_await session_->transport->read_message();
            auto json_msg = nlohmann::json::parse(raw);
            dispatch(std::move(json_msg));
        }
    } catch (const std::exception&) {
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

    boost::asio::co_spawn(*session_->strand, dispatch_request(std::move(json_msg)),
                          boost::asio::detached);
}

Context Server::make_context(std::shared_ptr<std::atomic<bool>> cancelled,
                             std::optional<ProgressToken> progress_token) {
    return Context(
        *session_->transport,
        [this](std::string method, std::optional<nlohmann::json> params) -> Task<nlohmann::json> {
            co_return co_await send_request(std::move(method), std::move(params));
        },
        std::move(cancelled), std::move(progress_token), &log_level_);
}

Task<void> Server::dispatch_request(nlohmann::json json_msg) {
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
        auto id_str = std::visit(
            [](auto&& val) -> std::string {
                if constexpr (std::is_same_v<std::decay_t<decltype(val)>, std::string>) {
                    return val;
                } else {
                    return std::to_string(val);
                }
            },
            params.requestId);

        auto it = session_->in_flight.find(id_str);
        if (it != session_->in_flight.end()) {
            it->second->store(true, std::memory_order_relaxed);
        }
    }
}

void Server::dispatch_response(const nlohmann::json& json_msg) {
    auto id = json_msg.at("id").get<RequestId>();
    auto id_str = std::visit(
        [](auto&& val) -> std::string {
            if constexpr (std::is_same_v<std::decay_t<decltype(val)>, std::string>) {
                return val;
            } else {
                return std::to_string(val);
            }
        },
        id);

    auto it = session_->pending_requests.find(id_str);
    if (it == session_->pending_requests.end()) {
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
    InitializeResult result;
    result.protocolVersion = std::string(LATEST_PROTOCOL_VERSION);
    result.capabilities = capabilities_;
    result.serverInfo = server_info_;

    nlohmann::json result_json = std::move(result);
    co_await send_result(request.id, std::move(result_json));
    initialized_ = true;
}

Task<void> Server::handle_shutdown(const JSONRPCRequest& request) {
    shutdown_requested_ = true;
    co_await send_result(request.id, nlohmann::json::object());
}

Task<void> Server::handle_ping(const JSONRPCRequest& request) {
    co_await send_result(request.id, nlohmann::json::object());
}

Task<void> Server::handle_tools_call(const JSONRPCRequest& request) {
    auto params = request.params.value().get<CallToolParams>();
    auto iter = tool_handlers_.find(params.name);
    if (iter == tool_handlers_.end()) {
        co_await send_error(request.id, METHOD_NOT_FOUND, "Unknown tool: " + params.name);
        co_return;
    }

    auto request_id_str = std::visit(
        [](auto&& val) -> std::string {
            if constexpr (std::is_same_v<std::decay_t<decltype(val)>, std::string>) {
                return val;
            } else {
                return std::to_string(val);
            }
        },
        request.id);

    auto cancelled = std::make_shared<std::atomic<bool>>(false);
    session_->in_flight[request_id_str] = cancelled;

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
    nlohmann::json result = co_await handler(ctx, params_json);

    auto has_output_schema =
        has_tool_output_schema(request.params.value().at("name").get<std::string>());
    if (has_output_schema) {
        nlohmann::json structured = result;
        result["structuredContent"] = std::move(structured);
    }

    session_->in_flight.erase(request_id_str);

    co_await send_result(request.id, std::move(result));
}

Task<void> Server::handle_tools_list(const JSONRPCRequest& request) {
    auto page = paginate(tools_.size(), request);
    if (!page) {
        co_await send_error(request.id, INVALID_PARAMS, "Invalid pagination cursor");
        co_return;
    }

    ListToolsResult result;
    auto [begin, end, next_cursor] = *page;
    result.tools.assign(tools_.begin() + static_cast<std::ptrdiff_t>(begin),
                        tools_.begin() + static_cast<std::ptrdiff_t>(end));
    result.nextCursor = std::move(next_cursor);

    nlohmann::json result_json = std::move(result);
    co_await send_result(request.id, std::move(result_json));
}

Task<void> Server::handle_resources_list(const JSONRPCRequest& request) {
    auto page = paginate(resources_.size(), request);
    if (!page) {
        co_await send_error(request.id, INVALID_PARAMS, "Invalid pagination cursor");
        co_return;
    }

    ListResourcesResult result;
    auto [begin, end, next_cursor] = *page;
    result.resources.assign(resources_.begin() + static_cast<std::ptrdiff_t>(begin),
                            resources_.begin() + static_cast<std::ptrdiff_t>(end));
    result.nextCursor = std::move(next_cursor);

    nlohmann::json result_json = std::move(result);
    co_await send_result(request.id, std::move(result_json));
}

Task<void> Server::handle_resources_read(const JSONRPCRequest& request) {
    auto params = request.params.value().get<ReadResourceRequestParams>();
    auto iter = resource_handlers_.find(params.uri);
    if (iter == resource_handlers_.end()) {
        co_await send_error(request.id, INVALID_PARAMS, "Unknown resource: " + params.uri);
        co_return;
    }

    auto handler = build_middleware_chain(iter->second);

    nlohmann::json params_json = std::move(params);
    auto ctx = make_context();
    nlohmann::json result = co_await handler(ctx, params_json);
    co_await send_result(request.id, std::move(result));
}

Task<void> Server::handle_resource_templates_list(const JSONRPCRequest& request) {
    auto page = paginate(resource_templates_.size(), request);
    if (!page) {
        co_await send_error(request.id, INVALID_PARAMS, "Invalid pagination cursor");
        co_return;
    }

    ListResourceTemplatesResult result;
    auto [begin, end, next_cursor] = *page;
    result.resourceTemplates.assign(resource_templates_.begin() + static_cast<std::ptrdiff_t>(begin),
                                    resource_templates_.begin() + static_cast<std::ptrdiff_t>(end));
    result.nextCursor = std::move(next_cursor);

    nlohmann::json result_json = std::move(result);
    co_await send_result(request.id, std::move(result_json));
}

Task<void> Server::handle_subscribe(const JSONRPCRequest& request) {
    auto params = request.params.value().get<ResourceSubscribeParams>();
    session_->subscriptions[params.uri] = true;
    co_await send_result(request.id, nlohmann::json::object());
    if (subscribe_handler_) {
        subscribe_handler_(params.uri);
    }
}

Task<void> Server::handle_unsubscribe(const JSONRPCRequest& request) {
    auto params = request.params.value().get<ResourceUnsubscribeParams>();
    session_->subscriptions.erase(params.uri);
    co_await send_result(request.id, nlohmann::json::object());
    if (unsubscribe_handler_) {
        unsubscribe_handler_(params.uri);
    }
}

Task<void> Server::handle_prompts_list(const JSONRPCRequest& request) {
    auto page = paginate(prompts_.size(), request);
    if (!page) {
        co_await send_error(request.id, INVALID_PARAMS, "Invalid pagination cursor");
        co_return;
    }

    ListPromptsResult result;
    auto [begin, end, next_cursor] = *page;
    result.prompts.assign(prompts_.begin() + static_cast<std::ptrdiff_t>(begin),
                          prompts_.begin() + static_cast<std::ptrdiff_t>(end));
    result.nextCursor = std::move(next_cursor);

    nlohmann::json result_json = std::move(result);
    co_await send_result(request.id, std::move(result_json));
}

Task<void> Server::handle_prompts_get(const JSONRPCRequest& request) {
    auto params = request.params.value().get<GetPromptRequestParams>();
    auto iter = prompt_handlers_.find(params.name);
    if (iter == prompt_handlers_.end()) {
        co_await send_error(request.id, METHOD_NOT_FOUND, "Unknown prompt: " + params.name);
        co_return;
    }

    auto handler = build_middleware_chain(iter->second);

    nlohmann::json params_json = std::move(params);
    auto ctx = make_context();
    nlohmann::json result = co_await handler(ctx, params_json);
    co_await send_result(request.id, std::move(result));
}

Task<void> Server::handle_set_level(const JSONRPCRequest& request) {
    auto params = request.params.value().get<SetLevelRequestParams>();
    log_level_.store(params.level, std::memory_order_relaxed);
    co_await send_result(request.id, nlohmann::json::object());
}

Task<void> Server::handle_complete(const JSONRPCRequest& request) {
    if (!completion_handler_) {
        co_await send_error(request.id, METHOD_NOT_FOUND, "No completion handler registered");
        co_return;
    }

    auto params = request.params.value().get<CompleteParams>();
    auto result = co_await completion_handler_(params);

    nlohmann::json result_json = std::move(result);
    co_await send_result(request.id, std::move(result_json));
}

Task<void> Server::send_result(const RequestId& id, nlohmann::json result) {
    JSONRPCResultResponse response;
    response.id = id;
    response.result = std::move(result);

    nlohmann::json json_msg = std::move(response);
    co_await session_->transport->write_message(json_msg.dump());
}

Task<void> Server::send_error(const RequestId& id, int code, std::string message) {
    Error error;
    error.code = code;
    error.message = std::move(message);

    JSONRPCErrorResponse response;
    response.id = id;
    response.error = std::move(error);

    nlohmann::json json_msg = std::move(response);
    co_await session_->transport->write_message(json_msg.dump());
}

Task<void> Server::send_notification(std::string method, std::optional<nlohmann::json> params) {
    if (!session_) {
        co_return;
    }

    JSONRPCNotification notification;
    notification.method = std::move(method);
    notification.params = std::move(params);

    nlohmann::json json_msg = std::move(notification);
    co_await session_->transport->write_message(json_msg.dump());
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
    if (!session_) {
        co_return;
    }
    auto it = session_->subscriptions.find(uri);
    if (it == session_->subscriptions.end() || !it->second) {
        co_return;
    }

    ResourceUpdatedNotificationParams params;
    params.uri = uri;

    JSONRPCNotification notification;
    notification.method = "notifications/resources/updated";
    nlohmann::json p = std::move(params);
    notification.params = std::move(p);

    nlohmann::json json_msg = std::move(notification);
    co_await session_->transport->write_message(json_msg.dump());
}

TypeErasedHandler Server::build_middleware_chain(TypeErasedHandler final_handler) {
    auto handler = std::move(final_handler);
    for (auto it = middlewares_.rbegin(); it != middlewares_.rend(); ++it) {
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
    if (page_size_ == 0 || total == 0) {
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

    auto end = std::min(offset + page_size_, total);
    std::optional<std::string> next_cursor;
    if (end < total) {
        next_cursor = std::to_string(end);
    }

    return PaginationSlice{offset, end, std::move(next_cursor)};
}

bool Server::has_tool_output_schema(const std::string& name) const {
    for (const auto& tool : tools_) {
        if (tool.name == name) {
            return tool.outputSchema.has_value();
        }
    }
    return false;
}

void Server::reset_session() {
    if (session_) {
        if (session_->transport) {
            session_->transport->close();
        }
        session_->pending_requests.clear();
        session_->in_flight.clear();
        session_->subscriptions.clear();
        session_.reset();
    }
}

}  // namespace mcp

#pragma once

#include <mcp/concepts.hpp>
#include <mcp/context.hpp>
#include <mcp/core.hpp>
#include <mcp/protocol.hpp>
#include <mcp/transport.hpp>

#include <atomic>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace mcp {

/// @brief Type-erased handler: takes a Context and JSON params, returns JSON result.
using TypeErasedHandler = std::function<Task<nlohmann::json>(Context&, const nlohmann::json&)>;

/// @brief Middleware function signature for intercepting handler invocations.
using Middleware =
    std::function<Task<nlohmann::json>(Context&, const nlohmann::json&, TypeErasedHandler)>;

/// @brief Completion handler: takes CompleteParams, returns CompleteResult.
using CompletionHandler = std::function<Task<CompleteResult>(const CompleteParams&)>;

namespace detail {

/// @brief Detects whether Fn is invocable with (In) returning Task<Out>.
template <typename Fn, typename In, typename Out>
concept AsyncHandlerNoCtx = requires(Fn fn, In in) {
    { fn(std::move(in)) } -> std::same_as<Task<Out>>;
};

/// @brief Detects whether Fn is invocable with (Context&, In) returning Task<Out>.
template <typename Fn, typename In, typename Out>
concept AsyncHandlerWithCtx = requires(Fn fn, Context& ctx, In in) {
    { fn(ctx, std::move(in)) } -> std::same_as<Task<Out>>;
};

/// @brief Detects whether Fn is invocable with (In) returning Out (sync, no context).
template <typename Fn, typename In, typename Out>
concept SyncHandlerNoCtx = requires(Fn fn, In in) {
    { fn(std::move(in)) } -> std::same_as<Out>;
};

/// @brief Detects whether Fn is invocable with (Context&, In) returning Out (sync, with context).
template <typename Fn, typename In, typename Out>
concept SyncHandlerWithCtx = requires(Fn fn, Context& ctx, In in) {
    { fn(ctx, std::move(in)) } -> std::same_as<Out>;
};

/**
 * @brief Wraps a handler of any supported signature into a TypeErasedHandler.
 *
 * @details Supports four handler signatures via if constexpr:
 * - Fn(In) -> Task<Out>         (async, no context)
 * - Fn(Context&, In) -> Task<Out>  (async, with context)
 * - Fn(In) -> Out               (sync, no context)
 * - Fn(Context&, In) -> Out     (sync, with context)
 *
 * @tparam In  The input type (must satisfy JsonSerializable).
 * @tparam Out The output type (must satisfy JsonSerializable).
 * @tparam Fn  The handler callable type.
 * @param fn The handler to wrap.
 * @return A TypeErasedHandler that deserializes JSON to In, calls fn, and serializes Out to JSON.
 */
template <JsonSerializable In, JsonSerializable Out, typename Fn>
    requires AsyncHandlerWithCtx<Fn, In, Out> || AsyncHandlerNoCtx<Fn, In, Out> ||
             SyncHandlerWithCtx<Fn, In, Out> || SyncHandlerNoCtx<Fn, In, Out>
TypeErasedHandler wrap_handler(Fn fn) {
    return
        [handler = std::move(fn)](Context& ctx, const nlohmann::json& params) -> Task<nlohmann::json> {
            auto input = params.template get<In>();
            if constexpr (AsyncHandlerWithCtx<Fn, In, Out>) {
                Out output = co_await handler(ctx, std::move(input));
                nlohmann::json result = std::move(output);
                co_return result;
            } else if constexpr (AsyncHandlerNoCtx<Fn, In, Out>) {
                Out output = co_await handler(std::move(input));
                nlohmann::json result = std::move(output);
                co_return result;
            } else if constexpr (SyncHandlerWithCtx<Fn, In, Out>) {
                Out output = handler(ctx, std::move(input));
                nlohmann::json result = std::move(output);
                co_return result;
            } else {
                Out output = handler(std::move(input));
                nlohmann::json result = std::move(output);
                co_return result;
            }
        };
}

}  // namespace detail

/**
 * @brief MCP server that handles client requests over a transport.
 *
 * @details Manages the JSON-RPC message loop, dispatching incoming
 * requests to the appropriate handler. Supports the MCP initialize
 * handshake and shutdown flow, as well as tool, resource, and prompt
 * handler registration and dispatch. Creates a Context per request
 * for handler use.
 *
 * All async operations execute on a boost::asio::strand for thread
 * safety without std::mutex.
 */
class Server {
   public:
    /**
     * @brief Construct a Server.
     *
     * @param server_info Information about this server implementation.
     * @param capabilities The capabilities this server advertises.
     */
    Server(Implementation server_info, ServerCapabilities capabilities)
        : server_info_(std::move(server_info)), capabilities_(std::move(capabilities)) {}

    ~Server() { reset_session(); }

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
    Server(Server&&) = delete;
    Server& operator=(Server&&) = delete;

    /**
     * @brief Register a tool with the server.
     *
     * @details Stores the tool metadata and wraps the handler into a
     * type-erased function. The handler is called when a tools/call
     * request matches the tool's name.
     *
     * @tparam In  The input type for the handler (must satisfy JsonSerializable).
     * @tparam Out The output type for the handler (must satisfy JsonSerializable).
     * @tparam Fn  The handler callable type (sync/async, with/without Context).
     * @param name The name of the tool.
     * @param description A human-readable description of the tool.
     * @param input_schema The JSON schema describing the tool's input.
     * @param handler The handler function to invoke on tools/call.
     */
    template <JsonSerializable In, JsonSerializable Out, typename Fn>
    void add_tool(std::string name, std::string description, nlohmann::json input_schema, Fn handler) {
        Tool tool;
        tool.name = name;
        tool.description = std::move(description);
        tool.inputSchema = std::move(input_schema);
        tools_.push_back(std::move(tool));

        tool_handlers_.emplace(std::move(name), detail::wrap_handler<In, Out>(std::move(handler)));
    }

    /**
     * @brief Register a tool with an output schema for structured output.
     *
     * @details Like add_tool, but also sets the tool's outputSchema. When a tool has
     * an outputSchema, its result will include a structuredContent field alongside content.
     */
    template <JsonSerializable In, JsonSerializable Out, typename Fn>
    void add_tool(std::string name, std::string description, nlohmann::json input_schema,
                  nlohmann::json output_schema, Fn handler) {
        Tool tool;
        tool.name = name;
        tool.description = std::move(description);
        tool.inputSchema = std::move(input_schema);
        tool.outputSchema = std::move(output_schema);
        tools_.push_back(std::move(tool));

        tool_handlers_.emplace(std::move(name), detail::wrap_handler<In, Out>(std::move(handler)));
    }

    /**
     * @brief Register a resource with the server.
     *
     * @details Stores the resource metadata and wraps the handler into a
     * type-erased function. The handler is called when a resources/read
     * request matches the resource's URI.
     *
     * @tparam In  The input type for the handler (must satisfy JsonSerializable).
     * @tparam Out The output type for the handler (must satisfy JsonSerializable).
     * @tparam Fn  The handler callable type (sync/async, with/without Context).
     * @param resource The resource metadata.
     * @param handler The handler function to invoke on resources/read.
     */
    template <JsonSerializable In, JsonSerializable Out, typename Fn>
    void add_resource(Resource resource, Fn handler) {
        auto uri = resource.uri;
        resources_.push_back(std::move(resource));
        resource_handlers_.emplace(std::move(uri), detail::wrap_handler<In, Out>(std::move(handler)));
    }

    /**
     * @brief Register a resource template with the server.
     *
     * @details Stores the resource template metadata for listing.
     * Resource templates describe parameterized resources that clients
     * can discover via resources/templates/list.
     *
     * @param tmpl The resource template metadata.
     */
    void add_resource_template(ResourceTemplate tmpl) {
        resource_templates_.push_back(std::move(tmpl));
    }

    /**
     * @brief Register a prompt with the server.
     *
     * @details Stores the prompt metadata and wraps the handler into a
     * type-erased function. The handler is called when a prompts/get
     * request matches the prompt's name.
     *
     * @tparam In  The input type for the handler (must satisfy JsonSerializable).
     * @tparam Out The output type for the handler (must satisfy JsonSerializable).
     * @tparam Fn  The handler callable type (sync/async, with/without Context).
     * @param prompt The prompt metadata.
     * @param handler The handler function to invoke on prompts/get.
     */
    template <JsonSerializable In, JsonSerializable Out, typename Fn>
    void add_prompt(Prompt prompt, Fn handler) {
        auto name = prompt.name;
        prompts_.push_back(std::move(prompt));
        prompt_handlers_.emplace(std::move(name), detail::wrap_handler<In, Out>(std::move(handler)));
    }

    /**
     * @brief Register a middleware function to intercept handler invocations.
     *
     * @details Middleware functions execute in registration order (first registered = outermost).
     * Each middleware receives the context, request parameters, and a continuation to invoke
     * the next middleware in the chain (or the final handler). Middleware can modify parameters,
     * short-circuit execution, or post-process results.
     *
     * @param mw The middleware function to register.
     */
    void use(Middleware mw) { middlewares_.push_back(std::move(mw)); }

    /**
     * @brief Register a completion handler for completion/complete requests.
     *
     * @param handler The handler that provides completion results.
     */
    void set_completion_provider(CompletionHandler handler) {
        completion_handler_ = std::move(handler);
    }

    /**
     * @brief Set the page size for paginated list responses.
     *
     * @param size The maximum number of items per page. 0 means no pagination.
     */
    void set_page_size(std::size_t size) { page_size_ = size; }

    /**
     * @brief Send a JSON-RPC request to the client and await its response (reverse RPC).
     *
     * @details Assigns a unique integer ID, sends the request over the
     * transport, then suspends the calling coroutine until the dispatch
     * loop routes the matching response back to this pending request.
     *
     * @param method The JSON-RPC method name.
     * @param params Optional parameters for the request.
     * @return A task that resolves to the result JSON.
     * @throws std::runtime_error If the client returns an error response.
     */
    Task<nlohmann::json> send_request(std::string method, std::optional<nlohmann::json> params) {
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

    /**
     * @brief Start the server session loop on the given transport.
     *
     * @details Reads messages from the transport, parses them as
     * JSON-RPC, and dispatches each to the appropriate handler.
     * Request handlers are co_spawned so the read loop can continue
     * processing incoming messages (required for reverse RPC).
     * The loop runs until the transport is closed or an error occurs.
     *
     * @param transport The transport to use for message exchange.
     *                  Ownership is transferred to the server.
     * @param executor The executor to use for async operations.
     */
    Task<void> run(std::unique_ptr<ITransport> transport,
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

    /**
     * @brief Dispatch a parsed JSON-RPC message to the appropriate handler.
     *
     * @details Non-coroutine entry point that routes messages. Responses
     * (messages with "id" but no "method") are handled synchronously via
     * dispatch_response(). Requests are co_spawned as independent
     * coroutines so the run loop is never blocked—this is essential for
     * reverse RPC (e.g. sampling) where a handler sends a request to the
     * client and must wait for a response that arrives on the same
     * transport. Notifications (messages without "id") are dispatched
     * to notification handlers (e.g. cancellation).
     *
     * @param json_msg The parsed JSON-RPC message.
     */
    void dispatch(nlohmann::json json_msg) {
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

    /**
     * @brief Check whether the server has been initialized.
     *
     * @return true if an initialize request has been handled.
     */
    [[nodiscard]] bool is_initialized() const { return initialized_; }

    /**
     * @brief Check whether a shutdown has been requested.
     *
     * @return true if a shutdown request has been handled.
     */
    [[nodiscard]] bool is_shutdown_requested() const { return shutdown_requested_; }

    /**
     * @brief Send a notifications/tools/list_changed notification to the client.
     */
    Task<void> notify_tools_list_changed() {
        co_await send_notification("notifications/tools/list_changed", std::nullopt);
    }

    /**
     * @brief Send a notifications/resources/list_changed notification to the client.
     */
    Task<void> notify_resources_list_changed() {
        co_await send_notification("notifications/resources/list_changed", std::nullopt);
    }

    /**
     * @brief Send a notifications/prompts/list_changed notification to the client.
     */
    Task<void> notify_prompts_list_changed() {
        co_await send_notification("notifications/prompts/list_changed", std::nullopt);
    }

    /**
     * @brief Send a notifications/resources/updated notification to all subscribers.
     *
     * @param uri The URI of the resource that was updated.
     */
    Task<void> notify_resource_updated(const std::string& uri) {
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

    using SubscriptionHandler = std::function<void(const std::string& uri)>;

    void on_subscribe(SubscriptionHandler handler) { subscribe_handler_ = std::move(handler); }

    void on_unsubscribe(SubscriptionHandler handler) { unsubscribe_handler_ = std::move(handler); }

    [[nodiscard]] LoggingLevel get_log_level() const {
        return log_level_.load(std::memory_order_relaxed);
    }

   private:
    Context make_context(std::shared_ptr<std::atomic<bool>> cancelled = nullptr,
                         std::optional<ProgressToken> progress_token = std::nullopt) {
        return Context(
            *session_->transport,
            [this](std::string method, std::optional<nlohmann::json> params) -> Task<nlohmann::json> {
                co_return co_await send_request(std::move(method), std::move(params));
            },
            std::move(cancelled), std::move(progress_token), &log_level_);
    }

    /**
     * @brief Handle a JSON-RPC request as a coroutine.
     *
     * @details Routes the request based on the "method" field. Handles
     * "initialize" and "shutdown" internally. Routes tool, resource,
     * and prompt methods to their respective handlers. Returns a
     * JSON-RPC error response for unknown methods.
     *
     * Takes json_msg by value because the coroutine is co_spawned
     * and may outlive the caller's stack frame.
     *
     * @param json_msg The parsed JSON-RPC request message (owned).
     */
    Task<void> dispatch_request(nlohmann::json json_msg) {
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

    void dispatch_notification(const nlohmann::json& json_msg) {
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

    void dispatch_response(const nlohmann::json& json_msg) {
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

    Task<void> handle_initialize(const JSONRPCRequest& request) {
        InitializeResult result;
        result.protocolVersion = std::string(LATEST_PROTOCOL_VERSION);
        result.capabilities = capabilities_;
        result.serverInfo = server_info_;

        nlohmann::json result_json = std::move(result);
        co_await send_result(request.id, std::move(result_json));
        initialized_ = true;
    }

    Task<void> handle_shutdown(const JSONRPCRequest& request) {
        shutdown_requested_ = true;
        co_await send_result(request.id, nlohmann::json::object());
    }

    Task<void> handle_ping(const JSONRPCRequest& request) {
        co_await send_result(request.id, nlohmann::json::object());
    }

    Task<void> handle_tools_call(const JSONRPCRequest& request) {
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

        TypeErasedHandler wrapped_handler =
            [original_handler = iter->second](
                Context& ctx, const nlohmann::json& full_params) -> Task<nlohmann::json> {
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

    Task<void> handle_tools_list(const JSONRPCRequest& request) {
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

    Task<void> handle_resources_list(const JSONRPCRequest& request) {
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

    Task<void> handle_resources_read(const JSONRPCRequest& request) {
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

    Task<void> handle_resource_templates_list(const JSONRPCRequest& request) {
        auto page = paginate(resource_templates_.size(), request);
        if (!page) {
            co_await send_error(request.id, INVALID_PARAMS, "Invalid pagination cursor");
            co_return;
        }

        ListResourceTemplatesResult result;
        auto [begin, end, next_cursor] = *page;
        result.resourceTemplates.assign(
            resource_templates_.begin() + static_cast<std::ptrdiff_t>(begin),
            resource_templates_.begin() + static_cast<std::ptrdiff_t>(end));
        result.nextCursor = std::move(next_cursor);

        nlohmann::json result_json = std::move(result);
        co_await send_result(request.id, std::move(result_json));
    }

    Task<void> handle_subscribe(const JSONRPCRequest& request) {
        auto params = request.params.value().get<ResourceSubscribeParams>();
        session_->subscriptions[params.uri] = true;
        co_await send_result(request.id, nlohmann::json::object());
        if (subscribe_handler_) {
            subscribe_handler_(params.uri);
        }
    }

    Task<void> handle_unsubscribe(const JSONRPCRequest& request) {
        auto params = request.params.value().get<ResourceUnsubscribeParams>();
        session_->subscriptions.erase(params.uri);
        co_await send_result(request.id, nlohmann::json::object());
        if (unsubscribe_handler_) {
            unsubscribe_handler_(params.uri);
        }
    }

    Task<void> handle_prompts_list(const JSONRPCRequest& request) {
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

    Task<void> handle_prompts_get(const JSONRPCRequest& request) {
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

    Task<void> handle_set_level(const JSONRPCRequest& request) {
        auto params = request.params.value().get<SetLevelRequestParams>();
        log_level_.store(params.level, std::memory_order_relaxed);
        co_await send_result(request.id, nlohmann::json::object());
    }

    Task<void> handle_complete(const JSONRPCRequest& request) {
        if (!completion_handler_) {
            co_await send_error(request.id, METHOD_NOT_FOUND, "No completion handler registered");
            co_return;
        }

        auto params = request.params.value().get<CompleteParams>();
        auto result = co_await completion_handler_(params);

        nlohmann::json result_json = std::move(result);
        co_await send_result(request.id, std::move(result_json));
    }

    Task<void> send_result(const RequestId& id, nlohmann::json result) {
        JSONRPCResultResponse response;
        response.id = id;
        response.result = std::move(result);

        nlohmann::json json_msg = std::move(response);
        co_await session_->transport->write_message(json_msg.dump());
    }

    Task<void> send_error(const RequestId& id, int code, std::string message) {
        Error error;
        error.code = code;
        error.message = std::move(message);

        JSONRPCErrorResponse response;
        response.id = id;
        response.error = std::move(error);

        nlohmann::json json_msg = std::move(response);
        co_await session_->transport->write_message(json_msg.dump());
    }

    Task<void> send_notification(std::string method, std::optional<nlohmann::json> params) {
        if (!session_) {
            co_return;
        }

        JSONRPCNotification notification;
        notification.method = std::move(method);
        notification.params = std::move(params);

        nlohmann::json json_msg = std::move(notification);
        co_await session_->transport->write_message(json_msg.dump());
    }

    TypeErasedHandler build_middleware_chain(TypeErasedHandler final_handler) {
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

    struct PaginationSlice {
        std::size_t begin;
        std::size_t end;
        std::optional<std::string> next_cursor;
    };

    std::optional<PaginationSlice> paginate(std::size_t total, const JSONRPCRequest& request) {
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

    [[nodiscard]] bool has_tool_output_schema(const std::string& name) const {
        for (const auto& tool : tools_) {
            if (tool.name == name) {
                return tool.outputSchema.has_value();
            }
        }
        return false;
    }

    Implementation server_info_;
    ServerCapabilities capabilities_;
    bool initialized_ = false;
    bool shutdown_requested_ = false;

    struct PendingRequest {
        std::unique_ptr<boost::asio::steady_timer> timer;
        nlohmann::json result;
        std::optional<Error> error;
    };

    struct Session {
        std::unique_ptr<ITransport> transport;
        std::unique_ptr<boost::asio::strand<boost::asio::any_io_executor>> strand;
        std::map<std::string, PendingRequest> pending_requests;

        std::map<std::string, std::shared_ptr<std::atomic<bool>>> in_flight;

        std::map<std::string, bool> subscriptions;
    };

    std::unique_ptr<Session> session_;
    std::atomic<int64_t> next_request_id_{1};

    std::atomic<LoggingLevel> log_level_{LoggingLevel::Debug};

    std::size_t page_size_ = 0;

    CompletionHandler completion_handler_;

    void reset_session() {
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

    /// @brief Registered tool metadata.
    std::vector<Tool> tools_;
    /// @brief Type-erased tool handlers keyed by tool name.
    std::map<std::string, TypeErasedHandler, std::less<>> tool_handlers_;

    /// @brief Registered resource metadata.
    std::vector<Resource> resources_;
    /// @brief Type-erased resource handlers keyed by resource URI.
    std::map<std::string, TypeErasedHandler, std::less<>> resource_handlers_;

    /// @brief Registered resource template metadata.
    std::vector<ResourceTemplate> resource_templates_;

    /// @brief Registered prompt metadata.
    std::vector<Prompt> prompts_;
    /// @brief Type-erased prompt handlers keyed by prompt name.
    std::map<std::string, TypeErasedHandler, std::less<>> prompt_handlers_;

    /// @brief Registered middleware functions in order of registration.
    std::vector<Middleware> middlewares_;

    SubscriptionHandler subscribe_handler_;
    SubscriptionHandler unsubscribe_handler_;
};

}  // namespace mcp

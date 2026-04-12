#pragma once

#include <mcp/concepts.hpp>
#include <mcp/context.hpp>
#include <mcp/core.hpp>
#include <mcp/protocol.hpp>
#include <mcp/transport.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iosfwd>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace boost::asio {
class any_io_executor;
}

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
    Server(Implementation server_info, ServerCapabilities capabilities);

    ~Server();

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
     * @brief Register a tool with a simple synchronous JSON handler.
     *
     * @details Convenience overload that avoids templates and coroutines.
     * The handler receives raw JSON params and returns a raw JSON result.
     * Exceptions thrown by the handler are propagated as JSON-RPC errors.
     *
     * @param name         The name of the tool.
     * @param description  A human-readable description of the tool.
     * @param input_schema The JSON schema describing the tool's input.
     * @param handler      Synchronous handler: takes JSON params, returns JSON result.
     */
    void add_tool(std::string name, std::string description, nlohmann::json input_schema,
                  std::function<nlohmann::json(const nlohmann::json&)> handler);

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
    Task<nlohmann::json> send_request(std::string method, std::optional<nlohmann::json> params);

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
    Task<void> run(std::unique_ptr<ITransport> transport, const boost::asio::any_io_executor& executor);

    /**
     * @brief Run the server on stdio (stdin/stdout), blocking until shutdown.
     *
     * @details Creates the transport and event loop internally, installs
     * signal handlers for SIGINT and SIGTERM, and blocks until the server
     * session ends or a signal is received. Exceptions from the server
     * session are propagated to the caller.
     */
    void run_stdio();

    /**
     * @brief Run the server on custom streams, blocking until shutdown.
     *
     * @details Same as run_stdio() but reads from / writes to the given
     * streams instead of stdin / stdout.
     *
     * @param input  Stream to read JSON-RPC messages from.
     * @param output Stream to write JSON-RPC responses to.
     */
    void run_stdio(std::istream& input, std::ostream& output);

    /**
     * @brief Run the server over HTTP, blocking until shutdown.
     *
     * @details Binds to the given host and port, accepts HTTP connections,
     * and processes JSON-RPC requests. Installs signal handlers for SIGINT
     * and SIGTERM for clean shutdown. Blocks until the server session ends
     * or a signal is received. Exceptions from the server session are
     * propagated to the caller.
     *
     * @param host  Local address to bind (e.g. "0.0.0.0" or "127.0.0.1").
     * @param port  TCP port to listen on.
     */
    void run_http(const std::string& host, uint16_t port);

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
    void dispatch(nlohmann::json json_msg);

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
    Task<void> notify_tools_list_changed();

    /**
     * @brief Send a notifications/resources/list_changed notification to the client.
     */
    Task<void> notify_resources_list_changed();

    /**
     * @brief Send a notifications/prompts/list_changed notification to the client.
     */
    Task<void> notify_prompts_list_changed();

    /**
     * @brief Send a notifications/resources/updated notification to all subscribers.
     *
     * @param uri The URI of the resource that was updated.
     */
    Task<void> notify_resource_updated(const std::string& uri);

    /// @brief Callback invoked when a client subscribes to or unsubscribes from a resource URI.
    using SubscriptionHandler = std::function<void(const std::string& uri)>;

    /**
     * @brief Register a callback invoked after a successful resource subscription.
     *
     * @param handler Callback receiving the subscribed resource URI.
     */
    void on_subscribe(SubscriptionHandler handler) { subscribe_handler_ = std::move(handler); }

    /**
     * @brief Register a callback invoked after a successful resource unsubscription.
     *
     * @param handler Callback receiving the unsubscribed resource URI.
     */
    void on_unsubscribe(SubscriptionHandler handler) { unsubscribe_handler_ = std::move(handler); }

    /**
     * @brief Get the currently active server log level.
     *
     * @return The log level used when filtering context log messages.
     */
    [[nodiscard]] LoggingLevel get_log_level() const {
        return log_level_.load(std::memory_order_relaxed);
    }

   private:
    Context make_context(std::shared_ptr<std::atomic<bool>> cancelled = nullptr,
                         std::optional<ProgressToken> progress_token = std::nullopt);

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
    Task<void> dispatch_request(nlohmann::json json_msg);

    void dispatch_notification(const nlohmann::json& json_msg);

    void dispatch_response(const nlohmann::json& json_msg);

    Task<void> handle_initialize(const JSONRPCRequest& request);

    Task<void> handle_shutdown(const JSONRPCRequest& request);

    Task<void> handle_ping(const JSONRPCRequest& request);

    Task<void> handle_tools_call(const JSONRPCRequest& request);

    Task<void> handle_tools_list(const JSONRPCRequest& request);

    Task<void> handle_resources_list(const JSONRPCRequest& request);

    Task<void> handle_resources_read(const JSONRPCRequest& request);

    Task<void> handle_resource_templates_list(const JSONRPCRequest& request);

    Task<void> handle_subscribe(const JSONRPCRequest& request);

    Task<void> handle_unsubscribe(const JSONRPCRequest& request);

    Task<void> handle_prompts_list(const JSONRPCRequest& request);

    Task<void> handle_prompts_get(const JSONRPCRequest& request);

    Task<void> handle_set_level(const JSONRPCRequest& request);

    Task<void> handle_complete(const JSONRPCRequest& request);

    Task<void> send_result(const RequestId& id, nlohmann::json result);

    Task<void> send_error(const RequestId& id, int code, std::string message);

    Task<void> send_notification(std::string method, std::optional<nlohmann::json> params);

    TypeErasedHandler build_middleware_chain(TypeErasedHandler final_handler);

    struct PaginationSlice {
        std::size_t begin;
        std::size_t end;
        std::optional<std::string> next_cursor;
    };

    std::optional<PaginationSlice> paginate(std::size_t total, const JSONRPCRequest& request);

    [[nodiscard]] bool has_tool_output_schema(const std::string& name) const;

    Implementation server_info_;
    ServerCapabilities capabilities_;
    bool initialized_ = false;
    bool shutdown_requested_ = false;

    struct PendingRequest;
    struct Session;

    std::unique_ptr<Session> session_;
    std::atomic<int64_t> next_request_id_{1};

    std::atomic<LoggingLevel> log_level_{LoggingLevel::Debug};

    std::size_t page_size_ = 0;

    CompletionHandler completion_handler_;

    void reset_session();

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

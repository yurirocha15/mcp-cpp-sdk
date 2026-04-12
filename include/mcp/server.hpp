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
 * Manages the JSON-RPC message loop, dispatching incoming requests to
 * registered handlers. Supports the MCP initialize handshake and shutdown
 * flow, as well as tool, resource, and prompt handler registration.
 * Creates a Context per request for handler use.
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
     * @tparam In  Handler input type (must satisfy JsonSerializable).
     * @tparam Out Handler output type (must satisfy JsonSerializable).
     * @tparam Fn  Handler callable (sync/async, with/without Context).
     * @param name         Tool name.
     * @param description  Human-readable description.
     * @param input_schema JSON schema for the tool's input parameters.
     * @param handler      Handler invoked on tools/call requests.
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
     * Like add_tool(), but also sets the tool's outputSchema so results include
     * a structuredContent field alongside content.
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
     * Accepts raw JSON params and returns a raw JSON result. Exceptions thrown
     * by the handler are propagated as JSON-RPC errors.
     *
     * @param name         Tool name.
     * @param description  Human-readable description.
     * @param input_schema JSON schema for the tool's input parameters.
     * @param handler      Synchronous handler: takes JSON params, returns JSON result.
     */
    void add_tool(std::string name, std::string description, nlohmann::json input_schema,
                  std::function<nlohmann::json(const nlohmann::json&)> handler);

    /**
     * @brief Register a resource with the server.
     *
     * @tparam In  Handler input type (must satisfy JsonSerializable).
     * @tparam Out Handler output type (must satisfy JsonSerializable).
     * @tparam Fn  Handler callable (sync/async, with/without Context).
     * @param resource Resource metadata.
     * @param handler  Handler invoked on resources/read requests.
     */
    template <JsonSerializable In, JsonSerializable Out, typename Fn>
    void add_resource(Resource resource, Fn handler) {
        auto uri = resource.uri;
        resources_.push_back(std::move(resource));
        resource_handlers_.emplace(std::move(uri), detail::wrap_handler<In, Out>(std::move(handler)));
    }

    /**
     * @brief Register a resource template for listing via resources/templates/list.
     *
     * @param tmpl Resource template metadata.
     */
    void add_resource_template(ResourceTemplate tmpl) {
        resource_templates_.push_back(std::move(tmpl));
    }

    /**
     * @brief Register a prompt with the server.
     *
     * @tparam In  Handler input type (must satisfy JsonSerializable).
     * @tparam Out Handler output type (must satisfy JsonSerializable).
     * @tparam Fn  Handler callable (sync/async, with/without Context).
     * @param prompt  Prompt metadata.
     * @param handler Handler invoked on prompts/get requests.
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
     * Middleware executes in registration order (first registered = outermost).
     * Each middleware receives the context, request parameters, and a
     * continuation to call the next in the chain or the final handler.
     *
     * @param mw Middleware function to register.
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
     * @brief Send a JSON-RPC request to the connected client and await its response.
     *
     * @param method JSON-RPC method name.
     * @param params Optional request parameters.
     * @return A task that resolves to the result JSON.
     * @throws std::runtime_error If the client returns an error response.
     */
    Task<nlohmann::json> send_request(std::string method, std::optional<nlohmann::json> params);

    /**
     * @brief Start the server session loop on the given transport.
     *
     * Runs until the transport is closed or an error occurs.
     *
     * @param transport The transport to use for message exchange. Ownership is transferred.
     * @param executor  The executor to use for async operations.
     */
    Task<void> run(std::unique_ptr<ITransport> transport, const boost::asio::any_io_executor& executor);

    /**
     * @brief Run the server on stdio (stdin/stdout), blocking until shutdown.
     *
     * Handles SIGINT and SIGTERM for clean shutdown. Exceptions from the
     * server session are propagated to the caller.
     */
    void run_stdio();

    /**
     * @brief Run the server on custom streams, blocking until shutdown.
     *
     * Same as run_stdio() but reads from / writes to the given streams
     * instead of stdin / stdout.
     *
     * @param input  Stream to read JSON-RPC messages from.
     * @param output Stream to write JSON-RPC responses to.
     */
    void run_stdio(std::istream& input, std::ostream& output);

    /**
     * @brief Run the server over HTTP, blocking until shutdown.
     *
     * Binds to the given host and port, accepts HTTP connections, and
     * processes JSON-RPC requests. Handles SIGINT and SIGTERM for clean
     * shutdown. Exceptions from the server session are propagated to the caller.
     *
     * @param host  Local address to bind (e.g. "0.0.0.0" or "127.0.0.1").
     * @param port  TCP port to listen on.
     */
    void run_http(const std::string& host, uint16_t port);

    /**
     * @brief Dispatch a parsed JSON-RPC message to the appropriate handler.
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

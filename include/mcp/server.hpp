#pragma once

// GCC 11 SSO Coroutine Safety — see docs/contributing.rst "Known Issues" for full details.
// Do NOT store std::string in a coroutine frame across co_await (GCC bugs #107288/#100611).
// Named patterns referenced at each call site below:
//   [no-named-temporaries] Pass params.get<In>() directly; don't store it in a named local.
//   [wire-builders]        make_result_wire/make_error_wire are synchronous helpers;
//                          do NOT convert them to Task<T> coroutines.

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
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

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
 * @brief Ensures a handler is async by wrapping sync handlers into Task-returning ones.
 *
 * @tparam In  The input type deserialized from JSON.
 * @tparam Out The output type serialized to JSON.
 * @tparam Fn  The handler callable type (may be sync or async).
 *
 * @details This template accepts four handler signatures:
 * - `Task<Out>(Context&, In)` — async with context
 * - `Task<Out>(In)` — async without context
 * - `Out(Context&, In)` — sync with context (wrapped in co_return)
 * - `Out(In)` — sync without context (wrapped in co_return)
 *
 * Sync handlers are automatically lifted into coroutines.
 */
template <typename In, typename Out, typename Fn>
auto ensure_async_handler(Fn fn) {
    if constexpr (AsyncHandlerWithCtx<Fn, In, Out> || AsyncHandlerNoCtx<Fn, In, Out>) {
        return fn;
    } else if constexpr (SyncHandlerWithCtx<Fn, In, Out>) {
        return [handler = std::move(fn)](Context& ctx, In in) -> Task<Out> {
            co_return handler(ctx, std::move(in));
        };
    } else {
        static_assert(SyncHandlerNoCtx<Fn, In, Out>,
                      "Handler must be sync or async with supported signature");
        return [handler = std::move(fn)](In in) -> Task<Out> { co_return handler(std::move(in)); };
    }
}

/**
 * @brief Wraps an asynchronous handler into a TypeErasedHandler.
 *
 * @tparam In  The input type (must satisfy JsonSerializable).
 * @tparam Out The output type (must satisfy JsonSerializable).
 * @tparam Fn  The handler callable type (must be async).
 * @param fn The async handler to wrap.
 * @return A TypeErasedHandler that deserializes JSON to In, calls fn, and serializes Out to JSON.
 */
template <JsonSerializable In, JsonSerializable Out, typename Fn>
    requires AsyncHandlerWithCtx<Fn, In, Out> || AsyncHandlerNoCtx<Fn, In, Out>
TypeErasedHandler wrap_handler(Fn fn) {
    return
        [handler = std::move(fn)](Context& ctx, const nlohmann::json& params) -> Task<nlohmann::json> {
            // [gcc11-sso: no-named-temporaries] DO NOT introduce a named `input` variable here.
            if constexpr (AsyncHandlerWithCtx<Fn, In, Out>) {
                Out output = co_await handler(ctx, params.get<In>());
                co_return nlohmann::json(std::move(output));
            } else {
                Out output = co_await handler(params.get<In>());
                co_return nlohmann::json(std::move(output));
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
    Server(const Implementation& server_info, const ServerCapabilities& capabilities);

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
    void add_tool(const std::string& name, const std::string& description,
                  const nlohmann::json& input_schema, Fn handler) {
        Tool tool;
        tool.name = name;
        tool.description = description;
        tool.inputSchema = input_schema;
        auto async_handler = detail::ensure_async_handler<In, Out>(std::move(handler));
        register_tool(tool, name, detail::wrap_handler<In, Out>(std::move(async_handler)));
    }

    /**
     * @brief Register a tool with an output schema for structured output.
     *
     * Like add_tool(), but also sets the tool's outputSchema so results include
     * a structuredContent field alongside content.
     */
    template <JsonSerializable In, JsonSerializable Out, typename Fn>
    void add_tool(const std::string& name, const std::string& description,
                  const nlohmann::json& input_schema, const nlohmann::json& output_schema, Fn handler) {
        Tool tool;
        tool.name = name;
        tool.description = description;
        tool.inputSchema = input_schema;
        tool.outputSchema = output_schema;
        auto async_handler = detail::ensure_async_handler<In, Out>(std::move(handler));
        register_tool(tool, name, detail::wrap_handler<In, Out>(std::move(async_handler)));
    }

    /**
     * @brief Register a tool with a simple synchronous JSON handler.
     *
     * Accepts raw JSON params and returns a raw JSON result. Exceptions thrown
     * by the handler become a tool error result (isError=true) rather than a
     * JSON-RPC protocol error.
     *
     * @param name         Tool name.
     * @param description  Human-readable description.
     * @param input_schema JSON schema for the tool's input parameters.
     * @param handler      Synchronous handler: takes JSON params, returns JSON result.
     */
    void add_tool(const std::string& name, const std::string& description,
                  const nlohmann::json& input_schema,
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
    void add_resource(const Resource& resource, Fn handler) {
        auto async_handler = detail::ensure_async_handler<In, Out>(std::move(handler));
        register_resource(resource, detail::wrap_handler<In, Out>(std::move(async_handler)));
    }

    /**
     * @brief Register a resource template for listing via resources/templates/list.
     *
     * @param tmpl Resource template metadata.
     */
    void add_resource_template(const ResourceTemplate& tmpl);

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
    void add_prompt(const Prompt& prompt, Fn handler) {
        auto async_handler = detail::ensure_async_handler<In, Out>(std::move(handler));
        register_prompt(prompt, detail::wrap_handler<In, Out>(std::move(async_handler)));
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
    void use(Middleware mw);

    /**
     * @brief Register a completion handler for completion/complete requests.
     *
     * @param handler The handler that provides completion results.
     */
    void set_completion_provider(CompletionHandler handler);

    /**
     * @brief Set the page size for paginated list responses.
     *
     * @param size The maximum number of items per page. 0 means no pagination.
     */
    void set_page_size(std::size_t size);

    /**
     * @brief Send a JSON-RPC request to the connected client and await its response.
     *
     * @param method JSON-RPC method name.
     * @param params Optional request parameters.
     * @return A task that resolves to the result JSON.
     * @throws std::runtime_error If the client returns an error response.
     */
    Task<nlohmann::json> send_request(const std::string& method,
                                      const std::optional<nlohmann::json>& params);

    /**
     * @brief Start the server session loop on the given transport.
     *
     * Runs until the transport is closed or an error occurs.
     *
     * @param transport The transport to use for message exchange. Ownership is shared.
     * @param executor  The executor to use for async operations.
     */
    Task<void> run(std::shared_ptr<ITransport> transport, boost::asio::any_io_executor executor);

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
    [[nodiscard]] bool is_initialized() const;

    /**
     * @brief Check whether a shutdown has been requested.
     *
     * @return true if a shutdown request has been handled.
     */
    [[nodiscard]] bool is_shutdown_requested() const;

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
    void on_subscribe(const SubscriptionHandler& handler);

    /**
     * @brief Register a callback invoked after a successful resource unsubscription.
     *
     * @param handler Callback receiving the unsubscribed resource URI.
     */
    void on_unsubscribe(const SubscriptionHandler& handler);

    /**
     * @brief Get the currently active server log level.
     *
     * @return The log level used when filtering context log messages.
     */
    [[nodiscard]] LoggingLevel get_log_level() const;

   private:
    // Non-template registration helpers called by the template add_* methods above.
    void register_tool(const Tool& tool, const std::string& name, TypeErasedHandler handler);
    void register_resource(const Resource& resource, TypeErasedHandler handler);
    void register_prompt(const Prompt& prompt, TypeErasedHandler handler);

    Context make_context(std::shared_ptr<std::atomic<bool>> cancelled = nullptr,
                         std::optional<ProgressToken> progress_token = std::nullopt);

    /**
     * @brief Dispatches an incoming JSON-RPC request to the appropriate registered handler.
     *
     * @details Exceptions thrown during dispatch are caught and returned as `g_INTERNAL_ERROR`
     * (-32603) JSON-RPC error responses. The exception's `what()` message becomes the error data.
     *
     * @param json_msg The raw JSON-RPC request object.
     */
    Task<void> dispatch_request(nlohmann::json json_msg);

    void dispatch_notification(const nlohmann::json& json_msg);

    void dispatch_response(const nlohmann::json& json_msg);

    Task<void> handle_initialize(const nlohmann::json& json_msg);

    Task<void> handle_shutdown(const nlohmann::json& json_msg);

    Task<void> handle_ping(const nlohmann::json& json_msg);

    Task<void> handle_tools_call(const nlohmann::json& json_msg);

    Task<void> handle_tools_list(const nlohmann::json& json_msg);

    Task<void> handle_resources_list(const nlohmann::json& json_msg);

    Task<void> handle_resources_read(const nlohmann::json& json_msg);

    Task<void> handle_resource_templates_list(const nlohmann::json& json_msg);

    Task<void> handle_subscribe(const nlohmann::json& json_msg);

    Task<void> handle_unsubscribe(const nlohmann::json& json_msg);

    Task<void> handle_prompts_list(const nlohmann::json& json_msg);

    Task<void> handle_prompts_get(const nlohmann::json& json_msg);

    Task<void> handle_set_level(const nlohmann::json& json_msg);

    Task<void> handle_complete(const nlohmann::json& json_msg);

    // [gcc11-sso: wire-builders] DO NOT convert to Task<T>.
    static std::string make_result_wire(const RequestId& id, nlohmann::json result);
    static std::string make_error_wire(const RequestId& id, int code, std::string message);

    Task<void> send_notification(const std::string& method,
                                 const std::optional<nlohmann::json>& params);

    TypeErasedHandler build_middleware_chain(TypeErasedHandler final_handler);

    struct PaginationSlice {
        std::size_t begin;
        std::size_t end;
        std::optional<std::string> next_cursor;
    };

    std::optional<PaginationSlice> paginate(std::size_t total, const nlohmann::json& json_msg);

    [[nodiscard]] bool has_tool_output_schema(const std::string& name) const;

    void reset_session();

    struct PendingRequest;
    struct Session;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mcp

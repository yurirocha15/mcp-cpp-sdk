#pragma once

// GCC 11 SSO Coroutine Safety — see docs/contributing.rst "Known Issues" for full details.
// Do NOT store std::string in a coroutine frame across co_await (GCC bugs #107288/#100611).
// Named patterns referenced at each call site below:
//   [no-named-temporaries] Pass params.get<In>() directly; don't store it in a named local.
//   [wire-builders]        make_result_wire/make_error_wire are synchronous helpers;
//                          do NOT convert them to Task<T> coroutines.

#include <boost/asio/any_io_executor.hpp>
#include <mcp/core/concepts.hpp>
#include <mcp/core/context.hpp>
#include <mcp/core/core.hpp>
#include <mcp/core/export.hpp>
#include <mcp/protocol/protocol.hpp>
#include <mcp/transport/transport.hpp>

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

enum class ToolResultMode : std::uint8_t {
    eNormalize,
    eValidated,
};

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
 * @brief Which checks a Server applies to tool calls (see Server::set_tool_validation()).
 *
 * Both protocol revisions require servers to validate tool inputs; turning a check off is for an
 * application that validates them itself.
 */
struct ToolValidationOptions {
    /// Check `tools/call` arguments against the tool's input schema. Arguments that do not conform
    /// are answered with an `isError` result listing the violations, and the handler does not run.
    bool validate_arguments = true;
    /// Check the `structuredContent` of a successful result against the tool's output schema. A
    /// result that does not conform is a fault in the tool, answered with `g_INTERNAL_ERROR`.
    bool validate_structured_content = true;
};

/**
 * @brief Tool calls a Server's validation has refused (see Server::tool_validation_stats()).
 */
struct ToolValidationStats {
    std::uint64_t argument_rejections = 0;  ///< Calls whose arguments did not conform.
    /// Calls whose result's `structuredContent` did not conform to the output schema.
    std::uint64_t structured_content_rejections = 0;
};

/**
 * @brief A result whose caching hints Server::set_cache_hints() sets.
 *
 * @details Protocol revision 2026-07-28 requires `ttlMs` and `cacheScope` on each of these results.
 */
enum class CacheTarget {
    eDiscover,               ///< server/discover.
    eToolsList,              ///< tools/list.
    ePromptsList,            ///< prompts/list.
    eResourcesList,          ///< resources/list.
    eResourceTemplatesList,  ///< resources/templates/list.
    eResourcesRead,          ///< resources/read.
};

/**
 * @brief The caching hints a server attaches to a cacheable result of a 2026-07-28 request.
 *
 * @details The defaults tell a client not to reuse the result (`ttlMs` 0) and, should it cache it
 * anyway, not to share it across authorization contexts (`private`).
 */
struct CacheHint {
    /// How long, in milliseconds, a client MAY consider the result fresh. 0 means do not reuse it.
    std::int64_t ttlMs = 0;
    /// Who may share a cached copy: only the same authorization context, or anyone.
    CacheScope scope = CacheScope::ePrivate;
};

/**
 * @brief Which protocol revisions a Server serves (see Server::set_protocol_policy()).
 *
 * @details A request that carries its protocol version in `_meta` (2026-07-28 and later) is
 * served on its own, without initialize. A client of 2025-11-25 or earlier starts a session with
 * initialize. By default a Server serves both, on the same transport.
 */
struct ServerProtocolPolicy {
    /// Serve requests that carry their protocol version in `_meta`. When false, every such
    /// request is answered with `g_UNSUPPORTED_PROTOCOL_VERSION`.
    bool serve_modern = true;
    /// Serve clients that start a session with initialize. When false, initialize is answered
    /// with `g_METHOD_NOT_FOUND` naming the versions served in its message and in
    /// `data.supported`, and any other request without a protocol version in `_meta` with
    /// `g_INVALID_PARAMS`.
    bool serve_legacy = true;
    /// The versions initialize may negotiate, each one of g_LEGACY_PROTOCOL_VERSIONS. Unset
    /// means all of them. An initialize asking for another version negotiates the newest one
    /// listed.
    std::optional<std::vector<std::string>> legacy_versions;

    /**
     * @brief Check that the policy can be applied.
     *
     * @throws std::invalid_argument If neither era is served, if legacy clients are served with
     *         an empty `legacy_versions`, or if `legacy_versions` names a version that is not one
     *         of g_LEGACY_PROTOCOL_VERSIONS.
     */
    void validate() const;
};

/**
 * @brief The version initialize negotiates under @p policy.
 *
 * @return @p requested when the policy lets initialize negotiate it, otherwise the newest legacy
 *         version the policy allows.
 */
MCP_API std::string negotiate_protocol_version(std::string_view requested,
                                               const ServerProtocolPolicy& policy);

/**
 * @brief The protocol versions served under @p policy, oldest first: the legacy versions
 * initialize may negotiate, then the versions served per request.
 */
MCP_API std::vector<std::string> served_protocol_versions(const ServerProtocolPolicy& policy);

/**
 * @brief Requests a Server refused for their protocol version or per-request protocol fields
 * (see Server::protocol_rejection_stats()).
 */
struct ProtocolRejectionStats {
    /// Requests whose required `_meta` fields were missing or malformed (`g_INVALID_PARAMS`).
    std::uint64_t invalid_metadata = 0;
    /// Requests naming a protocol version the server does not serve
    /// (`g_UNSUPPORTED_PROTOCOL_VERSION`).
    std::uint64_t unsupported_version = 0;
    /// Refusals of requests that name no protocol version in `_meta`: server/discover without
    /// one, and, when legacy clients are not served, initialize and every other such request. A
    /// legacy client turned away shows up here and nowhere else.
    std::uint64_t legacy_shaped = 0;
};

/**
 * @brief Per-call state for Server::dispatch_request_direct().
 */
struct RequestContext {
    /// Cancellation flag for this call: setting it cancels the request, and its handler sees it
    /// through Context::is_cancelled(). When null, the call gets a flag of its own.
    std::shared_ptr<std::atomic<bool>> cancelled;
    /// The `Mcp-Param-{Name}` headers of the HTTP request that carried this call, as
    /// {Name, value} pairs with the value as received. When set, a 2026-07-28 tools/call is
    /// answered with `g_HEADER_MISMATCH` unless each argument its tool mirrors into a header
    /// (`x-mcp-header`) that is present and not null has exactly one matching header, and every
    /// header the tool names has a valid value. Leave unset for a request that did not arrive
    /// over HTTP.
    std::optional<std::vector<std::pair<std::string, std::string>>> param_headers;
};

/**
 * @brief MCP server that handles client requests over a transport.
 *
 * Manages the JSON-RPC message loop, dispatching incoming requests to
 * registered handlers. Supports the MCP initialize handshake and shutdown
 * flow, as well as tool, resource, and prompt handler registration.
 * Creates a Context per request for handler use.
 */
class MCP_API Server {
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
     * @throws SchemaError when a schema is not a JSON Schema the SDK can use (see
     *         CompiledSchema), the input schema does not declare `"type": "object"`, or an
     *         `x-mcp-header` annotation breaks the rules of the 2026-07-28 tools specification
     *         (not a string, empty or not an HTTP token, repeated ignoring case, on a property
     *         that is not a string, integer or boolean, or on a property not reached through
     *         `properties` alone).
     * @throws std::invalid_argument when a tool with the same name is already registered.
     */
    template <JsonSerializable In, JsonSerializable Out, typename Fn>
    void add_tool(const std::string& name, const std::string& description,
                  const nlohmann::json& input_schema, Fn handler) {
        Tool tool;
        tool.name = name;
        tool.description = description;
        tool.inputSchema = input_schema;
        auto async_handler = detail::ensure_async_handler<In, Out>(std::move(handler));
        constexpr auto result_mode = std::same_as<Out, CallToolResult>
                                         ? detail::ToolResultMode::eValidated
                                         : detail::ToolResultMode::eNormalize;
        register_tool(std::move(tool), detail::wrap_handler<In, Out>(std::move(async_handler)),
                      result_mode);
    }

    /**
     * @brief Register a tool with an output schema for structured output.
     *
     * Like add_tool(), but also sets the tool's outputSchema so results include
     * a structuredContent field alongside content.
     *
     * @throws SchemaError when a schema is not a JSON Schema the SDK can use (see
     *         CompiledSchema), the input schema does not declare `"type": "object"`, or an
     *         `x-mcp-header` annotation breaks the rules of the 2026-07-28 tools specification
     *         (not a string, empty or not an HTTP token, repeated ignoring case, on a property
     *         that is not a string, integer or boolean, or on a property not reached through
     *         `properties` alone).
     * @throws std::invalid_argument when a tool with the same name is already registered.
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
        constexpr auto result_mode = std::same_as<Out, CallToolResult>
                                         ? detail::ToolResultMode::eValidated
                                         : detail::ToolResultMode::eNormalize;
        register_tool(std::move(tool), detail::wrap_handler<In, Out>(std::move(async_handler)),
                      result_mode);
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
     * @throws SchemaError when a schema is not a JSON Schema the SDK can use (see
     *         CompiledSchema), the input schema does not declare `"type": "object"`, or an
     *         `x-mcp-header` annotation breaks the rules of the 2026-07-28 tools specification
     *         (not a string, empty or not an HTTP token, repeated ignoring case, on a property
     *         that is not a string, integer or boolean, or on a property not reached through
     *         `properties` alone).
     * @throws std::invalid_argument when a tool with the same name is already registered.
     */
    void add_tool(const std::string& name, const std::string& description,
                  const nlohmann::json& input_schema,
                  std::function<nlohmann::json(const nlohmann::json&)> handler);

    /**
     * @brief Register a low-level tool whose handler returns a complete CallToolResult JSON object.
     *
     * Unlike add_tool(), this escape hatch never normalizes arbitrary output.
     * Every returned value is validated as a complete CallToolResult before it
     * is sent to the client.
     *
     * @param tool Tool metadata.
     * @param handler Asynchronous raw handler receiving tool arguments.
     * @throws SchemaError when a schema is not a JSON Schema the SDK can use (see
     *         CompiledSchema), the input schema does not declare `"type": "object"`, or an
     *         `x-mcp-header` annotation breaks the rules of the 2026-07-28 tools specification
     *         (not a string, empty or not an HTTP token, repeated ignoring case, on a property
     *         that is not a string, integer or boolean, or on a property not reached through
     *         `properties` alone).
     * @throws std::invalid_argument when a tool with the same name is already registered.
     */
    void add_raw_tool(const Tool& tool, TypeErasedHandler handler);

    /**
     * @brief Register a synchronous low-level tool returning complete CallToolResult JSON.
     *
     * @param tool Tool metadata.
     * @param handler Synchronous raw handler receiving tool arguments.
     * @throws SchemaError when a schema is not a JSON Schema the SDK can use (see
     *         CompiledSchema), the input schema does not declare `"type": "object"`, or an
     *         `x-mcp-header` annotation breaks the rules of the 2026-07-28 tools specification
     *         (not a string, empty or not an HTTP token, repeated ignoring case, on a property
     *         that is not a string, integer or boolean, or on a property not reached through
     *         `properties` alone).
     * @throws std::invalid_argument when a tool with the same name is already registered.
     */
    void add_raw_tool(const Tool& tool, std::function<nlohmann::json(const nlohmann::json&)> handler);

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
     * @brief Register a resource template and its resources/read handler.
     *
     * Exact resources registered with add_resource() take precedence over
     * template matches. Duplicate and structurally identical templates are
     * rejected, and a URI matching more than one template is reported as an
     * ambiguous request.
     *
     * @tparam In Handler input type, normally ReadResourceRequestParams.
     * @tparam Out Handler result type, normally ReadResourceResult.
     * @tparam Fn Handler callable (sync/async, with/without Context).
     * @param tmpl Resource template metadata.
     * @param handler Handler invoked when resources/read matches the template.
     */
    template <JsonSerializable In, JsonSerializable Out, typename Fn>
    void add_resource_template(const ResourceTemplate& tmpl, Fn handler) {
        auto async_handler = detail::ensure_async_handler<In, Out>(std::move(handler));
        register_resource_template(tmpl, detail::wrap_handler<In, Out>(std::move(async_handler)));
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
     * @brief Set which checks apply to tool calls.
     *
     * @details Read unsynchronized by request handlers; must be called before run().
     */
    void set_tool_validation(const ToolValidationOptions& options);

    /// Tool calls refused by validation since this server was created.
    [[nodiscard]] ToolValidationStats tool_validation_stats() const;

    /**
     * @brief Set the page size for paginated list responses.
     *
     * @param size The maximum number of items per page. 0 means no pagination.
     */
    void set_page_size(std::size_t size);

    /**
     * @brief Set the instructions string returned by initialize and server/discover.
     *
     * @details Read unsynchronized by request handlers; must be called before run().
     *
     * @param instructions Natural-language guidance for LLMs on how to use this server.
     */
    void set_instructions(std::string instructions);

    /**
     * @brief Set the `ttlMs` and `cacheScope` hints of one kind of result.
     *
     * @details Applies to results of 2026-07-28 requests; a result of an earlier revision carries
     * no hints. Until set, a target uses the CacheHint defaults. A handler's result that already
     * carries `ttlMs` or `cacheScope` keeps its own value. Read unsynchronized by request
     * handlers, so it must be called before the server serves its first request: before run()
     * or the first dispatch_request_direct().
     * With StreamableHttpSessionManager, configure the Server in the factory, before passing it
     * to set_modern_server(), or through modern_server() before listen().
     *
     * @param target The result the hints apply to.
     * @param hint The hints.
     * @throws std::invalid_argument If `hint.ttlMs` is negative.
     */
    void set_cache_hints(CacheTarget target, CacheHint hint);

    /**
     * @brief Choose whether results of 2026-07-28 requests name this server.
     *
     * @details When true (the default), every such result carries the server's name and version
     * under `_meta` (`io.modelcontextprotocol/serverInfo`), as the revision recommends. Results of
     * earlier revisions never do. Must be called before the server serves its first request:
     * before run() or the first dispatch_request_direct().
     * With StreamableHttpSessionManager, configure the Server in the factory, before passing it
     * to set_modern_server(), or through modern_server() before listen().
     */
    void set_report_server_info(bool report);

    /**
     * @brief Choose which protocol revisions this server serves.
     *
     * @details Takes effect for every transport. Must be called before the server serves its
     * first request.
     *
     * @param policy The revisions to serve.
     * @throws std::invalid_argument If ServerProtocolPolicy::validate() refuses the policy.
     * @throws std::logic_error If the server has already served a request or run() has started.
     */
    void set_protocol_policy(ServerProtocolPolicy policy);

    /// Requests refused for their protocol version or per-request protocol fields since this
    /// server was created.
    [[nodiscard]] ProtocolRejectionStats protocol_rejection_stats() const;

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
     * @brief Invoke a tool directly by name with the provided arguments.
     *
     * Bypasses the JSON-RPC message loop and calls the tool handler directly.
     * Intended for high-performance json_only mode where transport overhead
     * is unnecessary.
     *
     * @param tool_name The name of the tool to invoke.
     * @param args The arguments to pass to the tool.
     * @return A task that resolves to the tool result JSON.
     * @throws std::runtime_error If the tool is not found.
     */
    Task<nlohmann::json> invoke_tool(const std::string& tool_name, const nlohmann::json& args);

    /**
     * @brief Start the server session loop on the given transport.
     *
     * Runs until the transport is closed or an error occurs, then waits for
     * request handlers already dispatched by this session to finish. The
     * returned task keeps the server's state alive, so the Server object may be
     * destroyed before it completes; a reverse request that a handler issues
     * after that throws std::runtime_error.
     *
     * The session enforces the initialization sequence. When @p transport is an
     * HttpServerTransport, a session the client ends with HTTP DELETE is over and
     * the next client must initialize again; a transport that wraps an
     * HttpServerTransport is not recognized as one.
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
     * The transport keeps its default settings, so a request carrying an Origin
     * header is answered with 403. To accept browser clients, construct an
     * HttpServerTransport, call set_allowed_origins() on it, and pass it to run().
     *
     * @param host  Local address to bind (e.g. "0.0.0.0" or "127.0.0.1").
     * @param port  TCP port to listen on.
     */
    void run_http(const std::string& host, uint16_t port);

    /**
     * @brief Dispatch a parsed JSON-RPC message to the appropriate handler.
     *
     * @param json_msg The parsed JSON-RPC message.
     *
     * The Server object must outlive the returned task.
     */
    Task<void> dispatch(nlohmann::json json_msg);

    /**
     * @brief Dispatch a parsed JSON-RPC request and return its serialized response.
     *
     * Used by stateless HTTP JSON mode to avoid creating a transport-backed
     * server session for request/response messages. The request envelope is
     * validated, but no stateful initialize/initialized handshake is required.
     * A request that carries its protocol version in `_meta` is served as a 2026-07-28
     * request: its version and per-request fields are checked.
     *
     * @details Request parameter decoding failures are returned as `g_INVALID_PARAMS` (-32602).
     * Exceptions thrown by middleware, or by resource, prompt and completion handlers, are
     * returned as `g_INTERNAL_ERROR` (-32603); a tool handler's exception becomes a tool error
     * result (`isError: true`).
     *
     * @param json_msg The parsed JSON-RPC request object.
     * @return A task that resolves to the serialized JSON-RPC response.
     *
     * The Server object must outlive the returned task.
     */
    Task<std::string> dispatch_request_direct(nlohmann::json json_msg);

    /**
     * @brief Dispatch a parsed JSON-RPC request with per-call state and return its serialized
     * response.
     *
     * @details As the one-argument overload. A `tools/call` is cancelled through
     * `request_context.cancelled`, never through notifications/cancelled, so concurrent calls
     * that reuse a request id stay independent.
     *
     * @param json_msg The parsed JSON-RPC request object.
     * @param request_context Per-call state.
     * @return A task that resolves to the serialized JSON-RPC response.
     *
     * The Server object must outlive the returned task.
     */
    Task<std::string> dispatch_request_direct(nlohmann::json json_msg, RequestContext request_context);

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
     *
     * The Server object must outlive the returned task.
     */
    Task<void> notify_tools_list_changed();

    /**
     * @brief Send a notifications/resources/list_changed notification to the client.
     *
     * The Server object must outlive the returned task.
     */
    Task<void> notify_resources_list_changed();

    /**
     * @brief Send a notifications/prompts/list_changed notification to the client.
     *
     * The Server object must outlive the returned task.
     */
    Task<void> notify_prompts_list_changed();

    /**
     * @brief Send a notifications/resources/updated notification to all subscribers.
     *
     * @param uri The URI of the resource that was updated.
     *
     * The Server object must outlive the returned task.
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
     * @return The log level used when filtering context log messages of requests of 2025-11-25
     * and earlier. A 2026-07-28 request is filtered by its own io.modelcontextprotocol/logLevel.
     */
    [[nodiscard]] LoggingLevel get_log_level() const;

   private:
    /// Non-template registration helpers called by the template add_* methods above.
    void register_tool(const Tool& tool, const std::string& name, TypeErasedHandler handler,
                       detail::ToolResultMode result_mode);
    void register_tool(Tool&& tool, TypeErasedHandler handler, detail::ToolResultMode result_mode);
    void register_resource(const Resource& resource, TypeErasedHandler handler);
    void register_resource_template(const ResourceTemplate& tmpl, TypeErasedHandler handler);
    void register_prompt(const Prompt& prompt, TypeErasedHandler handler);

    TypeErasedHandler build_middleware_chain(TypeErasedHandler final_handler);

    struct PaginationSlice {
        std::size_t begin;
        std::size_t end;
        std::optional<std::string> next_cursor;
    };

    struct PendingRequest;
    struct Session;

    /// Tears down the current session. Runs on whatever thread destroys the Server, so it is safe
    /// to call from any thread.
    void reset_session();

    struct Impl;
    /// Shared rather than owned outright: work already in flight when this Server is destroyed
    /// keeps the implementation alive until it finishes, so it never runs against freed state.
    std::shared_ptr<Impl> impl_;
};

}  // namespace mcp

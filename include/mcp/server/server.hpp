#pragma once

#include <mcp/core/async_fwd.hpp>
#include <mcp/core/concepts.hpp>
#include <mcp/core/constants.hpp>
#include <mcp/core/context.hpp>
#include <mcp/core/executor.hpp>
#include <mcp/core/export.hpp>
#include <mcp/core/notification_rate_limit.hpp>
#include <mcp/protocol/protocol.hpp>
#include <mcp/server/cache_hint.hpp>
#include <mcp/server/handler.hpp>
#include <mcp/server/http_run_options.hpp>
#include <mcp/server/middleware.hpp>
#include <mcp/server/mrtr_options.hpp>
#include <mcp/server/protocol_policy.hpp>
#include <mcp/server/sync_server.hpp>
#include <mcp/server/task_options.hpp>
#include <mcp/server/task_store.hpp>
#include <mcp/server/tool_params.hpp>
#include <mcp/server/tool_validation.hpp>
#include <mcp/transport/request_context.hpp>
#include <mcp/transport/transport.hpp>

#include <atomic>
#include <chrono>
#include <concepts>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <iosfwd>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace mcp {

class StreamableHttpSessionManager;

namespace detail {
class RunControl;
}  // namespace detail

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
     * @brief Construct a Server named @p name at version @p version, such as
     *        `Server server("hello", "1.0.0");`.
     *
     * @details The server advertises what its registrations imply: tools once a tool is
     * registered, resources for a resource or resource template, prompts for a prompt and
     * completions for a completion provider. capabilities() adjusts what registration cannot
     * imply, such as list-changed notifications, resource subscriptions or logging.
     */
    template <typename Name, typename Version>
        requires(std::convertible_to<const Name&, std::string_view> &&
                 std::convertible_to<const Version&, std::string_view>)
    Server(const Name& name, const Version& version)
        : Server(named(name, version), ServerCapabilities{}) {}

    /**
     * @brief Construct a Server.
     *
     * @param server_info Information about this server implementation.
     * @param capabilities The capabilities this server advertises; each registration adds the
     *        capability of its kind when @p capabilities does not set it, as the constructor above
     *        describes.
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
     * @throws std::invalid_argument when a tool with the same name is already registered, or when
     * the name is empty, longer than 128 characters or holds a character other than A-Z, a-z,
     * 0-9, `_`, `-` and `.`.
     */
    template <JsonSerializable In, JsonSerializable Out, typename Fn>
    void add_tool(const std::string& name, const std::string& description,
                  const nlohmann::json& input_schema, Fn handler) {
        add_typed_tool<In, Out>(name, description, input_schema, nullptr, std::move(handler));
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
     * @throws std::invalid_argument when a tool with the same name is already registered, or when
     * the name is empty, longer than 128 characters or holds a character other than A-Z, a-z,
     * 0-9, `_`, `-` and `.`.
     */
    template <JsonSerializable In, JsonSerializable Out, typename Fn>
    void add_tool(const std::string& name, const std::string& description,
                  const nlohmann::json& input_schema, const nlohmann::json& output_schema, Fn handler) {
        add_typed_tool<In, Out>(name, description, input_schema, &output_schema, std::move(handler));
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
     * @throws std::invalid_argument when a tool with the same name is already registered, or when
     * the name is empty, longer than 128 characters or holds a character other than A-Z, a-z,
     * 0-9, `_`, `-` and `.`.
     */
    void add_tool(const std::string& name, const std::string& description,
                  const nlohmann::json& input_schema,
                  std::function<nlohmann::json(const nlohmann::json&)> handler);

    /// @brief As add_tool() above, with a synchronous handler that takes the request's Context
    /// first, such as for Context::is_cancelled() or, in a task, Context::report_progress_sync().
    void add_tool(const std::string& name, const std::string& description,
                  const nlohmann::json& input_schema,
                  std::function<nlohmann::json(Context&, const nlohmann::json&)> handler);

    /**
     * @brief As add_tool() above, with a coroutine handler, such as
     *        `[](mcp::Context& ctx, const nlohmann::json& args) -> mcp::Task<nlohmann::json>`.
     *
     * @details The handler takes the JSON arguments, after the request's Context if it wants it.
     * A coroutine needs <mcp/core/async.hpp>.
     */
    template <typename Fn>
        requires detail::JsonAsyncHandler<Fn>
    void add_tool(const std::string& name, const std::string& description,
                  const nlohmann::json& input_schema, Fn handler) {
        Tool tool;
        tool.name = name;
        tool.description = description;
        tool.inputSchema = input_schema;
        add_tool(tool, std::move(handler));
    }

    /**
     * @brief Register the tool @p tool describes, with its title, annotations, icons, output
     *        schema and `_meta` (such as an Apps link) as given.
     *
     * @details The handler takes the JSON arguments and returns a JSON value. A string becomes the
     * result's text. An object that is not already a CallToolResult becomes its
     * `structuredContent`, with the object's JSON text as its text, so a tool with an output schema
     * returns an object. Any other value is sent as its JSON text.
     * @throws SchemaError and std::invalid_argument as add_tool() above.
     */
    void add_tool(const Tool& tool, std::function<nlohmann::json(const nlohmann::json&)> handler);
    /// @brief As add_tool() above, with a synchronous handler that takes the request's Context
    /// first.
    void add_tool(const Tool& tool,
                  std::function<nlohmann::json(Context&, const nlohmann::json&)> handler);
    /// @brief As add_tool() above, with a coroutine handler, with or without the Context.
    template <typename Fn>
        requires detail::JsonAsyncHandler<Fn>
    void add_tool(const Tool& tool, Fn handler) {
        register_tool(tool, tool.name, detail::erase_json_handler(std::move(handler)),
                      detail::ToolResultMode::eNormalize);
    }

    /**
     * @brief Register a low-level tool whose handler returns a complete CallToolResult JSON object.
     *
     * Unlike add_tool(), this escape hatch never normalizes arbitrary output.
     * Every returned value is validated as a complete CallToolResult before it
     * is sent to the client.
     *
     * @param tool Tool metadata.
     * @param handler Coroutine raw handler receiving tool arguments, after the request's Context
     *        if it wants it, such as a TypeErasedHandler.
     * @throws SchemaError when a schema is not a JSON Schema the SDK can use (see
     *         CompiledSchema), the input schema does not declare `"type": "object"`, or an
     *         `x-mcp-header` annotation breaks the rules of the 2026-07-28 tools specification
     *         (not a string, empty or not an HTTP token, repeated ignoring case, on a property
     *         that is not a string, integer or boolean, or on a property not reached through
     *         `properties` alone).
     * @throws std::invalid_argument when a tool with the same name is already registered, or when
     * the name is empty, longer than 128 characters or holds a character other than A-Z, a-z,
     * 0-9, `_`, `-` and `.`.
     */
    template <typename Fn>
        requires detail::JsonAsyncHandler<Fn>
    void add_raw_tool(const Tool& tool, Fn handler) {
        register_tool(tool, tool.name, detail::erase_json_handler(std::move(handler)),
                      detail::ToolResultMode::eValidated);
    }

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
     * @throws std::invalid_argument when a tool with the same name is already registered, or when
     * the name is empty, longer than 128 characters or holds a character other than A-Z, a-z,
     * 0-9, `_`, `-` and `.`.
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
        static_assert(detail::SyncHandler<Fn, In, Out> || detail::AsyncHandler<Fn, In, Out>,
                      "Handler must be sync or async with supported signature; a coroutine handler "
                      "returns mcp::Task<Out>, from <mcp/core/async.hpp>");
        if constexpr (detail::SyncHandler<Fn, In, Out>) {
            register_sync_resource(resource, detail::wrap_sync_handler<In, Out>(std::move(handler)));
        } else {
            register_resource(resource, detail::wrap_handler<In, Out>(std::move(handler)));
        }
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
        static_assert(detail::SyncHandler<Fn, In, Out> || detail::AsyncHandler<Fn, In, Out>,
                      "Handler must be sync or async with supported signature; a coroutine handler "
                      "returns mcp::Task<Out>, from <mcp/core/async.hpp>");
        if constexpr (detail::SyncHandler<Fn, In, Out>) {
            register_sync_resource_template(tmpl,
                                            detail::wrap_sync_handler<In, Out>(std::move(handler)));
        } else {
            register_resource_template(tmpl, detail::wrap_handler<In, Out>(std::move(handler)));
        }
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
        static_assert(detail::SyncHandler<Fn, In, Out> || detail::AsyncHandler<Fn, In, Out>,
                      "Handler must be sync or async with supported signature; a coroutine handler "
                      "returns mcp::Task<Out>, from <mcp/core/async.hpp>");
        if constexpr (detail::SyncHandler<Fn, In, Out>) {
            register_sync_prompt(prompt, detail::wrap_sync_handler<In, Out>(std::move(handler)));
        } else {
            register_prompt(prompt, detail::wrap_handler<In, Out>(std::move(handler)));
        }
    }

    /**
     * @brief Register a text resource at @p uri named @p name, whose text @p read returns on
     *        each read, such as
     *        `server.add_text_resource("config://motd", "Message of the day", [] { return
     * std::string("hi"); });`.
     *
     * @param mime_type The MIME type the resource and its contents report.
     * @throws std::invalid_argument When a resource with the same URI is already registered.
     */
    void add_text_resource(const std::string& uri, const std::string& name,
                           std::function<std::string()> read,
                           const std::string& mime_type = "text/plain");
    /// @brief As add_text_resource() above, for the resource @p resource describes, with its
    /// title, description, annotations, icons and size as given and its MIME type text/plain when
    /// unset.
    void add_text_resource(const Resource& resource, std::function<std::string()> read);
    /// @brief As add_text_resource() above, with a coroutine that returns the text.
    template <typename Fn>
        requires detail::TextAsyncHandler<Fn>
    void add_text_resource(const std::string& uri, const std::string& name, Fn read,
                           const std::string& mime_type = "text/plain") {
        Resource resource;
        resource.uri = uri;
        resource.name = name;
        resource.mimeType = mime_type;
        add_text_resource(resource, std::move(read));
    }
    /// @brief As add_text_resource() above, with a coroutine that returns the text.
    template <typename Fn>
        requires detail::TextAsyncHandler<Fn>
    void add_text_resource(const Resource& resource, Fn read) {
        register_text_resource(resource, std::function<Task<std::string>()>(std::move(read)));
    }

    /// @brief The arguments of a prompts/get request, by name.
    using PromptArguments = std::map<std::string, std::string>;

    /**
     * @brief Register a prompt named @p name whose one message, from the user, is the text
     *        @p render makes of the request's arguments, such as
     *        `server.add_text_prompt("review", "Review code", {{"code", "The code", true}}, render);`.
     *
     * @details @p render gets the arguments the request gave, an empty map when it gave none. A
     * request that leaves out an argument @p arguments marks as required is answered with
     * mcp::constants::jsonrpc::g_INVALID_PARAMS, and @p render does not run.
     * @throws std::invalid_argument When a prompt with the same name is already registered.
     */
    void add_text_prompt(const std::string& name, const std::string& description,
                         std::vector<PromptArgument> arguments,
                         std::function<std::string(const PromptArguments&)> render);
    /// @brief As add_text_prompt() above, for the prompt @p prompt describes, with its title,
    /// icons and arguments as given.
    void add_text_prompt(const Prompt& prompt,
                         std::function<std::string(const PromptArguments&)> render);
    /// @brief As add_text_prompt() above, with a coroutine that makes the text.
    template <typename Fn>
        requires detail::TextAsyncHandler<Fn, const PromptArguments&>
    void add_text_prompt(const std::string& name, const std::string& description,
                         std::vector<PromptArgument> arguments, Fn render) {
        Prompt prompt;
        prompt.name = name;
        prompt.description = description;
        prompt.arguments = std::move(arguments);
        add_text_prompt(prompt, std::move(render));
    }
    /// @brief As add_text_prompt() above, with a coroutine that makes the text.
    template <typename Fn>
        requires detail::TextAsyncHandler<Fn, const PromptArguments&>
    void add_text_prompt(const Prompt& prompt, Fn render) {
        register_text_prompt(
            prompt, std::function<Task<std::string>(const PromptArguments&)>(std::move(render)));
    }

    /**
     * @brief Register a middleware function to intercept handler invocations.
     *
     * Middleware executes in registration order (first registered = outermost).
     * Each middleware receives the context, request parameters, and a
     * continuation to call the next in the chain or the final handler. It runs for every request
     * that reaches an application handler: tools/call, resources/read, prompts/get,
     * resources/subscribe, resources/unsubscribe and completion/complete. Requests the server
     * answers itself, such as the list methods, ping and logging/setLevel, do not pass through it.
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

    /**
     * @brief Limit the progress and log notifications each request may send.
     *
     * @details Each request gets its own buckets of @p limit, one for Context::report_progress()
     * and one for Context::log(); a notification over the limit is dropped and counted in
     * dropped_notifications(). By default there is no limit. Read unsynchronized by request
     * handlers; must be called before run().
     * @throws std::invalid_argument If `per_second` is negative or not a number.
     */
    void set_notification_rate_limit(const NotificationRateLimit& limit);

    /// Progress and log notifications dropped by set_notification_rate_limit() since this server
    /// was created.
    [[nodiscard]] std::uint64_t dropped_notifications() const;

    /**
     * @brief Run tool, resource, prompt and extension method handlers on @p executor instead of
     *        the executor the server runs on.
     *
     * @details The server's own work (reading, parsing and answering protocol messages) stays on
     * the executor passed to run(), and so do middleware and the completion provider; only the
     * call of one of those handlers moves to @p executor, and the server resumes on its own
     * executor when it returns. A tool call made as a task runs on @p executor whole, its
     * middleware included. A handler that blocks then ties up a thread of @p executor rather than
     * one the transport needs. Read unsynchronized by request handlers; must be called before
     * run().
     *
     * The execution context of @p executor must be destroyed after the one the server runs on: a
     * program that runs its own event loop constructs the Runtime that gives @p executor before
     * that loop's io_context. Destroying the server's execution context waits for the handlers on
     * @p executor that its requests still wait for; from the start of that destruction their
     * Context calls throw std::runtime_error, and what they return goes nowhere. run_stdio() and
     * run_http() return at the shutdown deadline all the same (see stop()), and destroy their
     * event loop on a thread of its own once those handlers have returned.
     * @throws std::invalid_argument If @p executor holds none.
     */
    void set_handler_executor(const Executor& executor);

    /**
     * @brief Declare support for an extension in the capabilities this server advertises.
     *
     * @details Adds `extensions[id]` to the capabilities that initialize and server/discover
     * return, replacing an earlier declaration of the same extension. Extensions are off until a
     * server declares them. Must be called before the server serves its first request.
     *
     * @param id Extension identifier, `{vendor-prefix}/{extension-name}`.
     * @param settings The extension's settings object; an empty object declares support with no
     *        settings.
     * @throws std::invalid_argument If @p id is empty or @p settings is not an object.
     */
    void declare_extension(const std::string& id, nlohmann::json settings = nlohmann::json::object());

    /**
     * @brief The capabilities this server advertises: those it was constructed with, what its
     *        registrations imply and the declared extensions.
     *
     * @details Change them before the server serves its first request, such as
     * `server.capabilities().tools->listChanged = true;` after registering a tool, or
     * `server.capabilities().logging = nlohmann::json::object();`. A registration does not
     * replace a capability that is already set.
     */
    [[nodiscard]] ServerCapabilities& capabilities();
    /// @copydoc capabilities()
    [[nodiscard]] const ServerCapabilities& capabilities() const;

    /**
     * @brief Serve an extension's request method to clients of protocol revision 2026-07-28.
     *
     * @details The handler receives the request's `params` (including `_meta`) and returns the
     * result object; the server adds `resultType` and the members every 2026-07-28 result carries.
     * A result that sets `ttlMs` and `cacheScope` keeps them. The handler reports invalid
     * parameters by throwing std::invalid_argument, which the client receives as -32602, and any
     * other exception as an internal error. Middleware registered with use() wraps these handlers
     * as it wraps `resources/read`, so a request it refuses never reaches the handler. Earlier
     * revisions answer the method with -32601. Must be called before the server serves its first
     * request.
     *
     * @param method The method name, for example `skills/list`.
     * @param handler Handler invoked for each request of that method.
     * @throws std::invalid_argument If @p method is empty, names a method the SDK serves itself, or
     *         is already registered.
     */
    void add_extension_method(const std::string& method, TypeErasedHandler handler);

    /// Tool calls refused by validation since this server was created.
    [[nodiscard]] ToolValidationStats tool_validation_stats() const;

    /**
     * @brief Set the page size for paginated list responses.
     *
     * @details A list request whose cursor is not one a `nextCursor` of this server could have
     * held, including one past the end of a list that has since become shorter, is answered with
     * -32602 (Invalid params).
     *
     * @param size The maximum number of items per page. 0 means no pagination.
     */
    void set_page_size(std::size_t size);

    /**
     * @brief Set how many requests of one session may run at once.
     *
     * @details A request past the limit is answered with an error instead of being queued, so a
     * peer that sends requests faster than they finish cannot grow the process without bound. The
     * same answer goes to a request past the memory limit, set_max_in_flight_bytes(). Requests
     * given to dispatch_request_direct(), such as 2026-07-28 requests, belong to no session and
     * share one such limit; each counts until its handler ends, even when its client has gone.
     * A handler waiting for client input is bounded by MrtrOptions instead, and counts here again
     * when it goes on after being given up.
     *
     * @param limit The limit, at least 1. The default is 1024.
     * @throws std::invalid_argument If @p limit is 0.
     */
    void set_max_in_flight_requests(std::size_t limit);

    /**
     * @brief Set how much memory the requests of one session that are running, and their answers
     * that are not written yet, may take.
     *
     * @details A request is counted as its parsed JSON takes memory, which can be far more than its
     * text: an array of empty objects takes about 27 times its length. An answer is counted by its
     * length until the peer has taken it, so a peer that stops reading stops the session from
     * reading more requests. A request that would go past the limit is answered with an error, as
     * one past set_max_in_flight_requests() is. The handlers' own copies of a request are not
     * counted. Requests given to dispatch_request_direct() share one such limit.
     *
     * @param bytes The limit, at least 1. The default is 256 MiB.
     * @throws std::invalid_argument If @p bytes is 0.
     */
    void set_max_in_flight_bytes(std::size_t bytes);

    /**
     * @brief Set how long a request the server sends the client may wait for its answer.
     *
     * @details The request then fails in the handler that made it, as a client that never answers
     * would otherwise hold that handler for as long as the session lasts.
     *
     * @param timeout The timeout, longer than zero. The default is ten minutes.
     * @throws std::invalid_argument If @p timeout is not positive.
     */
    void set_reverse_request_timeout(std::chrono::milliseconds timeout);

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
     * first request; afterwards only the policy in effect may be set again, as run_http() does
     * when its configure sets the same policy on every run.
     *
     * @param policy The revisions to serve.
     * @throws std::invalid_argument If ServerProtocolPolicy::validate() refuses the policy.
     * @throws std::logic_error If the server has already served a request or run() has started,
     *         and @p policy differs from the policy in effect.
     */
    void set_protocol_policy(ServerProtocolPolicy policy);

    /// Requests refused for their protocol version or per-request protocol fields since this
    /// server was created.
    [[nodiscard]] ProtocolRejectionStats protocol_rejection_stats() const;

    /**
     * @brief Set the limits on 2026-07-28 requests that wait for client input.
     *
     * @details With MrtrStateMode::eStored (the default) the waiting handlers live in this server,
     * so a client's retry must reach the same server instance (and process) as the request it
     * retries; one that does not is answered as for an unknown requestState. When run() ends, its
     * transport can bring no more retries, and the waiting handlers are given up. Takes effect for
     * requests parked afterwards.
     *
     * With MrtrStateMode::eSealed nothing waits in the server: `requestState` carries the state,
     * sealed with MrtrOptions::sealing_keys, and a retry may reach any server instance that has
     * the key. The instance runs the handler again from the start and the questions earlier
     * rounds answered return their answers at once (see Context::is_replay()), so a handler must
     * ask the same questions in the same order when given the same answers. Progress and log
     * notifications a handler sends before the question it asks again are sent again. A retry
     * is refused as an unknown requestState when its state is expired, was sealed by another key,
     * or belongs to another principal, method or arguments.
     *
     * A sealed state is not single-use: until it expires the client (or anyone holding the state)
     * can send it again, and each time the handler runs again from the start with the same
     * answers. The steps after the handler's last question then run once more, so make them
     * idempotent or set MrtrOptions::consume to refuse a state it has seen. A handler is released
     * at its first unanswered question without running any more of its code (its locals are
     * destroyed), so no `catch` of the handler sees the release. The expiry is a wall-clock time;
     * instances that share keys need reasonably synchronized clocks. Context::is_replay() is true only
     * while earlier answers remain to be replayed.
     *
     * The secrets in @p options are overwritten once the key ring is built and the server keeps
     * none of them.
     *
     * @param options The limits.
     * @throws std::invalid_argument when sealed mode has no key, a key is shorter than 32 bytes,
     *         two keys are the same secret or share a 4-byte key id, `max_rounds` is 0,
     *         `state_ttl` is zero or negative, or `max_state_bytes` is below 512.
     */
    void set_mrtr_options(MrtrOptions options);

    /// Counters of the 2026-07-28 requests that wait, or waited, for client input.
    [[nodiscard]] MrtrStats mrtr_stats() const;

    /**
     * @brief Serve the tasks extension (`io.modelcontextprotocol/tasks`) of protocol revision
     * 2026-07-28.
     *
     * @details A tool whose mode is not ToolTaskMode::eNever answers a client that declared the
     * extension with a task handle at once, runs the tool apart from the request, and keeps its
     * outcome in the task store for the client to fetch with `tasks/get`. The server then also
     * answers `tasks/get`, `tasks/update` and `tasks/cancel`, advertises the extension in
     * `server/discover`, and sends `notifications/tasks` to a `subscriptions/listen` stream that
     * asks for a task's id. Clients of earlier protocol revisions never see tasks.
     *
     * The task's handler runs on the executor set with set_handler_executor(), or else on the
     * executor of the request. A handler that blocks stalls whatever else runs there, so a
     * server that serves task tools needs a handler executor with threads of its own. Inside a
     * task, Context::report_progress() sets the task's status message, and
     * Context::elicit(), sample_llm() and request_roots() put the question in the task's
     * `inputRequests` and wait until the client answers with `tasks/update`.
     *
     * A task is bound to the principal (Context::principal()) that started it, and only that
     * principal can fetch, answer or cancel it. When the transport authenticates no principal,
     * every caller shares one anonymous scope, and the task id, which is random and not
     * guessable, is all that protects a task: serve tasks to anonymous callers only when that is
     * acceptable.
     *
     * Tasks last as long as their time to live, and with the default store only as long as the
     * process: a task does not survive a restart, and the store's other limits apply. The handler
     * of a task whose time to live passes is asked to stop, like one whose task is cancelled
     * (Context::is_cancelled()), but a handler that ignores that keeps running, and keeps
     * counting against TaskOptions::max_tasks_per_principal and TaskOptions::max_tasks until it
     * returns.
     *
     * @param options The store and the limits.
     * @throws std::logic_error If the server already serves requests.
     */
    void enable_tasks(TaskOptions options = {});

    /**
     * @brief Set whether calls of tool @p name may become tasks.
     *
     * @details A tool's mode is ToolTaskMode::eNever until set. The mode may be set before
     * enable_tasks(); it takes effect once tasks are enabled.
     *
     * @throws std::logic_error If the server already serves requests.
     * @throws std::invalid_argument If no tool is registered under @p name.
     */
    void set_tool_task_mode(const std::string& name, ToolTaskMode mode);

    /**
     * @brief Let tool @p name ask the client for input before its task is created.
     *
     * @details The preparation runs only for a tools/call that would create a task: tasks are
     * enabled, the tool's mode is not ToolTaskMode::eNever and the client declared the tasks
     * extension. It runs inside that request with the Context a synchronous handler of the tool
     * would get, so it may call Context::elicit(), Context::sample_llm() and
     * Context::request_roots(): the client answers them through the multi round-trip flow of the
     * request, and the call returns a task only once the preparation has returned. The
     * preparation receives the tool's arguments. When it returns TaskPreparation::result the call
     * is answered with that result and no task is created; otherwise the task is created and its
     * handler is called with TaskPreparation::arguments. A preparation that throws answers the
     * call the way a handler that throws does.
     *
     * Middleware sees the call twice: once around the preparation, where it may reject the call,
     * rewrite the parameters or answer without calling the next handler (no task is created then),
     * and later around the task's handler. When the preparation returns arguments, middleware
     * around it sees an empty CallToolResult as the answer, since no result exists yet.
     *
     * The task limits of TaskOptions are checked before the preparation runs, so a principal at its
     * limit is answered without being asked anything, and again when the task is created. A
     * request cancelled while the preparation runs creates no task.
     *
     * @throws std::logic_error If the server already serves requests.
     * @throws std::invalid_argument If no tool is registered under @p name.
     */
    void set_tool_task_preparation(const std::string& name, ToolTaskPreparation preparation);

    /**
     * @brief Give up every handler waiting for the retry of a 2026-07-28 request.
     *
     * @details Each stops at its question and its frame unwinds (a stored handler throws from the
     * question, a sealed handler is destroyed without running more of its code), and its answer is
     * discarded. For when no retry can arrive any more: run() does this when it ends, and
     * StreamableHttpSessionManager::close() for its modern server.
     */
    void abandon_waiting_handlers();

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
     * HttpServerTransport, or wraps one and returns it from
     * ITransport::http_server_transport(), a session the client ends with HTTP DELETE
     * is over and the next client must initialize again, the `Mcp-Param-*` headers of
     * 2026-07-28 tools/call requests are checked, and notifications of 2026-07-28
     * requests are dropped. A wrapper that does not return it is served like any other
     * transport.
     *
     * @param transport The transport to use for message exchange. Ownership is shared.
     * @param executor  The executor to use for async operations.
     */
    Task<void> run(std::shared_ptr<ITransport> transport, const Executor& executor);

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
     * @p input and @p output are used until the run's handlers have finished: when the shutdown
     * deadline ends the run while a handler on the set_handler_executor() executor still runs,
     * that is after this returns.
     *
     * @param input  Stream to read JSON-RPC messages from.
     * @param output Stream to write JSON-RPC responses to.
     */
    void run_stdio(std::istream& input, std::ostream& output);

    /**
     * @brief Run the server over HTTP, blocking until shutdown.
     *
     * Binds to the given host and port, accepts HTTP connections, and
     * processes JSON-RPC requests from any number of clients. Handles SIGINT and SIGTERM for
     * clean shutdown. Exceptions from listening are propagated to the caller.
     *
     * A client of protocol revision 2025-11-25 or earlier gets a session of its own
     * (`MCP-Session-Id`), served with this server's tools, resources, prompts, handlers and
     * settings; notify_tools_list_changed() and the other notify_ calls reach every session. The
     * members of this server that belong to one session, send_request(), is_initialized(),
     * is_shutdown_requested() and get_log_level(), stand for none of them: a handler reaches its
     * client through its Context. A
     * 2026-07-28 request belongs to no session and is answered by this server with plain JSON,
     * or, when its handler sends a notification and the client accepts `text/event-stream`, with
     * an SSE stream of the notifications and then the response. `subscriptions/listen` is served
     * as a stream.
     *
     * The server listens through a StreamableHttpSessionManager, which @p configure, when given,
     * receives before it listens: its setters choose the allowed origins, bearer token validation,
     * protected resource metadata, limits and stream options, such as
     * `server.run_http("127.0.0.1", 8080, [](mcp::StreamableHttpSessionManager& http) {
     * http.set_allowed_origins({"https://app.example"}); });`. A program that passes @p configure
     * includes `<mcp/transport/http_session_manager.hpp>`, which declares the manager. Without
     * @p configure the endpoint keeps its default settings, so a request carrying an Origin
     * header is answered with 403.
     *
     * stop() ends the call as SIGINT does, and bound_port() gives the port it listens on. A
     * shutdown is graceful: the requests being served when it begins are answered, within
     * mcp::constants::shutdown::g_TIMEOUT_MS, before the connections close. The overload taking
     * HttpRunOptions describes the steps and lets a program take part in them.
     *
     * @param host  Local address to bind (e.g. "0.0.0.0" or "127.0.0.1").
     * @param port  TCP port to listen on, or 0 for one the system chooses.
     * @param configure Called with the session manager before it listens; an exception it throws
     *        ends run_http() before anything is served.
     */
    void run_http(const std::string& host, uint16_t port,
                  const std::function<void(StreamableHttpSessionManager&)>& configure = {});

    /**
     * @brief Run the server over HTTP as run_http() above does, with @p options, blocking until
     *        shutdown.
     *
     * @details The options set the number of event loop threads, run code on the event loop
     * before the server listens and while it shuts down, and bound the shutdown; see
     * HttpRunOptions for the steps. Exceptions from listening, and from HttpRunOptions::on_start,
     * are propagated to the caller once the shutdown has run.
     *
     * @param host  Local address to bind (e.g. "0.0.0.0" or "127.0.0.1").
     * @param port  TCP port to listen on, or 0 for one the system chooses.
     * @param options How to run and shut down.
     * @throws std::invalid_argument When `options.io_threads` is zero, a timeout is negative,
     *         `options.drain_timeout` is longer than `options.shutdown_timeout`, or the two are
     *         equal while `options.on_shutdown` is set; nothing runs.
     */
    void run_http(const std::string& host, uint16_t port, const HttpRunOptions& options);

    /**
     * @brief Ends the runs of this server in progress: run_stdio() and run_http() return as they
     *        do on SIGINT, run() closes its transport and completes as it does when the transport
     *        closes, and a StreamableHttpSessionManager constructed with this server closes.
     *
     * @details May be called from any thread, including from a handler. As on SIGINT, run_stdio()
     * closes its transport at once, so a request still being served is not answered, and run_stdio()
     * returns once the handlers still running have finished, or after
     * mcp::constants::shutdown::g_TIMEOUT_MS. run_http() drains first (see HttpRunOptions): the
     * requests being served are answered, and it returns once the handlers still running have
     * finished, or at HttpRunOptions::shutdown_timeout. A synchronous handler that runs on the server's
     * own loop holds that loop, and with it the deadline, until it returns; one on the
     * set_handler_executor() executor is left running. The handler of a tool call made as a task
     * is not waited for either: it goes on, and a question it asks the client afterwards is never
     * answered. Does nothing when the server is not running: a run that starts after the call is
     * not ended by it.
     */
    void stop();

    /**
     * @brief The TCP port run_http() listens on, which is the one the system chose when it was
     *        given port 0.
     *
     * @return The port while run_http() runs and from before the first connection is accepted,
     *         and 0 before and after; may be called from any thread.
     */
    [[nodiscard]] std::uint16_t bound_port() const;

    /**
     * @brief A view of this server whose members block the calling thread until the matching
     *        coroutine finishes, for threads other than the event loop of run_stdio() and
     *        run_http(), such as `server.sync().notify_resource_updated("file:///notes.txt");`.
     *
     * @details See SyncServer.
     */
    [[nodiscard]] SyncServer sync();

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
     * @details Request parameter decoding failures are returned as
     * `mcp::constants::jsonrpc::g_INVALID_PARAMS` (-32602). Exceptions thrown by middleware, or by
     * resource, prompt and completion handlers, are returned as
     * `mcp::constants::jsonrpc::g_INTERNAL_ERROR` (-32603); a tool handler's exception becomes a tool
     * error result (`isError: true`).
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
     * Also sent, tagged with its subscription id, to each open subscriptions/listen
     * stream that asked for it; the same holds for the three notify methods below.
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

    /**
     * @brief Ends every open subscriptions/listen stream gracefully.
     *
     * Each stream's request is answered with its empty `resultType: "complete"` result carrying
     * the stream's `io.modelcontextprotocol/subscriptionId`, which the transport that carried the
     * request then delivers. Returns at once and may be called from any thread. Streams opened
     * later are not affected. A stream that ends because its client cancelled it, or its
     * transport closed, gets no response.
     */
    void close_subscriptions();

    /// @brief The number of subscriptions/listen streams currently open on this server.
    [[nodiscard]] std::size_t subscription_count() const;

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
    /// Registers @p tool, whose own schemas are unset, with @p input_schema and, unless it is null,
    /// @p output_schema. The server keeps the schemas in their compiled form, which servers that
    /// register the same schema share, so the schemas are not copied for each registration.
    void register_tool(Tool&& tool, const nlohmann::json& input_schema,
                       const nlohmann::json* output_schema, TypeErasedHandler handler,
                       detail::ToolResultMode result_mode);
    void register_resource(const Resource& resource, TypeErasedHandler handler);
    void register_resource_template(const ResourceTemplate& tmpl, TypeErasedHandler handler);
    void register_prompt(const Prompt& prompt, TypeErasedHandler handler);
    /// The synchronous counterparts of the helpers above: the handler is stored without a
    /// coroutine around it.
    void register_sync_tool(const Tool& tool, SyncTypeErasedHandler handler,
                            detail::ToolResultMode result_mode);
    void register_sync_tool(Tool&& tool, const nlohmann::json& input_schema,
                            const nlohmann::json* output_schema, SyncTypeErasedHandler handler,
                            detail::ToolResultMode result_mode);
    void register_sync_resource(const Resource& resource, SyncTypeErasedHandler handler);
    void register_sync_resource_template(const ResourceTemplate& tmpl, SyncTypeErasedHandler handler);
    void register_sync_prompt(const Prompt& prompt, SyncTypeErasedHandler handler);
    /// The registration helpers of the coroutine add_text_resource() and add_text_prompt().
    void register_text_resource(const Resource& resource, std::function<Task<std::string>()> read);
    void register_text_prompt(const Prompt& prompt,
                              std::function<Task<std::string>(const PromptArguments&)> render);

    /// Registers a typed tool: a sync handler without a coroutine, an async one through
    /// wrap_handler().
    template <JsonSerializable In, JsonSerializable Out, typename Fn>
    void add_typed_tool(const std::string& name, const std::string& description,
                        const nlohmann::json& input_schema, const nlohmann::json* output_schema,
                        Fn handler) {
        static_assert(detail::SyncHandler<Fn, In, Out> || detail::AsyncHandler<Fn, In, Out>,
                      "Handler must be sync or async with supported signature; a coroutine handler "
                      "returns mcp::Task<Out>, from <mcp/core/async.hpp>");
        Tool tool;
        tool.name = name;
        tool.description = description;
        constexpr auto result_mode = std::same_as<Out, CallToolResult>
                                         ? detail::ToolResultMode::eTyped
                                         : detail::ToolResultMode::eNormalize;
        if constexpr (detail::SyncHandler<Fn, In, Out>) {
            register_sync_tool(std::move(tool), input_schema, output_schema,
                               detail::wrap_sync_handler<In, Out>(std::move(handler)), result_mode);
        } else {
            register_tool(std::move(tool), input_schema, output_schema,
                          detail::wrap_handler<In, Out>(std::move(handler)), result_mode);
        }
    }

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

    /// The implementation information the name-and-version constructor gives the server.
    static Implementation named(std::string_view name, std::string_view version);

    struct Impl;
    explicit Server(std::shared_ptr<Impl> impl);
    /// A server for another session, sharing this server's identity, capabilities, registrations
    /// and settings; run_http() serves each legacy session with one.
    [[nodiscard]] std::unique_ptr<Server> session_server() const;

    friend class SyncServer;
    friend class StreamableHttpSessionManager;

    /// The runs of this server in progress, which the run forms register with.
    [[nodiscard]] std::shared_ptr<detail::RunControl> run_control() const;

    /// The protocol policy in effect (see set_protocol_policy()).
    [[nodiscard]] ServerProtocolPolicy protocol_policy() const;

    /// Shared rather than owned outright: work already in flight when this Server is destroyed
    /// keeps the implementation alive until it finishes, so it never runs against freed state.
    std::shared_ptr<Impl> impl_;
};

}  // namespace mcp

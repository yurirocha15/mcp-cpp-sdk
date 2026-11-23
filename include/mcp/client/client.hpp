#pragma once

// GCC 11 SSO Coroutine Safety — see docs/contributing.rst "Known Issues" for full details.
// Do NOT store std::string in a coroutine frame across co_await (GCC bugs #107288/#100611).
// Named patterns referenced at each call site below:
//   [not-a-coroutine]    Public methods with std::string params are plain functions; they
//                        serialise inputs before delegating to the real coroutine.
//   [string_view]        Use std::string_view (trivially copyable) for coroutine method params.
//   [int64-id]           Keep request IDs as int64_t across suspensions; rebuild string after.
//   [scope-before-await] Build SSO-risky objects in {}, serialise to wire string, then co_await.
//   [wire-builders]      make_result_wire/make_error_wire are synchronous helpers;
//                        do NOT convert them to Task<T> coroutines.

#include <mcp/client/cancellation_token.hpp>
#include <mcp/client/client_options.hpp>
#include <mcp/client/client_protocol.hpp>
#include <mcp/client/error.hpp>
#include <mcp/client/request_options.hpp>
#include <mcp/client/subscription.hpp>
#include <mcp/client/sync_client.hpp>
#include <mcp/client/task_handle.hpp>
#include <mcp/core/async_fwd.hpp>
#include <mcp/core/concepts.hpp>
#include <mcp/core/constants.hpp>
#include <mcp/core/export.hpp>
#include <mcp/core/notification_rate_limit.hpp>
#include <mcp/protocol/protocol.hpp>
#include <mcp/transport/transport.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <mcp/client/endpoint.hpp>
#include <mcp/core/executor.hpp>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace mcp {

/**
 * @brief MCP client that communicates with a server over a transport.
 *
 * @details Manages the JSON-RPC request/response lifecycle, including
 * an asynchronous read loop that dispatches incoming responses to their
 * corresponding pending requests. connect() settles on a protocol era
 * (ClientOptions::protocol): a legacy session starts with an initialize request
 * followed by an initialized notification, a modern one sends no handshake and
 * carries its protocol fields in each request's `_meta`.
 *
 * Runtime I/O and request state are serialized on a boost::asio::strand.
 */
class MCP_API Client {
   public:
    /**
     * @brief Construct a Client.
     *
     * @param transport The transport to use for message exchange.
     *                  Ownership is shared with the client.
     * @param executor  The executor to use for async operations.
     * @param options   Default timeout and protocol-validation behavior.
     */
    Client(std::shared_ptr<ITransport> transport, const Executor& executor, ClientOptions options = {});

    /**
     * @brief Construct a client of the server @p endpoint names, running its own event loop.
     *
     * @details The client starts a thread that runs an event loop for it, makes the endpoint's
     * transport on that loop, and needs no executor from the application: use it through sync()
     * or run(), or await its coroutines from run(). The destructor closes the connection, waits
     * for the loop to finish (a launched server exits and its StdioProcessOptions::on_exit runs)
     * up to mcp::constants::client::g_CLOSE_WAIT_MS, or as long as a launched server's
     * stdin_close_wait and terminate_wait when they add up to more, and joins the thread. A Client
     * constructed this way must not be destroyed on its own loop's thread, such as from a
     * notification callback.
     *
     * @param endpoint The server: Endpoint::stdio(), Endpoint::http() or Endpoint::custom().
     * @param options  Default timeout and protocol-validation behavior.
     * @throws std::runtime_error When the server of Endpoint::stdio() cannot be launched.
     * @throws std::invalid_argument When @p options holds a limit that is not positive, or the
     *         transport factory of Endpoint::custom() returns null.
     */
    explicit Client(const Endpoint& endpoint, ClientOptions options = {});

    /**
     * @brief Blocking calls to this client: each method runs the coroutine member of the same name
     *        on the client's event loop and waits for it.
     *
     * @details For a Client constructed from an Endpoint. A blocking call made on the client's
     * own event loop thread, such as from a notification callback or a coroutine awaiting the
     * client, would wait for itself and throws std::logic_error instead. Blocking calls, run()
     * and coroutines may be mixed on one client.
     * @throws std::logic_error If the client runs on the application's executor, whose event loop
     *         it cannot run itself: such a client is used through its coroutines.
     */
    [[nodiscard]] SyncClient sync();

    /**
     * @brief Run @p body, a coroutine that uses this client, on the client's event loop and wait
     *        for it to finish.
     *
     * @details For a Client constructed from an Endpoint. The coroutine's exception, if it throws,
     * is rethrown here. @p body is kept alive until the coroutine has finished, so a lambda may
     * capture what the coroutine uses.
     * @throws std::logic_error If the client runs on the application's executor, or the call is
     *         made on the client's own event loop thread.
     * @throws std::invalid_argument If @p body is empty or returns an empty Task.
     */
    void run(std::function<Task<void>()> body);

    /**
     * @brief The executor the Client was constructed with.
     *
     * @details Coroutines that use this Client can be spawned on it; get() returns it as a
     * boost::asio::any_io_executor.
     */
    [[nodiscard]] Executor executor() const;

    /// @brief Destructor. Closes the transport and clears pending requests
    /// so that asio-tied objects (timers, streams) are destroyed while the
    /// io_context is still alive.
    ~Client();

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;
    Client(Client&&) = delete;
    Client& operator=(Client&&) = delete;

    /**
     * @brief Connect with default capabilities.
     * @param name Client implementation name.
     * @param version Client implementation version.
     * @return A task that resolves to what the server declared; see the overload below.
     */
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    Task<ConnectResult> connect(std::string_view name, std::string_view version);

    /**
     * @brief Connect to the server and settle on a protocol era.
     *
     * @details Starts the read loop, then follows ClientOptions::protocol. The capabilities the
     * client declares are @p capabilities plus what the registered handlers imply: roots,
     * sampling and elicitation when a handler answers roots/list, sampling/createMessage or
     * elicitation/create (on_roots_list(), set_roots(), on_sampling(), on_elicitation() or
     * on_request()). A legacy session sends them in initialize, and then
     * notifications/initialized. A modern session sends server/discover once to learn the
     * server's capabilities and sends nothing else; from then on every request carries
     * `io.modelcontextprotocol/protocolVersion`, the capabilities, counting handlers registered
     * since connect(), and @p client_info in its `_meta`, merged with the `_meta` entries the
     * caller supplied. The era is fixed for the client's lifetime.
     *
     * In a modern session ping() and the removed subscription and log-level methods fail
     * locally, set_roots() sends no list-changed notification, and a result whose `resultType`
     * is neither absent nor "complete" fails with McpError.
     *
     * @param client_info Information about this client implementation.
     * @param capabilities Capabilities supported by this client.
     * @return A task that resolves to what the server declared.
     * @throws McpError If the server cannot be reached, closes the connection while it is probed
     *         (pin ClientProtocol::legacy() for such a server), or shares no protocol version.
     */
    Task<ConnectResult> connect(const Implementation& client_info,
                                const ClientCapabilities& capabilities);

    /**
     * @brief Send a JSON-RPC request and await its response.
     *
     * @details Assigns a unique integer ID, serializes the request on the stack,
     * and delegates to a coroutine that handles transport and suspension.
     * This is done to avoid GCC 11 SSO bugs and dangling references.
     *
     * @param method JSON-RPC method name.
     * @param params Optional parameters for the request.
     * @param request_options Per-request options, such as a deadline or a CancellationToken.
     * @return A task that resolves to the result JSON object.
     * @throws McpError If the peer returns an error response or the request times out.
     */
    // [gcc11-sso: string_view]
    Task<nlohmann::json> send_request(std::string_view method,
                                      const std::optional<nlohmann::json>& params,
                                      const RequestOptions& request_options = {});

    /**
     * @brief Open a subscriptions/listen stream (protocol revision 2026-07-28 and later).
     *
     * @details Sends the request and waits for the server's
     * `notifications/subscriptions/acknowledged`, then returns a handle for the stream. The
     * acknowledged filter may be a subset of @p filter; see Subscription::unhonored(). The
     * request is never answered while the stream is open, so ClientOptions::request_timeout (or
     * RequestOptions::timeout) limits only the wait for the acknowledgment. Any number of
     * streams may be open at once; each receives only the notifications that carry its id.
     * The handlers registered with on_notification() still run for these notifications as well.
     *
     * @param filter The notification types wanted.
     * @param request_options Per-request options; RequestOptions::timeout bounds the wait for the
     *        acknowledgment.
     * @return A task that resolves to the open stream.
     * @throws McpError mcp::constants::jsonrpc::g_METHOD_NOT_FOUND when the session is not modern (a
     * legacy session, or connect() has not finished), mcp::constants::jsonrpc::g_INVALID_REQUEST before
     * connect(), mcp::constants::jsonrpc::g_REQUEST_TIMEOUT when no acknowledgment arrives in time,
     * mcp::constants::jsonrpc::g_CONNECTION_CLOSED when the connection ends first, or the server's
     * error (for example mcp::constants::jsonrpc::g_INVALID_PARAMS for a malformed filter,
     * mcp::constants::jsonrpc::g_METHOD_NOT_FOUND for a server or transport without streams).
     */
    Task<Subscription> listen(const SubscriptionFilter& filter,
                              const RequestOptions& request_options = {});

    /**
     * @brief Send a JSON-RPC notification (no response expected).
     *
     * @param method The notification method name.
     * @param params Optional parameters for the notification.
     * @return A task that completes when the notification has been written to the transport.
     */
    // [gcc11-sso: string_view]
    Task<void> send_notification(std::string_view method, const std::optional<nlohmann::json>& params);

    /**
     * @brief Get the number of pending (in-flight) requests.
     *
     * @return The count of requests awaiting responses.
     */
    [[nodiscard]] std::size_t pending_request_count() const;

    /**
     * @brief Drop every result the response cache holds, so each cacheable call asks the server
     * again. Safe from any thread.
     */
    void clear_response_cache();

    /// @brief Progress and log notifications dropped by ClientOptions::notification_rate_limit.
    [[nodiscard]] std::uint64_t dropped_notifications() const;

    /// @brief Close the client, stopping the read loop and releasing resources.
    void close();

    /**
     * @brief Call a tool on the server.
     *
     * @details Serializes the arguments to JSON, sends a "tools/call"
     * request, and deserializes the response into a CallToolResult.
     *
     * @tparam Args A JsonSerializable type for the tool arguments.
     * @param name The name of the tool to call.
     * @param arguments The arguments to pass to the tool.
     * @param request_options Per-request options, such as a deadline or a CancellationToken.
     * @return A task that resolves to the server's CallToolResult.
     * @throws StructuredContentError when the result's `structuredContent` does not conform to
     *         the tool's output schema (see ClientOptions::validate_tool_schemas).
     * @throws McpError with mcp::constants::jsonrpc::g_REQUEST_CANCELLED for a cancelled call.
     */
    template <JsonSerializable Args, typename Self = Client>
        requires(!std::same_as<Args, nlohmann::json>)
    auto call_tool(const std::string& name, const Args& arguments,
                   const RequestOptions& request_options = {}) {
        // [gcc11-sso: not-a-coroutine]
        CallToolParams call_params;
        call_params.name = name;
        call_params.arguments = nlohmann::json(arguments);
        return static_cast<Self*>(this)->call_tool_with(std::move(call_params), request_options);
    }

    /**
     * @brief Call a tool with JSON arguments.
     *
     * @details As call_tool() above. Null arguments, which `{}` makes, are sent as an empty object.
     * Build other arguments in a variable before the co_await, such as
     * `nlohmann::json arguments{{"a", 1}, {"b", 2}}; co_await client.call_tool("add", arguments);`:
     * GCC 12 and 13 crash on a braced list inside a co_await (see the README's compiler notes).
     */
    Task<CallToolResult> call_tool(const std::string& name, const nlohmann::json& arguments,
                                   const RequestOptions& request_options = {});

    /**
     * @brief Call a tool, taking a task if the server starts one (protocol revision 2026-07-28).
     *
     * @details Declares the tasks extension for this call whatever ClientOptions::tasks is set
     * to. The server decides whether the call becomes a task.
     *
     * @tparam Args A JsonSerializable type for the tool arguments.
     * @param request_options Per-request options, such as a deadline or a CancellationToken.
     * @return The tool's result when the server ran the call to its end, or a TaskHandle when it
     *         answered with a task. A legacy session always gets the result.
     * @throws StructuredContentError as call_tool() does for a result.
     */
    template <JsonSerializable Args, typename Self = Client>
        requires(!std::same_as<Args, nlohmann::json>)
    auto start_tool(const std::string& name, const Args& arguments,
                    const RequestOptions& request_options = {}) {
        // [gcc11-sso: not-a-coroutine]
        CallToolParams call_params;
        call_params.name = name;
        call_params.arguments = nlohmann::json(arguments);
        return static_cast<Self*>(this)->start_tool_with(std::move(call_params), request_options);
    }

    /**
     * @brief Start a tool call with JSON arguments.
     *
     * @details As start_tool() above, with the arguments as call_tool() takes JSON ones.
     */
    Task<std::variant<CallToolResult, TaskHandle>> start_tool(
        const std::string& name, const nlohmann::json& arguments,
        const RequestOptions& request_options = {});

    /**
     * @brief A handle to a task by its id, for a task started earlier.
     *
     * @details The handle's created() holds nothing but the id until get() or wait() fetches the
     * task's state.
     */
    [[nodiscard]] TaskHandle task_handle(std::string id);

    /**
     * @brief List available tools from the server.
     *
     * Tools whose schemas cannot be used are left out (see ClientOptions::validate_tool_schemas).
     *
     * @param cursor Optional pagination cursor.
     * @param request_options Per-request options, such as a deadline or a CancellationToken.
     * @return A task that resolves to the server's ListToolsResult.
     */
    Task<ListToolsResult> list_tools(const std::optional<std::string>& cursor = std::nullopt,
                                     const RequestOptions& request_options = {});

    /**
     * @brief List available resources from the server.
     *
     * @param cursor Optional pagination cursor.
     * @param request_options Per-request options, such as a deadline or a CancellationToken.
     * @return A task that resolves to the server's ListResourcesResult.
     */
    Task<ListResourcesResult> list_resources(const std::optional<std::string>& cursor = std::nullopt,
                                             const RequestOptions& request_options = {});

    /**
     * @brief Read a resource from the server by URI.
     *
     * @param uri The URI of the resource to read.
     * @param request_options Per-request options, such as a deadline or a CancellationToken.
     * @return A task that resolves to the server's ReadResourceResult.
     */
    Task<ReadResourceResult> read_resource(const std::string& uri,
                                           const RequestOptions& request_options = {});

    /**
     * @brief List available resource templates from the server.
     *
     * @param cursor Optional pagination cursor.
     * @param request_options Per-request options, such as a deadline or a CancellationToken.
     * @return A task that resolves to the server's ListResourceTemplatesResult.
     */
    Task<ListResourceTemplatesResult> list_resource_templates(
        const std::optional<std::string>& cursor = std::nullopt,
        const RequestOptions& request_options = {});

    /**
     * @brief List available prompts from the server.
     *
     * @param cursor Optional pagination cursor.
     * @param request_options Per-request options, such as a deadline or a CancellationToken.
     * @return A task that resolves to the server's ListPromptsResult.
     */
    Task<ListPromptsResult> list_prompts(const std::optional<std::string>& cursor = std::nullopt,
                                         const RequestOptions& request_options = {});

    /**
     * @brief Get a specific prompt from the server.
     *
     * @param name The name of the prompt.
     * @param arguments The arguments that fill in the prompt, by name; the request carries none
     *        when the map is empty. As for call_tool(), build them in a variable before the
     *        co_await.
     * @param request_options Per-request options, such as a deadline or a CancellationToken.
     * @return A task that resolves to the server's GetPromptResult.
     */
    Task<GetPromptResult> get_prompt(const std::string& name,
                                     const std::map<std::string, std::string>& arguments = {},
                                     const RequestOptions& request_options = {});

    /**
     * @brief Request completions from the server.
     *
     * @param params The completion parameters.
     * @param request_options Per-request options, such as a deadline or a CancellationToken.
     * @return A task that resolves to the server's CompleteResult.
     */
    Task<CompleteResult> complete(const CompleteParams& params,
                                  const RequestOptions& request_options = {});

    /**
     * @brief Send a ping request to the connected server.
     *
     * @details Fails with McpError (mcp::constants::jsonrpc::g_METHOD_NOT_FOUND) without sending
     * anything in a modern session: protocol revision 2026-07-28 has no ping.
     *
     * @return A task that completes once the ping round-trip succeeds.
     */
    Task<void> ping();

    /**
     * @brief Send a cancellation notification for an in-flight request.
     *
     * @details To cancel a request without knowing its id, give it a CancellationToken in
     * RequestOptions::cancellation instead.
     *
     * @param request_id The ID of the request to cancel.
     * @param reason Optional human-readable reason for the cancellation.
     * @return A task that completes when the cancellation notification has been sent.
     */
    Task<void> cancel(const RequestId& request_id,
                      const std::optional<std::string>& reason = std::nullopt);

    /// @brief Type for notification callback: receives the notification params JSON.
    using NotificationCallback = std::function<void(const nlohmann::json&)>;

    /// @brief Type for request handler: receives params JSON, returns result JSON.
    using RequestHandler = std::function<Task<nlohmann::json>(const nlohmann::json&)>;

    /**
     * @brief Register a callback for incoming notifications from the server.
     *
     * @details When the server sends a notification with the given method,
     * the callback is invoked with the notification's params JSON.
     *
     * @param method The notification method to listen for (e.g. "notifications/progress").
     * @param callback The callback to invoke when the notification arrives.
     */
    void on_notification(const std::string& method, NotificationCallback callback);

    /// @brief Callback invoked for a deserialized progress notification.
    using ProgressCallback = std::function<void(const ProgressNotificationParams&)>;

    /**
     * @brief Register a callback for progress notifications.
     *
     * @details A notification whose token belongs to a request in flight with
     * RequestOptions::on_progress set goes to that request's callback instead.
     *
     * @param callback The callback invoked when a notifications/progress message arrives.
     */
    void on_progress(ProgressCallback callback);

    /**
     * @brief Register a handler for incoming requests from the server (reverse RPC).
     *
     * @details When the server sends a request with the given method (e.g. "ping",
     * "sampling/createMessage", "elicitation/create", "roots/list"), the handler
     * is invoked and its return value is sent back as the JSON-RPC response.
     *
     * @param method The request method to handle.
     * @param handler The async handler that returns a result JSON.
     */
    void on_request(const std::string& method, RequestHandler handler);

    /**
     * @brief Register a handler for incoming elicitation requests from the server.
     *
     * @details Convenience wrapper that registers a request handler for "elicitation/create".
     * The handler receives ElicitRequestParams and returns ElicitResult. Accepted form content is
     * checked against the request's schema first, see ClientOptions::validate_elicitation_content.
     *
     * @param handler Async handler: receives ElicitRequestParams, returns ElicitResult.
     */
    void on_elicitation(std::function<Task<ElicitResult>(const ElicitRequestParams&)> handler);

    /**
     * @brief Register a handler for sampling requests from the server.
     *
     * @deprecated doc-only: protocol revision 2026-07-28 deprecates sampling; see Deprecated Features
     * on the Specification Support page for why the SDK keeps it.
     *
     * @details Registers a request handler for "sampling/createMessage": the handler receives the
     * request's CreateMessageRequestParams and returns the CreateMessageResult sent back. A
     * modern server's `input_required` sampling questions are answered by it too. Registering
     * it declares the sampling capability, see connect().
     *
     * @param handler Async handler: receives CreateMessageRequestParams, returns
     *        CreateMessageResult.
     */
    void on_sampling(
        std::function<Task<CreateMessageResult>(const CreateMessageRequestParams&)> handler);

    /**
     * @brief Set static roots and auto-register the roots/list handler.
     *
     * @deprecated doc-only: protocol revision 2026-07-28 deprecates roots; see Deprecated Features
     * on the Specification Support page for why the SDK keeps it.
     *
     * @details Stores the given roots and registers a request handler for
     * "roots/list" that returns them. If notify is true, sends a
     * notifications/roots/list_changed notification to inform the server. A modern session has
     * no such notification, and nothing is sent.
     *
     * @param roots The list of roots to provide.
     * @param notify Whether to send a roots-changed notification.
     */
    void set_roots(const std::vector<Root>& roots, bool notify = false);

    /**
     * @brief Register a dynamic handler for roots/list requests.
     *
     * @deprecated doc-only: protocol revision 2026-07-28 deprecates roots; see Deprecated Features
     * on the Specification Support page for why the SDK keeps it.
     *
     * @param handler Async handler that returns a ListRootsResult.
     */
    void on_roots_list(std::function<Task<ListRootsResult>(const nlohmann::json&)> handler);

   private:
    friend class SyncClient;
    friend class TaskHandle;

    Task<CallToolResult> call_tool_with(CallToolParams params,
                                        const RequestOptions& request_options = {});
    Task<std::variant<CallToolResult, TaskHandle>> start_tool_with(
        CallToolParams params, const RequestOptions& request_options = {});

    void start(std::shared_ptr<ITransport> transport, const Executor& executor, ClientOptions options);

    /// The event loop of a client constructed from an Endpoint; null otherwise. Declared before
    /// impl_ so that impl_ is destroyed first. The state of a TaskHandle or Subscription shares it,
    /// so that the loop's io_context outlives the objects of that state that belong to it.
    struct OwnedLoop;
    std::shared_ptr<OwnedLoop> loop_;

    struct Impl;
    std::shared_ptr<Impl> impl_;
};

}  // namespace mcp

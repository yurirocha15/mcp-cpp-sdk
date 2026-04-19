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

#include <mcp/concepts.hpp>
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
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mcp {

/**
 * @brief MCP client that communicates with a server over a transport.
 *
 * @details Manages the JSON-RPC request/response lifecycle, including
 * an asynchronous read loop that dispatches incoming responses to their
 * corresponding pending requests. The client initiates the MCP handshake
 * via connect(), which sends an initialize request followed by an
 * initialized notification.
 *
 * All async operations execute on a boost::asio::strand for thread safety
 * without std::mutex.
 */
class Client {
   public:
    /**
     * @brief Construct a Client.
     *
     * @param transport The transport to use for message exchange.
     *                  Ownership is shared with the client.
     * @param executor  The executor to use for async operations.
     */
    Client(std::shared_ptr<ITransport> transport, const boost::asio::any_io_executor& executor)
        : transport_(std::move(transport)), strand_(boost::asio::make_strand(executor)) {}

    /// @brief Destructor. Closes the transport and clears pending requests
    /// so that asio-tied objects (timers, streams) are destroyed while the
    /// io_context is still alive.
    ~Client() {
        if (transport_) {
            transport_->close();
        }
        pending_requests_.clear();
    }

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;
    Client(Client&&) = delete;
    Client& operator=(Client&&) = delete;

    /**
     * @brief Perform the MCP initialization handshake.
     *
     * @details Starts the read loop via co_spawn, sends an initialize
     * request with the given client info and capabilities, waits for the
     * server response, then sends an initialized notification.
     *
     * @param client_info Information about this client implementation.
     * @param capabilities The capabilities this client supports.
     * @return A task that resolves to the server's InitializeResult.
     */
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    Task<InitializeResult> connect(std::string_view name, std::string_view version) {
        Implementation info;
        info.name = std::string(name);
        info.version = std::string(version);
        return connect(info, {});
    }

    Task<InitializeResult> connect(const Implementation& client_info,
                                   const ClientCapabilities& capabilities) {
        // [gcc11-sso: not-a-coroutine] DO NOT add co_await / co_return here.
        boost::asio::co_spawn(strand_, read_loop(), boost::asio::detached);
        InitializeRequest init_req;
        init_req.protocolVersion = std::string(g_LATEST_PROTOCOL_VERSION);
        init_req.clientInfo = client_info;
        init_req.capabilities = capabilities;
        return connect_impl(nlohmann::json(std::move(init_req)));
    }

    /**
     * @brief Send a JSON-RPC request and await its response.
     *
     * @details Assigns a unique integer ID, serializes the request on the stack,
     * and delegates to a coroutine that handles transport and suspension.
     * This is done to avoid GCC 11 SSO bugs and dangling references.
     *
     * @param method JSON-RPC method name.
     * @param params Optional parameters for the request.
     * @return A task that resolves to the result JSON object.
     * @throws std::runtime_error If the server returns an error response.
     */
    // [gcc11-sso: string_view]
    Task<nlohmann::json> send_request(std::string_view method,
                                      const std::optional<nlohmann::json>& params) {
        int64_t id = next_request_id_.fetch_add(1, std::memory_order_relaxed);
        auto id_str = std::to_string(id);

        JSONRPCRequest request;
        request.id = RequestId{id_str};
        request.method = std::string(method);
        request.params = params;

        auto& pending = pending_requests_[id_str];
        pending.timer = std::make_unique<boost::asio::steady_timer>(strand_);
        pending.timer->expires_at(std::chrono::steady_clock::time_point::max());
        auto* timer_ptr = pending.timer.get();

        return send_request_impl(nlohmann::json(std::move(request)).dump(), timer_ptr, id);
    }

    /**
     * @brief Send a JSON-RPC notification (no response expected).
     *
     * @param method The notification method name.
     * @param params Optional parameters for the notification.
     * @return A task that completes when the notification has been written to the transport.
     */
    // [gcc11-sso: string_view]
    Task<void> send_notification(std::string_view method, const std::optional<nlohmann::json>& params) {
        JSONRPCNotification notification;
        notification.method = std::string(method);
        notification.params = params;
        return send_notification_impl(nlohmann::json(std::move(notification)).dump());
    }

    /**
     * @brief Get the number of pending (in-flight) requests.
     *
     * @return The count of requests awaiting responses.
     */
    [[nodiscard]] std::size_t pending_request_count() const { return pending_requests_.size(); }

    /// @brief Close the client, stopping the read loop and releasing resources.
    void close() {
        if (transport_) {
            transport_->close();
        }
        pending_requests_.clear();
    }

    /**
     * @brief Call a tool on the server.
     *
     * @details Serializes the arguments to JSON, sends a "tools/call"
     * request, and deserializes the response into a CallToolResult.
     *
     * @tparam Args A JsonSerializable type for the tool arguments.
     * @param name The name of the tool to call.
     * @param arguments The arguments to pass to the tool.
     * @return A task that resolves to the server's CallToolResult.
     */
    template <JsonSerializable Args>
    Task<CallToolResult> call_tool(const std::string& name, const Args& arguments) {
        // [gcc11-sso: not-a-coroutine]
        CallToolParams call_params;
        call_params.name = name;
        call_params.arguments = nlohmann::json(arguments);
        return call_and_parse<CallToolResult>("tools/call", nlohmann::json(std::move(call_params)));
    }

    /**
     * @brief List available tools from the server.
     *
     * @param cursor Optional pagination cursor.
     * @return A task that resolves to the server's ListToolsResult.
     */
    Task<ListToolsResult> list_tools(const std::optional<std::string>& cursor = std::nullopt) {
        // [gcc11-sso: not-a-coroutine]
        std::optional<nlohmann::json> params;
        if (cursor) {
            PaginatedRequestParams paginated;
            paginated.cursor = cursor;
            params = nlohmann::json(std::move(paginated));
        }
        return call_and_parse<ListToolsResult>("tools/list", std::move(params));
    }

    /**
     * @brief List available resources from the server.
     *
     * @param cursor Optional pagination cursor.
     * @return A task that resolves to the server's ListResourcesResult.
     */
    Task<ListResourcesResult> list_resources(const std::optional<std::string>& cursor = std::nullopt) {
        // [gcc11-sso: not-a-coroutine]
        std::optional<nlohmann::json> params;
        if (cursor) {
            PaginatedRequestParams paginated;
            paginated.cursor = cursor;
            params = nlohmann::json(std::move(paginated));
        }
        return call_and_parse<ListResourcesResult>("resources/list", std::move(params));
    }

    /**
     * @brief Read a resource from the server by URI.
     *
     * @param uri The URI of the resource to read.
     * @return A task that resolves to the server's ReadResourceResult.
     */
    Task<ReadResourceResult> read_resource(const std::string& uri) {
        // [gcc11-sso: not-a-coroutine]
        ReadResourceRequestParams p;
        p.uri = uri;
        return call_and_parse<ReadResourceResult>("resources/read", nlohmann::json(std::move(p)));
    }

    /**
     * @brief List available resource templates from the server.
     *
     * @param cursor Optional pagination cursor.
     * @return A task that resolves to the server's ListResourceTemplatesResult.
     */
    Task<ListResourceTemplatesResult> list_resource_templates(
        const std::optional<std::string>& cursor = std::nullopt) {
        // [gcc11-sso: not-a-coroutine]
        std::optional<nlohmann::json> params;
        if (cursor) {
            PaginatedRequestParams paginated;
            paginated.cursor = cursor;
            params = nlohmann::json(std::move(paginated));
        }
        return call_and_parse<ListResourceTemplatesResult>("resources/templates/list",
                                                           std::move(params));
    }

    /**
     * @brief List available prompts from the server.
     *
     * @param cursor Optional pagination cursor.
     * @return A task that resolves to the server's ListPromptsResult.
     */
    Task<ListPromptsResult> list_prompts(const std::optional<std::string>& cursor = std::nullopt) {
        // [gcc11-sso: not-a-coroutine]
        std::optional<nlohmann::json> params;
        if (cursor) {
            PaginatedRequestParams paginated;
            paginated.cursor = cursor;
            params = nlohmann::json(std::move(paginated));
        }
        return call_and_parse<ListPromptsResult>("prompts/list", std::move(params));
    }

    /**
     * @brief Get a specific prompt from the server.
     *
     * @param name The name of the prompt.
     * @param arguments Optional arguments to template the prompt.
     * @return A task that resolves to the server's GetPromptResult.
     */
    Task<GetPromptResult> get_prompt(
        const std::string& name,
        const std::optional<std::map<std::string, std::string>>& arguments = std::nullopt) {
        // [gcc11-sso: not-a-coroutine]
        GetPromptRequestParams p;
        p.name = name;
        p.arguments = arguments;
        return call_and_parse<GetPromptResult>("prompts/get", nlohmann::json(std::move(p)));
    }

    /**
     * @brief Request completions from the server.
     *
     * @param params The completion parameters.
     * @return A task that resolves to the server's CompleteResult.
     */
    Task<CompleteResult> complete(const CompleteParams& params) {
        // [gcc11-sso: not-a-coroutine]
        return call_and_parse<CompleteResult>("completion/complete", nlohmann::json(params));
    }

    /**
     * @brief Send a ping request to the connected server.
     *
     * @return A task that completes once the ping round-trip succeeds.
     */
    Task<void> ping() { co_await send_request("ping", std::nullopt); }

    /**
     * @brief Send a cancellation notification for an in-flight request.
     *
     * @param request_id The ID of the request to cancel.
     * @param reason Optional human-readable reason for the cancellation.
     * @return A task that completes when the cancellation notification has been sent.
     */
    Task<void> cancel(const RequestId& request_id,
                      const std::optional<std::string>& reason = std::nullopt) {
        // [gcc11-sso: not-a-coroutine]
        CancelledNotificationParams params;
        params.requestId = request_id;
        params.reason = reason;
        return send_notification("notifications/cancelled", nlohmann::json(std::move(params)));
    }

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
    void on_notification(const std::string& method, NotificationCallback callback) {
        notification_handlers_[method] = std::move(callback);
    }

    /// @brief Callback invoked for a deserialized progress notification.
    using ProgressCallback = std::function<void(const ProgressNotificationParams&)>;

    /**
     * @brief Register a callback for progress notifications.
     *
     * @param callback The callback invoked when a notifications/progress message arrives.
     */
    void on_progress(ProgressCallback callback) {
        on_notification("notifications/progress",
                        [cb = std::move(callback)](const nlohmann::json& params) {
                            cb(params.get<ProgressNotificationParams>());
                        });
    }

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
    void on_request(const std::string& method, RequestHandler handler) {
        request_handlers_[method] = std::move(handler);
    }

    /**
     * @brief Register a handler for incoming elicitation requests from the server.
     *
     * @details Convenience wrapper that registers a request handler for "elicitation/create".
     * The handler receives ElicitRequestParams and returns ElicitResult.
     *
     * @param handler Async handler: receives ElicitRequestParams, returns ElicitResult.
     */
    void on_elicitation(std::function<Task<ElicitResult>(const ElicitRequestParams&)> handler) {
        on_request("elicitation/create",
                   [h = std::move(handler)](const nlohmann::json& params) -> Task<nlohmann::json> {
                       auto elicit_params = params.get<ElicitRequestParams>();
                       auto result = co_await h(elicit_params);
                       nlohmann::json result_json = std::move(result);
                       co_return result_json;
                   });
    }

    /**
     * @brief Set static roots and auto-register the roots/list handler.
     *
     * @details Stores the given roots and registers a request handler for
     * "roots/list" that returns them. If notify is true, sends a
     * notifications/roots/list_changed notification to inform the server.
     *
     * @param roots The list of roots to provide.
     * @param notify Whether to send a roots-changed notification.
     */
    void set_roots(const std::vector<Root>& roots, bool notify = false) {
        roots_ = roots;

        if (request_handlers_.find("roots/list") == request_handlers_.end()) {
            on_request("roots/list", [this](const nlohmann::json&) -> Task<nlohmann::json> {
                ListRootsResult result;
                result.roots = roots_;
                nlohmann::json j = std::move(result);
                co_return j;
            });
        }

        bool server_supports_roots_changed =
            server_capabilities_.resources.has_value() &&
            server_capabilities_.resources->listChanged.value_or(false);

        if (notify && server_supports_roots_changed && transport_) {
            boost::asio::co_spawn(
                strand_,
                [this]() -> Task<void> {
                    co_await send_notification("notifications/roots/list_changed", std::nullopt);
                },
                boost::asio::detached);
        }
    }

    /**
     * @brief Register a dynamic handler for roots/list requests.
     *
     * @param handler Async handler that returns a ListRootsResult.
     */
    void on_roots_list(std::function<Task<ListRootsResult>(const nlohmann::json&)> handler) {
        on_request("roots/list",
                   [h = std::move(handler)](const nlohmann::json& params) -> Task<nlohmann::json> {
                       auto result = co_await h(params);
                       nlohmann::json j = std::move(result);
                       co_return j;
                   });
    }

   private:
    struct PendingRequest {
        std::unique_ptr<boost::asio::steady_timer> timer;
        nlohmann::json result;
        std::optional<Error> error;
    };

    // [gcc11-sso: not-a-coroutine] DO NOT change init_params to a type containing std::string.
    Task<InitializeResult> connect_impl(nlohmann::json init_params) {
        auto result_json = co_await send_request("initialize", std::move(init_params));
        server_capabilities_ = result_json.get<InitializeResult>().capabilities;
        co_await send_notification("notifications/initialized", std::nullopt);
        co_return result_json.get<InitializeResult>();
    }

    Task<nlohmann::json> send_request_impl(std::string wire, boost::asio::steady_timer* timer_ptr,
                                           int64_t id) {
        co_await transport_->write_message(wire);

        try {
            co_await timer_ptr->async_wait(boost::asio::use_awaitable);
        } catch (const boost::system::system_error& err) {
            if (err.code() != boost::asio::error::operation_aborted) {
                throw;
            }
        }

        auto id_str = std::to_string(id);
        auto it = pending_requests_.find(id_str);
        if (it == pending_requests_.end()) {
            throw std::runtime_error("pending request not found for id: " + id_str);
        }
        auto result = std::move(it->second.result);
        auto error = std::move(it->second.error);
        pending_requests_.erase(it);

        if (error) {
            throw std::runtime_error("JSON-RPC error " + std::to_string(error->code) + ": " +
                                     error->message);
        }
        co_return result;
    }

    Task<void> send_notification_impl(std::string wire) { co_await transport_->write_message(wire); }

    // [gcc11-sso: string_view] nlohmann::json is heap-allocated; string_view is trivially copyable.
    template <typename Result>
    Task<Result> call_and_parse(std::string_view method, std::optional<nlohmann::json> params) {
        auto result_json = co_await send_request(method, params);
        co_return result_json.template get<Result>();
    }

    /**
     * @brief Infinite read loop that dispatches incoming messages.
     *
     * @details Continuously reads messages from the transport, classifies
     * them as responses, requests, or notifications, and dispatches each
     * to the appropriate handler. Responses wake their pending coroutine.
     * Requests are dispatched to registered request handlers and their
     * result is sent back. Notifications invoke registered callbacks.
     */
    Task<void> read_loop() {
        if (!transport_) {
            co_return;
        }
        try {
            for (;;) {
                auto raw = co_await transport_->read_message();
                auto json_msg = nlohmann::json::parse(raw);

                bool has_id = json_msg.contains("id");
                bool has_method = json_msg.contains("method");

                if (has_id && !has_method) {
                    dispatch_response(json_msg);
                } else if (has_id && has_method) {
                    boost::asio::co_spawn(strand_, dispatch_incoming_request(std::move(json_msg)),
                                          boost::asio::detached);
                } else if (!has_id && has_method) {
                    dispatch_notification(json_msg);
                }
            }
        } catch (const std::exception& e) {
            // Transport closed or error occurred, terminate loop.
            (void)e;
        }
    }

    void dispatch_response(const nlohmann::json& json_msg) {
        auto id = json_msg.at("id").get<RequestId>();
        auto id_str = id.to_string();

        auto it = pending_requests_.find(id_str);
        if (it == pending_requests_.end()) {
            return;
        }

        if (json_msg.contains("error")) {
            it->second.error = json_msg.at("error").get<Error>();
        } else if (json_msg.contains("result")) {
            it->second.result = json_msg.at("result");
        }

        it->second.timer->cancel();
    }

    void dispatch_notification(const nlohmann::json& json_msg) {
        auto method = json_msg.at("method").get<std::string>();

        if (method == "notifications/cancelled") {
            if (json_msg.contains("params")) {
                auto params = json_msg.at("params").get<CancelledNotificationParams>();
                auto id_str = params.requestId.to_string();

                auto it = pending_requests_.find(id_str);
                if (it != pending_requests_.end()) {
                    it->second.error = Error{g_REQUEST_CANCELLED, "Request cancelled by server"};
                    it->second.timer->cancel();
                }
            }
            return;
        }

        auto it = notification_handlers_.find(method);
        if (it != notification_handlers_.end()) {
            auto params = json_msg.contains("params") ? json_msg.at("params") : nlohmann::json{};
            it->second(params);
        }
    }

    /**
     * @brief Handles an incoming JSON-RPC request from the server (reverse RPC).
     *
     * @details Looks up the method in request_handlers_ and invokes the registered
     * handler. Exceptions from handlers are caught and returned as JSON-RPC error
     * responses with code -32603 (g_INTERNAL_ERROR). The @c ping method is handled
     * automatically and returns an empty result object. Unknown methods return a
     * g_METHOD_NOT_FOUND error response. Runs on the client's asio strand
     * (co_spawned from the read loop).
     *
     * @param json_msg The raw JSON-RPC request with @c id, @c method, and optional @c params.
     */
    Task<void> dispatch_incoming_request(nlohmann::json json_msg) {
        // [gcc11-sso: string_view] Points into heap-allocated json_msg. DO NOT change to std::string.
        std::string_view method = json_msg.at("method").get_ref<const nlohmann::json::string_t&>();

        auto it = request_handlers_.find(method);
        if (it != request_handlers_.end()) {
            auto params = json_msg.contains("params") ? json_msg.at("params") : nlohmann::json{};
            // [gcc11-sso: scope-before-await] nlohmann::json is heap-safe. DO NOT use optional<string>.
            nlohmann::json error_payload;
            nlohmann::json result;
            try {
                result = co_await it->second(params);
            } catch (const std::exception& e) {
                error_payload = e.what();
            }
            // Re-extract id just-in-time AFTER suspension — never stored in frame as RequestId.
            if (!error_payload.is_null()) {
                co_await transport_->write_message(make_error_wire(json_msg.at("id").get<RequestId>(),
                                                                   g_INTERNAL_ERROR,
                                                                   error_payload.get<std::string>()));
            } else {
                co_await transport_->write_message(
                    make_result_wire(json_msg.at("id").get<RequestId>(), std::move(result)));
            }
        } else if (method == "ping") {
            co_await transport_->write_message(
                make_result_wire(json_msg.at("id").get<RequestId>(), nlohmann::json::object()));
        } else {
            co_await transport_->write_message(
                make_error_wire(json_msg.at("id").get<RequestId>(), g_METHOD_NOT_FOUND,
                                "Method not found: " + std::string(method)));
        }
    }

    // [gcc11-sso: wire-builders] DO NOT convert to Task<T>.
    static std::string make_result_wire(const RequestId& id, nlohmann::json result) {
        JSONRPCResultResponse response;
        response.id = id;
        response.result = std::move(result);
        return nlohmann::json(std::move(response)).dump();
    }

    static std::string make_error_wire(const RequestId& id, int code, std::string message) {
        Error error;
        error.code = code;
        error.message = std::move(message);
        JSONRPCErrorResponse response;
        response.id = id;
        response.error = std::move(error);
        return nlohmann::json(std::move(response)).dump();
    }

    std::shared_ptr<ITransport> transport_;
    boost::asio::strand<boost::asio::any_io_executor> strand_;
    std::map<std::string, PendingRequest> pending_requests_;
    std::atomic<int64_t> next_request_id_{1};

    std::map<std::string, NotificationCallback, std::less<>> notification_handlers_;
    std::map<std::string, RequestHandler, std::less<>> request_handlers_;
    std::vector<Root> roots_;
    ServerCapabilities server_capabilities_;
};

}  // namespace mcp

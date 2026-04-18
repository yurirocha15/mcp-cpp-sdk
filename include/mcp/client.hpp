#pragma once

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
    Task<InitializeResult> connect(Implementation client_info, ClientCapabilities capabilities) {
        boost::asio::co_spawn(strand_, read_loop(), boost::asio::detached);

        InitializeRequest init_req;
        init_req.protocolVersion = std::string(LATEST_PROTOCOL_VERSION);
        init_req.clientInfo = std::move(client_info);
        init_req.capabilities = std::move(capabilities);

        nlohmann::json params = init_req;
        auto result_json = co_await send_request("initialize", std::move(params));
        auto result = result_json.get<InitializeResult>();

        server_capabilities_ = result.capabilities;

        co_await send_notification("notifications/initialized", std::nullopt);

        co_return result;
    }

    /**
     * @brief Send a JSON-RPC request and await its response.
     *
     * @details Assigns a unique integer ID, sends the request over the
     * transport, then suspends the calling coroutine until the read loop
     * dispatches the matching response.
     *
     * @param method The JSON-RPC method name.
     * @param params Optional parameters for the request.
     * @return A task that resolves to the result JSON.
     * @throws std::runtime_error If the server returns an error response.
     */
    Task<nlohmann::json> send_request(std::string method, std::optional<nlohmann::json> params) {
        auto id = next_request_id_.fetch_add(1, std::memory_order_relaxed);
        auto id_str = std::to_string(id);

        JSONRPCRequest request;
        request.id = RequestId{id_str};
        request.method = std::move(method);
        request.params = std::move(params);

        // Pending entry uses a timer as a coroutine suspension mechanism:
        // set to max expiry, cancelled by read_loop when response arrives.
        auto& pending = pending_requests_[id_str];
        pending.timer = std::make_unique<boost::asio::steady_timer>(strand_);
        pending.timer->expires_at(std::chrono::steady_clock::time_point::max());

        nlohmann::json msg = request;
        co_await transport_->write_message(msg.dump());

        try {
            co_await pending.timer->async_wait(boost::asio::use_awaitable);
        } catch (const boost::system::system_error& err) {
            if (err.code() != boost::asio::error::operation_aborted) {
                throw;
            }
        }

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

    /**
     * @brief Send a JSON-RPC notification (no response expected).
     *
     * @param method The notification method name.
     * @param params Optional parameters for the notification.
     * @return A task that completes when the notification has been written to the transport.
     */
    Task<void> send_notification(std::string method, std::optional<nlohmann::json> params) {
        JSONRPCNotification notification;
        notification.method = std::move(method);
        notification.params = std::move(params);

        nlohmann::json msg = notification;
        co_await transport_->write_message(msg.dump());
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
    Task<CallToolResult> call_tool(std::string name, Args arguments) {
        CallToolParams call_params;
        call_params.name = std::move(name);
        call_params.arguments = nlohmann::json(std::move(arguments));

        nlohmann::json params = std::move(call_params);
        auto result_json = co_await send_request("tools/call", std::move(params));
        co_return result_json.template get<CallToolResult>();
    }

    /**
     * @brief List available tools from the server.
     *
     * @param cursor Optional pagination cursor.
     * @return A task that resolves to the server's ListToolsResult.
     */
    Task<ListToolsResult> list_tools(std::optional<std::string> cursor = std::nullopt) {
        std::optional<nlohmann::json> params;
        if (cursor) {
            PaginatedRequestParams paginated;
            paginated.cursor = std::move(cursor);
            nlohmann::json p = std::move(paginated);
            params = std::move(p);
        }
        auto result_json = co_await send_request("tools/list", std::move(params));
        co_return result_json.get<ListToolsResult>();
    }

    /**
     * @brief List available resources from the server.
     *
     * @param cursor Optional pagination cursor.
     * @return A task that resolves to the server's ListResourcesResult.
     */
    Task<ListResourcesResult> list_resources(std::optional<std::string> cursor = std::nullopt) {
        std::optional<nlohmann::json> params;
        if (cursor) {
            PaginatedRequestParams paginated;
            paginated.cursor = std::move(cursor);
            nlohmann::json p = std::move(paginated);
            params = std::move(p);
        }
        auto result_json = co_await send_request("resources/list", std::move(params));
        co_return result_json.get<ListResourcesResult>();
    }

    /**
     * @brief Read a resource from the server by URI.
     *
     * @param uri The URI of the resource to read.
     * @return A task that resolves to the server's ReadResourceResult.
     */
    Task<ReadResourceResult> read_resource(std::string uri) {
        ReadResourceRequestParams read_params;
        read_params.uri = std::move(uri);

        nlohmann::json params = std::move(read_params);
        auto result_json = co_await send_request("resources/read", std::move(params));
        co_return result_json.get<ReadResourceResult>();
    }

    /**
     * @brief List available resource templates from the server.
     *
     * @param cursor Optional pagination cursor.
     * @return A task that resolves to the server's ListResourceTemplatesResult.
     */
    Task<ListResourceTemplatesResult> list_resource_templates(
        std::optional<std::string> cursor = std::nullopt) {
        std::optional<nlohmann::json> params;
        if (cursor) {
            PaginatedRequestParams paginated;
            paginated.cursor = std::move(cursor);
            nlohmann::json p = std::move(paginated);
            params = std::move(p);
        }
        auto result_json = co_await send_request("resources/templates/list", std::move(params));
        co_return result_json.get<ListResourceTemplatesResult>();
    }

    /**
     * @brief List available prompts from the server.
     *
     * @param cursor Optional pagination cursor.
     * @return A task that resolves to the server's ListPromptsResult.
     */
    Task<ListPromptsResult> list_prompts(std::optional<std::string> cursor = std::nullopt) {
        std::optional<nlohmann::json> params;
        if (cursor) {
            PaginatedRequestParams paginated;
            paginated.cursor = std::move(cursor);
            nlohmann::json p = std::move(paginated);
            params = std::move(p);
        }
        auto result_json = co_await send_request("prompts/list", std::move(params));
        co_return result_json.get<ListPromptsResult>();
    }

    /**
     * @brief Get a specific prompt from the server.
     *
     * @param name The name of the prompt.
     * @param arguments Optional arguments to template the prompt.
     * @return A task that resolves to the server's GetPromptResult.
     */
    Task<GetPromptResult> get_prompt(
        std::string name, std::optional<std::map<std::string, std::string>> arguments = std::nullopt) {
        GetPromptRequestParams prompt_params;
        prompt_params.name = std::move(name);
        prompt_params.arguments = std::move(arguments);

        nlohmann::json params = std::move(prompt_params);
        auto result_json = co_await send_request("prompts/get", std::move(params));
        co_return result_json.get<GetPromptResult>();
    }

    /**
     * @brief Request completions from the server.
     *
     * @param params The completion parameters.
     * @return A task that resolves to the server's CompleteResult.
     */
    Task<CompleteResult> complete(CompleteParams params) {
        nlohmann::json json_params = std::move(params);
        auto result_json = co_await send_request("completion/complete", std::move(json_params));
        co_return result_json.get<CompleteResult>();
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
    Task<void> cancel(RequestId request_id, std::optional<std::string> reason = std::nullopt) {
        CancelledNotificationParams params;
        params.requestId = std::move(request_id);
        params.reason = std::move(reason);

        nlohmann::json json_params = std::move(params);
        co_await send_notification("notifications/cancelled", std::move(json_params));
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
    void on_notification(std::string method, NotificationCallback callback) {
        notification_handlers_[std::move(method)] = std::move(callback);
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
    void on_request(std::string method, RequestHandler handler) {
        request_handlers_[std::move(method)] = std::move(handler);
    }

    /**
     * @brief Register a handler for incoming elicitation requests from the server.
     *
     * @details Convenience wrapper that registers a request handler for "elicitation/create".
     * The handler receives ElicitRequestParams and returns ElicitResult.
     *
     * @param handler Async handler: receives ElicitRequestParams, returns ElicitResult.
     */
    void on_elicitation(std::function<Task<ElicitResult>(ElicitRequestParams)> handler) {
        on_request("elicitation/create",
                   [h = std::move(handler)](const nlohmann::json& params) -> Task<nlohmann::json> {
                       auto elicit_params = params.get<ElicitRequestParams>();
                       auto result = co_await h(std::move(elicit_params));
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
    void set_roots(std::vector<Root> roots, bool notify = false) {
        roots_ = std::move(roots);

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
    /**
     * @brief A pending request awaiting its response.
     */
    struct PendingRequest {
        std::unique_ptr<boost::asio::steady_timer> timer;  ///< Timer used for coroutine suspension.
        nlohmann::json result;                             ///< The result payload when resolved.
        std::optional<Error> error;                        ///< The error, if the request failed.
    };

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
        } catch (const std::exception&) {
            // Transport closed or error occurred, terminate loop.
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
            it->second.result = std::move(json_msg.at("result"));
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
                    it->second.error = Error{-32800, "Request cancelled by server"};
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
     * responses with code -32603 (INTERNAL_ERROR). The @c ping method is handled
     * automatically and returns an empty result object. Unknown methods return a
     * METHOD_NOT_FOUND error response. Runs on the client's asio strand
     * (co_spawned from the read loop).
     *
     * @param json_msg The raw JSON-RPC request with @c id, @c method, and optional @c params.
     */
    Task<void> dispatch_incoming_request(nlohmann::json json_msg) {
        auto id = json_msg.at("id").get<RequestId>();
        auto method = json_msg.at("method").get<std::string>();

        auto it = request_handlers_.find(method);
        if (it != request_handlers_.end()) {
            auto params = json_msg.contains("params") ? json_msg.at("params") : nlohmann::json{};
            std::optional<std::string> error_msg;
            nlohmann::json result;
            try {
                result = co_await it->second(params);
            } catch (const std::exception& e) {
                error_msg = e.what();
            }
            if (error_msg) {
                co_await send_error_response(id, -32603, std::move(*error_msg));
            } else {
                co_await send_response(id, std::move(result));
            }
        } else if (method == "ping") {
            co_await send_response(id, nlohmann::json::object());
        } else {
            co_await send_error_response(id, METHOD_NOT_FOUND, "Method not found: " + method);
        }
    }

    Task<void> send_response(const RequestId& id, nlohmann::json result) {
        JSONRPCResultResponse response;
        response.id = id;
        response.result = std::move(result);

        nlohmann::json json_msg = std::move(response);
        co_await transport_->write_message(json_msg.dump());
    }

    Task<void> send_error_response(const RequestId& id, int code, std::string message) {
        Error error;
        error.code = code;
        error.message = std::move(message);

        JSONRPCErrorResponse response;
        response.id = id;
        response.error = std::move(error);

        nlohmann::json json_msg = std::move(response);
        co_await transport_->write_message(json_msg.dump());
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

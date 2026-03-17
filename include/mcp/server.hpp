#pragma once

#include <mcp/context.hpp>
#include <mcp/core.hpp>
#include <mcp/protocol.hpp>
#include <mcp/transport.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/strand.hpp>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace mcp {

/**
 * @brief MCP server that handles client requests over a transport.
 *
 * @details Manages the JSON-RPC message loop, dispatching incoming
 * requests to the appropriate handler. Supports the MCP initialize
 * handshake and shutdown flow. Creates a Context per request for
 * handler use.
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

    ~Server() = default;

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
    Server(Server&&) = delete;
    Server& operator=(Server&&) = delete;

    /**
     * @brief Start the server session loop on the given transport.
     *
     * @details Reads messages from the transport, parses them as
     * JSON-RPC, and dispatches each to the appropriate handler.
     * The loop runs until the transport is closed or an error occurs.
     *
     * @param transport The transport to use for message exchange.
     *                  Ownership is transferred to the server.
     * @param executor The executor to use for async operations.
     */
    Task<void> run(std::unique_ptr<ITransport> transport,
                   const boost::asio::any_io_executor& executor) {
        transport_ = std::move(transport);
        strand_ = std::make_unique<boost::asio::strand<boost::asio::any_io_executor>>(
            boost::asio::make_strand(executor));

        try {
            for (;;) {
                auto raw = co_await transport_->read_message();
                auto json_msg = nlohmann::json::parse(raw);
                co_await dispatch(json_msg);
            }
        } catch (const std::exception&) {
        }
    }

    /**
     * @brief Dispatch a parsed JSON-RPC message to the appropriate handler.
     *
     * @details Routes requests based on the "method" field. Handles
     * "initialize" and "shutdown" internally. Returns a JSON-RPC
     * error response for unknown methods. Notifications (messages
     * without "id") are silently ignored.
     *
     * @param json_msg The parsed JSON-RPC message.
     */
    Task<void> dispatch(const nlohmann::json& json_msg) {
        if (!json_msg.contains("id")) {
            co_return;
        }

        auto request = json_msg.get<JSONRPCRequest>();

        if (request.method == "initialize") {
            co_await handle_initialize(request);
        } else if (request.method == "shutdown") {
            co_await handle_shutdown(request);
        } else {
            co_await send_error(request.id, METHOD_NOT_FOUND, "Method not found: " + request.method);
        }
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

   private:
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

    Task<void> send_result(const RequestId& id, nlohmann::json result) {
        JSONRPCResultResponse response;
        response.id = id;
        response.result = std::move(result);

        nlohmann::json json_msg = std::move(response);
        co_await transport_->write_message(json_msg.dump());
    }

    Task<void> send_error(const RequestId& id, int code, std::string message) {
        Error error;
        error.code = code;
        error.message = std::move(message);

        JSONRPCErrorResponse response;
        response.id = id;
        response.error = std::move(error);

        nlohmann::json json_msg = std::move(response);
        co_await transport_->write_message(json_msg.dump());
    }

    Implementation server_info_;
    ServerCapabilities capabilities_;
    std::unique_ptr<ITransport> transport_;
    std::unique_ptr<boost::asio::strand<boost::asio::any_io_executor>> strand_;
    bool initialized_ = false;
    bool shutdown_requested_ = false;
};

}  // namespace mcp

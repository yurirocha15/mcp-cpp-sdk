#pragma once

#include <mcp/transport.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <memory>
#include <string>
#include <string_view>

namespace mcp {

/**
 * @brief Server-side WebSocket transport for an accepted TCP connection.
 *
 * Takes an accepted TCP socket and handles the WebSocket protocol.
 * Use inside a server accept loop: accept a TCP socket, pass it here,
 * then use read_message() / write_message() for MCP message exchange.
 */
class WebSocketServerTransport final : public ITransport {
   public:
    /**
     * @brief Construct from an accepted TCP socket.
     *
     * @param socket An accepted TCP socket.
     */
    explicit WebSocketServerTransport(boost::asio::ip::tcp::socket socket);

    ~WebSocketServerTransport() override;

    WebSocketServerTransport(const WebSocketServerTransport&) = delete;
    WebSocketServerTransport& operator=(const WebSocketServerTransport&) = delete;
    WebSocketServerTransport(WebSocketServerTransport&&) = delete;
    WebSocketServerTransport& operator=(WebSocketServerTransport&&) = delete;

    /**
     * @brief Read the next message from the WebSocket connection.
     *
     * Throws std::runtime_error if the transport is closed or the connection drops.
     */
    Task<std::string> read_message() override;

    /**
     * @brief Send a message over the WebSocket connection.
     *
     * @param message The message to send.
     */
    Task<void> write_message(std::string_view message) override;

    /**
     * @brief Close the WebSocket connection. Safe to call multiple times.
     */
    void close() override;

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/**
 * @brief Client-side WebSocket transport.
 *
 * Connects to a WebSocket server and provides read_message() / write_message()
 * for MCP message exchange.
 */
class WebSocketClientTransport final : public ITransport {
   public:
    /**
     * @brief Construct a WebSocket client transport.
     *
     * @param executor Executor for async operations.
     * @param host     Remote hostname or IP address.
     * @param port     Remote port number.
     * @param path     WebSocket request path (default: "/").
     */
    WebSocketClientTransport(const boost::asio::any_io_executor& executor, std::string host,
                             std::string port, std::string path = "/");

    ~WebSocketClientTransport() override;

    WebSocketClientTransport(const WebSocketClientTransport&) = delete;
    WebSocketClientTransport& operator=(const WebSocketClientTransport&) = delete;
    WebSocketClientTransport(WebSocketClientTransport&&) = delete;
    WebSocketClientTransport& operator=(WebSocketClientTransport&&) = delete;

    /**
     * @brief Read the next message from the WebSocket connection.
     *
     * Connects and performs the WebSocket handshake on the first call.
     * Throws std::runtime_error if the transport is closed or the connection drops.
     */
    Task<std::string> read_message() override;

    /**
     * @brief Send a message over the WebSocket connection.
     *
     * Connects and performs the WebSocket handshake on the first call.
     *
     * @param message The message to send.
     */
    Task<void> write_message(std::string_view message) override;

    /**
     * @brief Close the WebSocket connection. Safe to call multiple times.
     */
    void close() override;

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mcp

#pragma once

#include <mcp/protocol.hpp>
#include <mcp/transport.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <memory>
#include <string>

namespace mcp {

/**
 * @brief HTTP transport implementation for MCP client message exchange.
 *
 * @details Sends JSON-RPC messages via HTTP POST to an MCP Streamable HTTP
 * endpoint and enqueues response payloads for asynchronous consumption via
 * read_message(). Supports both `application/json` and `text/event-stream`
 * response content types.
 */
class HttpClientTransport final : public ITransport {
   public:
    /**
     * @brief Construct an HTTP client transport.
     *
     * @param executor Executor used for async operations.
     * @param url HTTP endpoint URL (e.g. `http://localhost:8080/mcp`).
     *
     * @throws std::invalid_argument If the URL is malformed or does not use
     *         the `http://` scheme.
     */
    HttpClientTransport(const boost::asio::any_io_executor& executor, std::string url);

    ~HttpClientTransport() override;

    HttpClientTransport(const HttpClientTransport&) = delete;
    HttpClientTransport& operator=(const HttpClientTransport&) = delete;
    HttpClientTransport(HttpClientTransport&&) = delete;
    HttpClientTransport& operator=(HttpClientTransport&&) = delete;

    /**
     * @brief Get the currently active MCP session identifier.
     *
     * @return The session identifier captured from server responses, or an empty string if no
     * session exists.
     */
    [[nodiscard]] const std::string& session_id() const;

    /**
     * @brief Get the most recent SSE event identifier seen from the server.
     *
     * @return The last event ID used for resumable HTTP replay, or an empty string if none was
     * received.
     */
    [[nodiscard]] const std::string& last_event_id() const;

    /**
     * @brief Dequeue the next MCP message received from HTTP responses.
     *
     * @details Suspends until a queued response message is available or until
     * the transport is closed.
     *
     * @return A task resolving to the next queued message.
     * @throws std::runtime_error If the transport is closed.
     */
    Task<std::string> read_message() override;

    /**
     * @brief Send an MCP JSON-RPC message to the server via HTTP POST.
     *
     * @param message Serialized JSON-RPC message body.
     *
     * @throws std::runtime_error On transport closure or HTTP error status.
     * @throws boost::system::system_error On networking failures.
     */
    Task<void> write_message(std::string_view message) override;

    /**
     * @brief Close the transport and release HTTP connection resources.
     *
     * @details Marks the transport as closed, wakes pending readers, sends an
     * HTTP DELETE request if an MCP session is active, and closes the TCP
     * connection.
     */
    void close() override;

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mcp

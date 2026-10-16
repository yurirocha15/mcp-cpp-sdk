#pragma once

#include <mcp/core/export.hpp>
#include <mcp/protocol/protocol.hpp>
#include <mcp/transport/transport.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace mcp {

/** @brief HTTP-layer error returned by a remote MCP endpoint. */
class MCP_API HttpStatusError : public std::runtime_error {
   public:
    /**
     * @brief Construct an error for a non-success HTTP response.
     * @param status Numeric HTTP response status.
     * @param message Human-readable error description.
     * @param authenticate_challenge Optional WWW-Authenticate header value.
     */
    HttpStatusError(unsigned int status, std::string message, std::string authenticate_challenge = {})
        : std::runtime_error(std::move(message)),
          status_(status),
          authenticate_challenge_(std::move(authenticate_challenge)) {}

    /** @return Numeric HTTP response status. */
    [[nodiscard]] unsigned int status() const noexcept { return status_; }

    /** @return WWW-Authenticate header value, or an empty string when absent. */
    [[nodiscard]] const std::string& authenticate_challenge() const noexcept {
        return authenticate_challenge_;
    }

   private:
    unsigned int status_;
    std::string authenticate_challenge_;
};

/**
 * @brief HTTP transport implementation for MCP client message exchange.
 *
 * Sends JSON-RPC messages via HTTP POST to an MCP Streamable HTTP endpoint.
 * Supports both `application/json` and `text/event-stream` response content types.
 */
class MCP_API HttpClientTransport final : public ITransport {
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
    HttpClientTransport(const boost::asio::any_io_executor& executor, const std::string& url);

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
    [[nodiscard]] std::string session_id() const;

    /**
     * @brief Get the most recent SSE event identifier seen from the server.
     *
     * @return The last event ID used for resumable HTTP replay, or an empty string if none was
     * received.
     */
    [[nodiscard]] std::string last_event_id() const;

    /**
     * @brief Configure a provider for HTTP Authorization: Bearer headers.
     *
     * Configure this before starting client operations. Returning an empty string omits the header.
     */
    void set_bearer_token_provider(std::function<std::string()> provider);

    /**
     * @brief Dequeue the next MCP message received from the server.
     *
     * @throws std::runtime_error If the transport is closed.
     */
    Task<std::string> read_message() override;

    /**
     * @brief Send an MCP JSON-RPC message to the server via HTTP POST.
     *
     * @param message Serialized JSON-RPC message body.
     *
     * @throws std::runtime_error On transport closure or HTTP error.
     */
    Task<void> write_message(std::string_view message) override;

    /**
     * @brief Close the transport and release connection resources.
     */
    void close() override;

   private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

}  // namespace mcp

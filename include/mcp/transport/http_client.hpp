#pragma once

#include <mcp/core/export.hpp>
#include <mcp/protocol/protocol.hpp>
#include <mcp/transport/proxy.hpp>
#include <mcp/transport/tls.hpp>
#include <mcp/transport/transport.hpp>

#include <chrono>
#include <functional>
#include <mcp/core/executor.hpp>
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
 *
 * An `https://` URL is reached over TLS, with the server's certificate checked against the
 * system's certificate authorities (see set_tls()).
 *
 * Each message goes on an exchange of its own, so requests overlap, and an event stream is read
 * as it comes: a request the server sends while it handles one of the client's reaches the client
 * at once. In a session of protocol revision 2025-11-25 or earlier, the transport opens the
 * session's GET stream after notifications/initialized, for what the server sends outside any
 * request, and opens it again from its last event when it ends; a server that answers that GET
 * with 405 is not asked again. A request's stream that ends before its response is resumed with a
 * GET that names its last event.
 *
 * An exchange goes on a connection that an earlier one read to the end, when the server left it
 * open. Up to four such connections are kept; one that sat idle for 90 seconds, or that the server
 * has closed, is not used again.
 */
class MCP_API HttpClientTransport final : public ITransport {
   public:
    /**
     * @brief Construct an HTTP client transport.
     *
     * @param executor Executor used for async operations.
     * @param url Endpoint URL, `http://` or `https://` (e.g. `https://example.com/mcp`). An IPv6
     *        host is written in brackets.
     *
     * @throws std::invalid_argument If the URL is malformed, carries credentials, or uses
     *         another scheme.
     */
    HttpClientTransport(const Executor& executor, const std::string& url);

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
     * @brief Whether to start a new session when the server answers 404 for the current one.
     *
     * On by default. A server ends a session it has forgotten, for instance after its idle
     * timeout, and answers its requests 404; the client must then start a new one. The transport
     * sends the initialize request that began the session again, then notifications/initialized,
     * and sends the failed request once more in the new session. What the server kept for the old
     * session, such as resource subscriptions and the log level, is gone. When this is off, the
     * request fails with HttpStatusError 404. When the new session cannot be started, the request
     * fails with the error that start met, and the next request tries to start one again. When
     * the server answers the new initialize with another protocol version or other capabilities,
     * the request fails as well, since the Client still knows the server by the old ones; connect
     * again to use the new session.
     */
    void set_reinitialize_on_expired_session(bool enabled);

    /**
     * @brief Configure a provider for HTTP Authorization: Bearer headers.
     *
     * Returning an empty string omits the header. Safe to call at any time, including while
     * requests are in flight: each request pins the provider installed when it started, so a
     * request already running keeps its provider and the next one picks up the new value. With an
     * `http://` URL whose host is not a loopback one, the warning handler is told once that tokens
     * travel without TLS.
     */
    void set_bearer_token_provider(std::function<std::string()> provider);

    /**
     * @brief Configure a receiver for warnings about the connection and what the server sent.
     *
     * A tool whose `x-mcp-header` annotations break the rules of the Streamable HTTP specification
     * is left out of the result of tools/list when the client speaks protocol revision 2026-07-28;
     * the handler is told which tool and why, on the transport's executor. A bearer token provider
     * on an `http://` URL whose host is not a loopback one gets one warning, on the thread that
     * starts the first request. The handler must not block.
     */
    void set_warning_handler(std::function<void(const std::string&)> handler);

    /**
     * @brief Limit the wait for the response to a request sent to a server of an older protocol.
     *
     * Past it the connection that waits is closed and the request fails. The default is ten
     * minutes; ClientOptions::request_timeout still decides how long a caller waits. Zero, a
     * negative value, or one too long for the clock to represent disables it. Requests of protocol
     * revision 2026-07-28 have no such limit.
     */
    void set_response_timeout(std::chrono::milliseconds timeout);

    /**
     * @brief Use these TLS settings for the connections opened from now on.
     *
     * Without a call, an `https://` server's certificate is checked against the system's
     * certificate authorities.
     *
     * @throws std::invalid_argument If the URL is not `https://`.
     * @throws TlsConfigError If the options cannot be used.
     */
    void set_tls(const TlsClientOptions& options);

    /** @brief Use a shared TLS context for the connections opened from now on; see set_tls(). */
    void set_tls(std::shared_ptr<const TlsClientContext> context);

    /**
     * @brief Check the server's certificate against @p name instead of the URL's host, and send it
     * as the TLS server name. For a server reached by an address or an alias its certificate does
     * not name.
     *
     * @throws std::invalid_argument If the URL is not `https://` or @p name is empty.
     */
    void set_server_name(std::string name);

    /**
     * @brief Send the connections opened from now on through an HTTP proxy.
     *
     * Without a call the transport connects directly. Pass ProxyOptions::from_environment() to use
     * the proxy the environment names.
     *
     * @throws std::invalid_argument If a proxy URL is malformed or not supported.
     */
    void set_proxy(ProxyOptions options);

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

    /** @brief True: each message has its own exchange. */
    [[nodiscard]] bool overlapping_writes() const noexcept override { return true; }

   private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

}  // namespace mcp

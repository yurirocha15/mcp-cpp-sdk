#pragma once

#include <mcp/core/core.hpp>

#include <string>
#include <string_view>
#include <utility>

namespace mcp {

class HttpServerTransport;

/**
 * @brief A message read from a transport, with the principal that sent it.
 */
struct IncomingMessage {
    /// The complete JSON-RPC message.
    std::string message;
    /// Who sent the message, as the transport authenticated it, or empty when the transport does
    /// not authenticate its peer. Handlers see it as Context::principal().
    std::string principal;
};

/**
 * @brief Abstract transport interface for MCP message exchange.
 *
 * Implementations handle the underlying I/O mechanism (stdio, WebSocket, etc.)
 * for reading and writing JSON-RPC messages.
 */
class ITransport {
   public:
    virtual ~ITransport() = default;

    ITransport() = default;
    ITransport(const ITransport&) = delete;
    ITransport& operator=(const ITransport&) = delete;
    /**
     * @brief Move-construct the transport interface.
     *
     * @param other Transport interface state to move from.
     */
    ITransport(ITransport&& other) = default;
    /**
     * @brief Move-assign the transport interface.
     *
     * @param other Transport interface state to move from.
     * @return A reference to this transport interface.
     */
    ITransport& operator=(ITransport&& other) = default;

    /**
     * @brief Read the next serialized MCP message from the transport.
     *
     * @return A task that resolves to the next complete JSON-RPC message.
     * @throws std::runtime_error If the transport is closed while waiting.
     */
    virtual Task<std::string> read_message() = 0;

    /**
     * @brief Read the next message together with the principal that sent it.
     *
     * @details Server::run() reads through this. A transport that authenticates each message, such
     * as HttpServerTransport with a bearer token validator, overrides it so that every message
     * stays bound to its own sender, however many senders' messages are queued. A transport that
     * wraps another forwards this as well as read_message(), or its messages arrive without a
     * principal.
     *
     * @return A task that resolves to the next message; the default reads read_message() and
     *         gives it an empty principal.
     * @throws std::runtime_error If the transport is closed while waiting.
     */
    virtual Task<IncomingMessage> read_incoming() {
        auto message = co_await read_message();
        co_return IncomingMessage{std::move(message), {}};
    }

    /**
     * @brief Write a serialized MCP message to the transport.
     *
     * @param message The complete JSON-RPC message to send.
     * @return A task that completes once the message has been accepted by the transport.
     * @throws std::runtime_error If the transport is closed.
     */
    virtual Task<void> write_message(std::string_view message) = 0;

    /**
     * @brief Close the transport and release underlying resources.
     *
     * @details Implementations should wake any pending readers and make future
     * read/write operations fail fast.
     */
    virtual void close() = 0;

    /**
     * @brief The HttpServerTransport whose messages this transport carries, if any.
     *
     * @details Server::run() serves an HttpServerTransport specially: an HTTP DELETE ends the
     * session, the `Mcp-Param-*` headers of 2026-07-28 tools/call requests are checked against
     * their arguments, and the notifications of 2026-07-28 requests are not sent. A transport that
     * wraps an HttpServerTransport overrides this to return the wrapped one, so it is served the
     * same way.
     *
     * @return This transport when it is an HttpServerTransport, the wrapped one for a wrapper, and
     *         null otherwise (the default).
     */
    virtual HttpServerTransport* http_server_transport() noexcept { return nullptr; }
};

}  // namespace mcp

#pragma once

#include <mcp/core.hpp>

#include <string>
#include <string_view>

namespace mcp {

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
};

}  // namespace mcp

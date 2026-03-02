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
    ITransport(ITransport&&) = default;
    ITransport& operator=(ITransport&&) = default;

    virtual Task<std::string> read_message() = 0;

    virtual Task<void> write_message(std::string_view message) = 0;

    virtual void close() = 0;
};

}  // namespace mcp

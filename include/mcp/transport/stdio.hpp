#pragma once

#include <mcp/core/export.hpp>
#include <mcp/transport/transport.hpp>

#include <iostream>
#include <memory>
#include <string>
#include <string_view>

namespace boost::asio {
class any_io_executor;
}

namespace mcp {

/**
 * @brief Transport implementation for newline-delimited stdio communication.
 *
 * Reads JSON-RPC messages line-by-line from an input stream and writes
 * responses to an output stream. By default uses std::cin / std::cout,
 * but accepts arbitrary streams for testing.
 */
class MCP_API StdioTransport final : public ITransport {
   public:
    /**
     * @brief Construct a StdioTransport.
     *
     * @param executor The executor to use for async operations.
     * @param input    Input stream to read messages from (default: std::cin).
     * @param output   Output stream to write messages to (default: std::cout).
     *
     * @note The input and output streams must outlive the transport. Once a
     * read has started, destruction waits for the blocking input operation to
     * finish. Because a generic std::istream cannot be cancelled, callers
     * using a blocking custom stream must make it return EOF before destroying
     * the transport.
     */
    explicit StdioTransport(const boost::asio::any_io_executor& executor,
                            std::istream& input = std::cin, std::ostream& output = std::cout);

    ~StdioTransport() override;

    StdioTransport(const StdioTransport&) = delete;
    StdioTransport& operator=(const StdioTransport&) = delete;
    StdioTransport(StdioTransport&&) = delete;
    StdioTransport& operator=(StdioTransport&&) = delete;

    /**
     * @brief Read the next newline-delimited message from the input stream.
     *
     * At most one read may be outstanding at a time. A concurrent read
     * completes with std::logic_error.
     *
     * Throws std::runtime_error if the transport is closed or the input stream
     * reaches EOF.
     */
    Task<std::string> read_message() override;

    /**
     * @brief Write a message followed by a newline to the output stream.
     *
     * Concurrent calls are safe.
     *
     * @param message The message to write.
     */
    Task<void> write_message(std::string_view message) override;

    /**
     * @brief Close the transport. Safe to call multiple times.
     *
     * Wakes the asynchronous reader, but cannot interrupt a blocking operation
     * inside the supplied std::istream.
     */
    void close() override;

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mcp

#pragma once

#include <mcp/transport.hpp>

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
 * @details Reads JSON-RPC messages line-by-line from an input stream and writes
 * responses to an output stream. A dedicated reader thread blocks on the input
 * stream and posts messages into the boost::asio event loop. Writes are
 * serialized through a boost::asio::strand.
 *
 * By default uses std::cin / std::cout, but accepts arbitrary streams for
 * testing.
 */
class StdioTransport final : public ITransport {
   public:
    /**
     * @brief Construct a StdioTransport.
     *
     * @param executor The executor to use for async operations.
     * @param input    Input stream to read messages from (default: std::cin).
     * @param output   Output stream to write messages to (default: std::cout).
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
     * @details Suspends the calling coroutine until a complete line is
     * available. Throws std::runtime_error if the transport is closed or
     * the input stream reaches EOF.
     *
     * @return A task that resolves to the next message string.
     */
    Task<std::string> read_message() override;

    /**
     * @brief Write a message followed by a newline to the output stream.
     *
     * @details Writes are serialized through a strand so concurrent calls
     * are safe without a mutex.
     *
     * @param message The message to write.
     * @return A task that completes when the message has been flushed.
     */
    Task<void> write_message(std::string_view message) override;

    /**
     * @brief Close the transport.
     *
     * @details Sets the closed flag, preventing further message processing.
     * Safe to call multiple times.
     */
    void close() override;

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mcp

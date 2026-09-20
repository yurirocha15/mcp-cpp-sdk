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
 *
 * @warning On the stdio transport the output stream *is* the protocol channel.
 * With the default std::cout the application shares that channel: a stray
 * printf, a logging library whose default sink is stdout, or a dependency's
 * debug line lands between framed messages and the peer's parser rejects it.
 * The transport cannot detect this, because such writes never pass through it.
 * Applications that cannot guarantee a silent stdout should use
 * create_owning_stdout(), which moves the protocol onto a private descriptor
 * and points the process's stdout at stderr for the transport's lifetime.
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

    /**
     * @brief Construct a StdioTransport that owns the process's standard output.
     *
     * Duplicates the current standard output onto a private descriptor, writes
     * the protocol to that descriptor, and points the process's standard output
     * at standard error. Everything the application subsequently writes to
     * stdout - std::cout, printf, a logging library's default sink, a
     * dependency's debug line - is then diagnostics on stderr instead of
     * corruption in the middle of the message stream. The original standard
     * output is restored when the transport is destroyed.
     *
     * @param executor The executor to use for async operations.
     * @param input    Input stream to read messages from (default: std::cin).
     *
     * @return The transport. It must be destroyed before the process relies on
     * stdout again.
     *
     * @throws std::runtime_error if standard output cannot be duplicated or
     * redirected, or if another StdioTransport in this process already owns it.
     *
     * @note This changes process-global state, so construct the transport
     * during start-up, before other threads write to stdout, and keep at most
     * one owning transport alive at a time. On Windows the redirect covers the
     * C runtime (std::cout, printf, fwrite); code writing directly to the
     * Win32 STD_OUTPUT_HANDLE is not covered.
     */
    static std::unique_ptr<StdioTransport> create_owning_stdout(
        const boost::asio::any_io_executor& executor, std::istream& input = std::cin);

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
    struct OwnStdoutTag {};
    StdioTransport(const boost::asio::any_io_executor& executor, std::istream& input, OwnStdoutTag);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mcp

#pragma once

#include <mcp/core/async_fwd.hpp>
#include <mcp/core/executor.hpp>
#include <mcp/transport/stdio_client.hpp>
#include <mcp/transport/transport.hpp>

#include <memory>
#include <string>
#include <string_view>

namespace mcp::detail {

/**
 * @brief A stdio client transport that starts its server again, as a StdioRestartPolicy allows,
 *        when the process ends without having been asked to.
 *
 * @details The Client above it goes on with the new process and keeps its handlers:
 * - the requests still waiting for an answer when the process ended are answered with a
 *   g_CONNECTION_CLOSED error, so that their callers learn of the end at once;
 * - answers to requests the ended process sent are not written to the new one;
 * - a legacy session's initialize request and initialized notification go to the new process
 *   before anything else, and the answer to that initialize is not passed on;
 * - messages written while the process starts again wait for it.
 *
 * All of its state is used on its own strand.
 */
class RestartingStdioTransport final : public ITransport {
   public:
    /// Launches the first process.
    /// @throws std::runtime_error If it cannot be started, as StdioClientTransport does.
    RestartingStdioTransport(const Executor& executor, StdioProcessOptions process,
                             StdioRestartPolicy policy);
    ~RestartingStdioTransport() override;

    RestartingStdioTransport(const RestartingStdioTransport&) = delete;
    RestartingStdioTransport& operator=(const RestartingStdioTransport&) = delete;
    RestartingStdioTransport(RestartingStdioTransport&&) = delete;
    RestartingStdioTransport& operator=(RestartingStdioTransport&&) = delete;

    Task<std::string> read_message() override;
    Task<void> write_message(std::string_view message) override;
    void close() override;

   private:
    struct State;
    std::shared_ptr<State> state_;
};

}  // namespace mcp::detail

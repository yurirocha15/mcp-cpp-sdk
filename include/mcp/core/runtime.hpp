#pragma once

#include <mcp/core/async_fwd.hpp>
#include <mcp/core/executor.hpp>
#include <mcp/core/export.hpp>

#include <cstddef>
#include <memory>

namespace mcp {

/// Manages the asynchronous I/O event loop used by transports and servers.
class MCP_API Runtime {
   public:
    Runtime();
    ~Runtime();

    Runtime(Runtime&&) noexcept;
    Runtime& operator=(Runtime&&) noexcept;

    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    /// Run the event loop (blocks until stop() or no more work).
    /// @throws What a task given to spawn() throws, after which the loop may be run again.
    void run();

    /**
     * @brief Run the event loop on the calling thread and @p threads - 1 threads more, until stop()
     *        or no more work, and join them.
     *
     * @details A task given to spawn() that throws stops the loop; its exception is rethrown here
     * once every thread has returned.
     * @throws std::invalid_argument When @p threads is 0.
     */
    void run(std::size_t threads);

    /// Request the event loop to stop.
    void stop();

    /// The executor of the event loop, for Server::run(), Client(transport, executor), a
    /// transport's constructor or Server::set_handler_executor().
    [[nodiscard]] Executor executor();

    /**
     * @brief Start @p task on the event loop, such as
     *        `runtime.spawn(server.run(transport, runtime.executor()));`.
     *
     * @details The task runs when run() does. An exception it throws comes out of run().
     */
    void spawn(Task<void> task);

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    friend struct RuntimeAccess;
};

}  // namespace mcp

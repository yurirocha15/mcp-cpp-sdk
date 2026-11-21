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

    /// Run the event loop (blocks until stop() or no more work). Each call runs the loop again
    /// after an earlier run ended, unless start() has run it. A stop() made while nothing runs the
    /// loop makes the next run(), run(threads) or start() return without running it.
    /// @throws What a task given to spawn() throws, after which the loop may be run again.
    void run();

    /**
     * @brief Run the event loop on the calling thread and @p threads - 1 threads more, until stop()
     *        or no more work, and join them.
     *
     * @details A task given to spawn() that throws stops the loop; its exception is rethrown here
     * once every thread has returned. As with run(), each call runs the loop again after an
     * earlier run ended.
     * @throws std::invalid_argument When @p threads is 0.
     */
    void run(std::size_t threads);

    /**
     * @brief Run the event loop on @p threads new threads until stop() or the Runtime's
     *        destruction, which joins them, even while it has no work.
     *
     * @details For an executor that waits for work, such as a pool of handler threads:
     * `pool.start(4); server.set_handler_executor(pool.executor());`. An exception that a task given
     * to spawn() throws on one of these threads is not caught there, so it ends the program, as an
     * exception leaving any std::thread does. Starts no thread when a stop() came while nothing
     * ran the loop (see run()).
     * @throws std::invalid_argument When @p threads is 0.
     * @throws std::logic_error When the Runtime has been started already.
     */
    void start(std::size_t threads);

    /// Request the event loop to stop; threads start() made end once their current work does.
    /// Made while nothing runs the loop, it ends the next run(), run(threads) or start() instead.
    void stop();

    /// The executor of the event loop, for Server::run(), Client(transport, executor), a
    /// transport's constructor or Server::set_handler_executor().
    [[nodiscard]] Executor executor();

    /**
     * @brief Start @p task on the event loop, such as
     *        `runtime.spawn(server.run(transport, runtime.executor()));`.
     *
     * @details The task runs when run() does. An exception it throws comes out of run(). A
     * program that calls spawn() includes `<mcp/core/async.hpp>`, which defines Task.
     * @throws std::invalid_argument If @p task is empty.
     */
    void spawn(Task<void> task);

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    friend struct RuntimeAccess;
};

}  // namespace mcp

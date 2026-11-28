#pragma once

#include <mcp/core/async_fwd.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/io_context.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace mcp::detail {

/**
 * @brief The runs of one Server in progress, for Server::stop(), Server::bound_port() and
 *        Server::sync().
 *
 * @details Each run form registers itself for as long as it runs. Every member may be called from
 * any thread.
 */
class RunControl : public std::enable_shared_from_this<RunControl> {
   public:
    /// Keeps a run registered until it is destroyed.
    class Registration {
       public:
        Registration(std::shared_ptr<RunControl> control, std::uint64_t id);
        ~Registration();
        Registration(const Registration&) = delete;
        Registration& operator=(const Registration&) = delete;
        Registration(Registration&&) = delete;
        Registration& operator=(Registration&&) = delete;

        /// Makes the calling thread one of the run's loop threads, for run_http() with more than
        /// one: run_blocking() refuses on each of them. The thread that added the run is one.
        void add_loop_thread();

       private:
        std::shared_ptr<RunControl> control_;
        std::uint64_t id_;
    };

    /// Keeps the port of run_http() readable through bound_port() until it is destroyed.
    class BoundPort {
       public:
        BoundPort(RunControl& control, std::uint16_t port);
        ~BoundPort();
        BoundPort(const BoundPort&) = delete;
        BoundPort& operator=(const BoundPort&) = delete;
        BoundPort(BoundPort&&) = delete;
        BoundPort& operator=(BoundPort&&) = delete;

       private:
        RunControl& control_;
    };

    /**
     * @brief Registers a run that @p stop ends.
     *
     * @param stop Called by stop_all() with the control's mutex held, so it must only start the
     *        end, such as by posting to the run's loop, and never wait for it.
     * @param loop The event loop that run_stdio() or run_http() owns and runs on the calling
     *        thread, for run_blocking(); none for run().
     */
    [[nodiscard]] Registration add(std::function<void()> stop,
                                   std::optional<boost::asio::any_io_executor> loop = std::nullopt);

    /// Starts the end of every run in progress; does nothing when none is.
    void stop_all();

    /**
     * @brief Runs the coroutine @p start makes on the loop of run_stdio() or run_http() and waits
     *        for it, rethrowing what it throws.
     *
     * @details Returns at once when the server is not running, and without waiting further when the
     * run ends before the coroutine has run.
     * @throws std::logic_error When called on one of that loop's threads, or when the server runs only
     *         through run(), whose executor this cannot tell a waiting thread apart from.
     */
    void run_blocking(const std::function<Task<void>()>& start);

    /// The port run_http() listens on while it runs; 0 otherwise.
    [[nodiscard]] std::uint16_t port() const { return port_.load(std::memory_order_acquire); }

   private:
    struct Run {
        std::function<void()> stop;
        std::optional<boost::asio::any_io_executor> loop;
        /// The threads that run the loop; the one that added the run, then those it added.
        std::vector<std::thread::id> threads;
    };

    void remove(std::uint64_t id);
    void add_thread(std::uint64_t id);

    std::mutex mutex_;
    std::map<std::uint64_t, Run> runs_;
    std::uint64_t next_id_ = 0;
    std::atomic<std::uint16_t> port_{0};
};

/**
 * @brief Stops an io_context that has not run out of work by a deadline.
 *
 * @details The wait runs on a thread of its own, so it is not work of the io_context: its run()
 * returns as soon as the work it has is done, and the deadline only cuts off work that would
 * otherwise go on, such as a handler that never returns. Destroy it, or call finish(), before the
 * io_context.
 */
class ShutdownDeadline {
   public:
    ShutdownDeadline() = default;
    ~ShutdownDeadline();
    ShutdownDeadline(const ShutdownDeadline&) = delete;
    ShutdownDeadline& operator=(const ShutdownDeadline&) = delete;
    ShutdownDeadline(ShutdownDeadline&&) = delete;
    ShutdownDeadline& operator=(ShutdownDeadline&&) = delete;

    /// Stops @p io_ctx after @p timeout unless finish() comes first. Calls after the first do
    /// nothing.
    void start(boost::asio::io_context& io_ctx, std::chrono::milliseconds timeout);

    /// Ends the wait, without stopping the io_context, and joins its thread.
    void finish();

   private:
    std::mutex mutex_;
    std::condition_variable finished_changed_;
    bool finished_ = false;
    std::thread thread_;
};

/**
 * @brief Runs @p work on a thread of its own that nothing joins.
 *
 * @details The thread begins @p work only once it is detached, so that it cannot end during
 * pthread_detach(): some glibc versions' pthread_detach() reads the thread's state after marking it
 * detached, and a detached thread that has ended has freed that state (glibc bug 19951).
 */
template <typename Work>
void run_detached(Work work) {
    std::promise<void> detached;
    std::thread thread([work = std::move(work), ready = detached.get_future()]() mutable {
        ready.wait();
        work();
    });
    thread.detach();
    detached.set_value();
}

/**
 * @brief Destroys @p io_ctx, whose run has ended, without waiting for handlers on another
 *        executor.
 *
 * @details Destroying an execution context waits for the handlers on other contexts that its
 * coroutines still wait for (see LoopGate in handler_pool.hpp), and those remain only when a
 * deadline stopped the run. @p io_ctx is then destroyed on a thread of its own, so that
 * run_stdio() and run_http() return at the deadline, and otherwise here.
 */
void destroy_loop(std::unique_ptr<boost::asio::io_context> io_ctx);

}  // namespace mcp::detail

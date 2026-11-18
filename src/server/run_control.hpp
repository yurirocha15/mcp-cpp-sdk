#pragma once

#include <mcp/core/async_fwd.hpp>

#include <boost/asio/any_io_executor.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

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
     * @throws std::logic_error When called on that loop's thread, or when the server runs only
     *         through run(), whose executor this cannot tell a waiting thread apart from.
     */
    void run_blocking(const std::function<Task<void>()>& start);

    /// The port run_http() listens on while it runs; 0 otherwise.
    [[nodiscard]] std::uint16_t port() const { return port_.load(std::memory_order_acquire); }

   private:
    struct Run {
        std::function<void()> stop;
        std::optional<boost::asio::any_io_executor> loop;
        std::thread::id thread;
    };

    void remove(std::uint64_t id);

    std::mutex mutex_;
    std::map<std::uint64_t, Run> runs_;
    std::uint64_t next_id_ = 0;
    std::atomic<std::uint16_t> port_{0};
};

}  // namespace mcp::detail

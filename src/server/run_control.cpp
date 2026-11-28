#include "mcp/detail/server/run_control.hpp"

#include "handler_pool.hpp"

#include <mcp/core/async.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/use_future.hpp>

#include <algorithm>
#include <future>
#include <stdexcept>
#include <thread>
#include <utility>

namespace mcp::detail {

RunControl::Registration::Registration(std::shared_ptr<RunControl> control, std::uint64_t id)
    : control_(std::move(control)), id_(id) {}

RunControl::Registration::~Registration() { control_->remove(id_); }

void RunControl::Registration::add_loop_thread() { control_->add_thread(id_); }

RunControl::BoundPort::BoundPort(RunControl& control, std::uint16_t port) : control_(control) {
    control_.port_.store(port, std::memory_order_release);
}

RunControl::BoundPort::~BoundPort() { control_.port_.store(0, std::memory_order_release); }

RunControl::Registration RunControl::add(std::function<void()> stop,
                                         std::optional<boost::asio::any_io_executor> loop) {
    std::scoped_lock lock(mutex_);
    const auto id = next_id_++;
    runs_.emplace(
        id,
        Run{.stop = std::move(stop), .loop = std::move(loop), .threads = {std::this_thread::get_id()}});
    return {shared_from_this(), id};
}

void RunControl::remove(std::uint64_t id) {
    std::scoped_lock lock(mutex_);
    runs_.erase(id);
}

void RunControl::add_thread(std::uint64_t id) {
    std::scoped_lock lock(mutex_);
    const auto run = runs_.find(id);
    if (run != runs_.end()) {
        run->second.threads.push_back(std::this_thread::get_id());
    }
}

void RunControl::stop_all() {
    // The mutex stays held while each run is told to end, so that no run's loop is destroyed
    // in between: a run deregisters, under the same mutex, before its loop goes.
    std::scoped_lock lock(mutex_);
    for (const auto& [id, run] : runs_) {
        run.stop();
    }
}

void RunControl::run_blocking(const std::function<Task<void>()>& start) {
    std::future<void> done;
    {
        std::scoped_lock lock(mutex_);
        const Run* owned = nullptr;
        for (const auto& [id, run] : runs_) {
            if (run.loop) {
                owned = &run;
                break;
            }
        }
        if (owned == nullptr) {
            if (runs_.empty()) {
                return;
            }
            throw std::logic_error(
                "mcp::Server: blocking calls need run_stdio() or run_http(); a server run with "
                "run() or served by a StreamableHttpSessionManager is used through its coroutines");
        }
        if (std::ranges::find(owned->threads, std::this_thread::get_id()) != owned->threads.end()) {
            throw std::logic_error(
                "mcp::Server: a blocking call on the server's own event loop thread would wait for "
                "itself; await the coroutine instead");
        }
        // Spawned with the mutex held, so the loop cannot be destroyed first. The coroutine is made
        // on the loop as well, so that no part of the call runs on this thread.
        done = boost::asio::co_spawn(
            *owned->loop, [start]() -> boost::asio::awaitable<void> { co_await start(); },
            boost::asio::use_future);
    }
    try {
        done.get();
    } catch (const std::future_error&) {
        // The run ended, and its loop was destroyed, before the coroutine ran.
    }
}

ShutdownDeadline::~ShutdownDeadline() { finish(); }

void ShutdownDeadline::start(boost::asio::io_context& io_ctx, std::chrono::milliseconds timeout) {
    const std::scoped_lock lock(mutex_);
    if (thread_.joinable() || finished_) {
        return;
    }
    thread_ = std::thread([this, &io_ctx, timeout] {
        std::unique_lock wait_lock(mutex_);
        if (!finished_changed_.wait_for(wait_lock, timeout, [this] { return finished_; })) {
            io_ctx.stop();
        }
    });
}

void ShutdownDeadline::finish() {
    {
        const std::scoped_lock lock(mutex_);
        finished_ = true;
    }
    finished_changed_.notify_all();
    if (thread_.joinable() && thread_.get_id() != std::this_thread::get_id()) {
        thread_.join();
    }
}

void destroy_loop(std::unique_ptr<boost::asio::io_context> io_ctx) {
    if (awaits_elsewhere(io_ctx->get_executor())) {
        run_detached([io_ctx = std::move(io_ctx)]() mutable { io_ctx.reset(); });
    }
}

}  // namespace mcp::detail

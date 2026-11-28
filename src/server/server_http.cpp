#include "mcp/detail/server/run_control.hpp"
#include "mcp/detail/transport/manager_run.hpp"

#include <mcp/core/async.hpp>
#include <mcp/core/constants.hpp>
#include <mcp/server/http_run_options.hpp>
#include <mcp/server/server.hpp>
#include <mcp/transport/http_session_manager.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/strand.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace mcp {

namespace {

using LoopStrand = boost::asio::strand<boost::asio::io_context::executor_type>;

/// What one run_http() call shares between the coroutines that serve and shut down. The members
/// after `strand` are touched on the strand only.
struct HttpRun {
    HttpRun(HttpRunOptions options_arg, std::shared_ptr<StreamableHttpSessionManager> manager_arg,
            boost::asio::io_context& io_ctx_arg, LoopStrand strand_arg,
            boost::asio::signal_set& signals_arg, detail::ShutdownDeadline& deadline_arg)
        : options(std::move(options_arg)),
          manager(std::move(manager_arg)),
          io_ctx(io_ctx_arg),
          strand(std::move(strand_arg)),
          signals(signals_arg),
          deadline(deadline_arg) {}

    const HttpRunOptions options;
    const std::shared_ptr<StreamableHttpSessionManager> manager;
    boost::asio::io_context& io_ctx;
    /// Runs the coroutines of the run, and the signal handler, one at a time.
    LoopStrand strand;
    boost::asio::signal_set& signals;
    detail::ShutdownDeadline& deadline;

    bool shutting_down{false};
    /// The first failure of the run, which run_http() rethrows once the shutdown has run.
    std::exception_ptr failure;

    void record(std::exception_ptr error) {
        if (error && !failure) {
            failure = std::move(error);
        }
    }
};

/// Runs the shutdown of @p run, which every cause starts the same way, once.
Task<void> shut_down(std::shared_ptr<HttpRun> run) {
    const auto& options = run->options;
    const auto began = std::chrono::steady_clock::now();
    const auto drain_timeout = options.drain_timeout.value_or(options.shutdown_timeout / 2);

    if (options.on_drain_start) {
        try {
            options.on_drain_start();
        } catch (...) {
            run->record(std::current_exception());
        }
    }

    auto first_wait = run->manager->drain(drain_timeout);
    co_await std::move(first_wait);

    if (options.on_shutdown) {
        std::exception_ptr failure;
        try {
            auto work = options.on_shutdown();
            co_await std::move(work);
        } catch (...) {
            failure = std::current_exception();
        }
        run->record(std::move(failure));
    }

    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - began);
    const auto remaining =
        std::max(std::chrono::milliseconds::zero(), options.shutdown_timeout - elapsed);
    auto second_wait = run->manager->drain(remaining);
    co_await std::move(second_wait);

    run->signals.cancel();
    run->manager->close();
}

/// Starts the shutdown of @p run unless it has started: the hard deadline begins here.
void begin_shutdown(const std::shared_ptr<HttpRun>& run) {
    boost::asio::post(run->strand, [run] {
        if (run->shutting_down) {
            return;
        }
        run->shutting_down = true;
        run->deadline.start(run->io_ctx, run->options.shutdown_timeout);
        boost::asio::co_spawn(run->strand, shut_down(run), boost::asio::detached);
    });
}

/// Starts what the run serves, then listens until the listener ends, which starts the shutdown.
Task<void> serve(std::shared_ptr<HttpRun> run) {
    bool started = true;
    if (run->options.on_start) {
        try {
            auto starting = run->options.on_start(*run->manager);
            co_await std::move(starting);
        } catch (...) {
            // A shutdown that began while on_start ran ends the run normally, also when on_start
            // fails because the shutdown closed what it waited on.
            if (!run->shutting_down) {
                run->record(std::current_exception());
            }
            started = false;
        }
    }
    if (started && !run->shutting_down) {
        try {
            auto listening = run->manager->listen();
            co_await std::move(listening);
        } catch (...) {
            run->record(std::current_exception());
        }
    }
    // The listener is gone, whether it was closed or failed; nothing new can arrive.
    run->signals.cancel();
    begin_shutdown(run);
}

/// The loop threads a run starts besides the calling one, stopped and joined when it ends.
class LoopThreads {
   public:
    LoopThreads(boost::asio::io_context& io_ctx, detail::RunControl::Registration& registration,
                std::size_t count)
        : io_ctx_(io_ctx) {
        threads_.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            threads_.emplace_back([this, &registration] {
                registration.add_loop_thread();
                try {
                    io_ctx_.run();
                } catch (...) {
                    const std::scoped_lock lock(mutex_);
                    if (!failure_) {
                        failure_ = std::current_exception();
                    }
                    io_ctx_.stop();
                }
            });
        }
    }

    ~LoopThreads() {
        io_ctx_.stop();
        join();
    }

    LoopThreads(const LoopThreads&) = delete;
    LoopThreads& operator=(const LoopThreads&) = delete;
    LoopThreads(LoopThreads&&) = delete;
    LoopThreads& operator=(LoopThreads&&) = delete;

    /// Waits for the threads to end, which they do once the loop runs out of work or is stopped.
    void join() {
        for (auto& thread : threads_) {
            if (thread.joinable()) {
                thread.join();
            }
        }
    }

    [[nodiscard]] std::exception_ptr failure() {
        const std::scoped_lock lock(mutex_);
        return failure_;
    }

   private:
    boost::asio::io_context& io_ctx_;
    std::vector<std::thread> threads_;
    std::mutex mutex_;
    std::exception_ptr failure_;
};

void check_options(const HttpRunOptions& options) {
    if (options.io_threads == 0) {
        throw std::invalid_argument("HttpRunOptions::io_threads must be at least 1");
    }
    if (options.shutdown_timeout < std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("HttpRunOptions::shutdown_timeout must not be negative");
    }
    if (options.drain_timeout) {
        if (*options.drain_timeout < std::chrono::milliseconds::zero()) {
            throw std::invalid_argument("HttpRunOptions::drain_timeout must not be negative");
        }
        if (*options.drain_timeout > options.shutdown_timeout) {
            throw std::invalid_argument(
                "HttpRunOptions::drain_timeout must not be longer than shutdown_timeout");
        }
        if (options.on_shutdown && *options.drain_timeout == options.shutdown_timeout) {
            throw std::invalid_argument(
                "HttpRunOptions::drain_timeout must be shorter than shutdown_timeout when on_shutdown "
                "is set");
        }
    }
}

}  // namespace

void Server::run_http(const std::string& host, uint16_t port,
                      const std::function<void(StreamableHttpSessionManager&)>& configure) {
    HttpRunOptions options;
    options.configure = configure;
    run_http(host, port, options);
}

void Server::run_http(const std::string& host, uint16_t port, const HttpRunOptions& options) {
    check_options(options);

    auto loop = std::make_unique<boost::asio::io_context>();
    std::exception_ptr ep;
    {
        // What the run uses is destroyed at the end of this block, before its loop, which
        // detail::destroy_loop() destroys.
        boost::asio::io_context& io_ctx = *loop;
        const Executor executor(io_ctx.get_executor());

        auto manager = std::make_shared<StreamableHttpSessionManager>(executor, host, port, *this);
        detail::leave_stop_to_run(*manager);
        if (options.configure) {
            options.configure(*manager);
        }

        // SIGINT, SIGTERM and stop() end the run the same way, once: the manager drains, the
        // requests being served are answered, and the loop runs until the handlers still running
        // have finished, or until the deadline stops it.
        detail::ShutdownDeadline deadline;
        auto strand = boost::asio::make_strand(io_ctx);
        boost::asio::signal_set signals(strand, SIGINT, SIGTERM);
        const auto run = std::make_shared<HttpRun>(options, manager, io_ctx, strand, signals, deadline);
        signals.async_wait([run](const boost::system::error_code& ec, int) {
            if (ec == boost::asio::error::operation_aborted) {
                return;
            }
            begin_shutdown(run);
        });
        const auto control = run_control();
        auto registration = control->add([run] { begin_shutdown(run); }, executor.get());
        const detail::RunControl::BoundPort bound(*control, manager->port());

        boost::asio::co_spawn(run->strand, serve(run), boost::asio::detached);

        {
            LoopThreads threads(io_ctx, registration, options.io_threads - 1);
            io_ctx.run();
            threads.join();
            ep = threads.failure();
        }
        deadline.finish();
        if (run->failure) {
            ep = run->failure;
        }
    }
    detail::destroy_loop(std::move(loop));

    if (ep) {
        std::rethrow_exception(ep);
    }
}

}  // namespace mcp

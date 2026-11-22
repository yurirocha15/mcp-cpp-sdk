#include <mcp/core/async.hpp>
#include <mcp/core/runtime.hpp>
#include "../detail/async/runtime_access.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>

#include <cstddef>
#include <exception>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace mcp {

struct Runtime::Impl {
    Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    ~Impl() {
        release();
        io_ctx->stop();
        for (auto& thread : threads) {
            // A Runtime destroyed by a task on one of its own threads cannot wait for that thread,
            // which goes on in io_context::run() once the task returns. That thread shares the
            // io_context, so the io_context is destroyed when the thread ends instead.
            if (thread.get_id() == std::this_thread::get_id()) {
                thread.detach();
            } else {
                thread.join();
            }
        }
    }

    /// Lets run() and the threads of start() return once the loop has no work.
    void release() {
        std::lock_guard lock(mutex);
        keep_running.reset();
    }

    /// Begins a call of run(). Returns false, for a call that returns without running the loop,
    /// when a stop() came while nothing ran it.
    bool enter_run() {
        std::lock_guard lock(mutex);
        if (stop_pending) {
            stop_pending = false;
            return false;
        }
        restart_ended_loop();
        ++runs;
        return true;
    }

    void leave_run() {
        std::lock_guard lock(mutex);
        --runs;
    }

    /// io_context::run() returns at once after the loop ran out of work, was stopped or was
    /// stopped by a task that threw, until it is restarted, which Asio allows only while nothing
    /// runs it. The threads of start() run it until they are joined, so it is left as it is then.
    /// Called with mutex held.
    void restart_ended_loop() {
        if (runs == 0 && threads.empty() && io_ctx->stopped()) {
            io_ctx->restart();
        }
    }

    /// Shared with the threads of start(); see the destructor.
    std::shared_ptr<boost::asio::io_context> io_ctx = std::make_shared<boost::asio::io_context>();
    std::mutex mutex;
    std::optional<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>
        keep_running;
    std::vector<std::thread> threads;
    /// The calls of run() in progress.
    std::size_t runs = 0;
    /// A stop() came while nothing ran the loop; the next run() or start() returns without
    /// running it.
    bool stop_pending = false;

    /// Counts a call of run() while it runs the loop.
    class Running {
       public:
        explicit Running(Impl& impl) : impl_(impl) {}
        Running(const Running&) = delete;
        Running& operator=(const Running&) = delete;
        Running(Running&&) = delete;
        Running& operator=(Running&&) = delete;
        ~Running() { impl_.leave_run(); }

       private:
        Impl& impl_;
    };
};

Runtime::Runtime() : impl_(std::make_unique<Impl>()) {}
Runtime::~Runtime() = default;
Runtime::Runtime(Runtime&&) noexcept = default;
Runtime& Runtime::operator=(Runtime&&) noexcept = default;

void Runtime::run() {
    if (!impl_->enter_run()) {
        return;
    }
    const Impl::Running running(*impl_);
    impl_->io_ctx->run();
}

void Runtime::run(std::size_t threads) {
    if (threads == 0) {
        throw std::invalid_argument("mcp::Runtime::run: threads must be at least 1");
    }
    if (!impl_->enter_run()) {
        return;
    }
    const Impl::Running running(*impl_);
    auto& io_ctx = *impl_->io_ctx;
    std::mutex failure_mutex;
    std::exception_ptr failure;
    const auto run_loop = [&io_ctx, &failure_mutex, &failure] {
        try {
            io_ctx.run();
        } catch (...) {
            {
                std::lock_guard lock(failure_mutex);
                if (!failure) {
                    failure = std::current_exception();
                }
            }
            io_ctx.stop();
        }
    };
    std::vector<std::thread> workers;
    workers.reserve(threads - 1);
    for (std::size_t i = 1; i < threads; ++i) {
        workers.emplace_back(run_loop);
    }
    run_loop();
    for (auto& worker : workers) {
        worker.join();
    }
    if (failure) {
        std::rethrow_exception(failure);
    }
}

void Runtime::start(std::size_t threads) {
    if (threads == 0) {
        throw std::invalid_argument("mcp::Runtime::start: threads must be at least 1");
    }
    std::lock_guard lock(impl_->mutex);
    if (!impl_->threads.empty()) {
        throw std::logic_error("mcp::Runtime::start: the runtime has been started already");
    }
    if (impl_->stop_pending) {
        impl_->stop_pending = false;
        return;
    }
    impl_->restart_ended_loop();
    impl_->keep_running.emplace(impl_->io_ctx->get_executor());
    impl_->threads.reserve(threads);
    for (std::size_t i = 0; i < threads; ++i) {
        impl_->threads.emplace_back([loop = impl_->io_ctx] { loop->run(); });
    }
}

void Runtime::stop() {
    std::lock_guard lock(impl_->mutex);
    impl_->keep_running.reset();
    if (impl_->runs == 0 && impl_->threads.empty()) {
        impl_->stop_pending = true;
    }
    impl_->io_ctx->stop();
}

Executor Runtime::executor() { return impl_->io_ctx->get_executor(); }

void Runtime::spawn(Task<void> task) {
    if (!task.valid()) {
        throw std::invalid_argument("mcp::Runtime::spawn() needs a coroutine; the Task is empty");
    }
    boost::asio::co_spawn(*impl_->io_ctx, std::move(task), [](const std::exception_ptr& failure) {
        if (failure) {
            std::rethrow_exception(failure);
        }
    });
}

struct RuntimeAccess {
    static boost::asio::any_io_executor get_executor(Runtime& r) {
        return r.impl_->io_ctx->get_executor();
    }
};

namespace detail {

boost::asio::any_io_executor get_executor(Runtime& runtime) {
    return RuntimeAccess::get_executor(runtime);
}

}  // namespace detail
}  // namespace mcp

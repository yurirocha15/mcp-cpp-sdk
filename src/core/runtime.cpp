#include <mcp/core/async.hpp>
#include <mcp/core/runtime.hpp>
#include <mcp/detail/runtime_access.hpp>

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

    /// Readies a loop that has ended for run(): io_context::run() returns at once after the loop
    /// ran out of work, was stopped or was stopped by a task that threw, until it is restarted.
    /// The threads of start() run it until they are joined, so it is left as it is then.
    void ready_to_run() {
        std::lock_guard lock(mutex);
        if (threads.empty() && io_ctx->stopped()) {
            io_ctx->restart();
        }
    }

    /// Shared with the threads of start(); see the destructor.
    std::shared_ptr<boost::asio::io_context> io_ctx = std::make_shared<boost::asio::io_context>();
    std::mutex mutex;
    std::optional<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>
        keep_running;
    std::vector<std::thread> threads;
};

Runtime::Runtime() : impl_(std::make_unique<Impl>()) {}
Runtime::~Runtime() = default;
Runtime::Runtime(Runtime&&) noexcept = default;
Runtime& Runtime::operator=(Runtime&&) noexcept = default;

void Runtime::run() {
    impl_->ready_to_run();
    impl_->io_ctx->run();
}

void Runtime::run(std::size_t threads) {
    if (threads == 0) {
        throw std::invalid_argument("mcp::Runtime::run: threads must be at least 1");
    }
    impl_->ready_to_run();
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
    impl_->keep_running.emplace(impl_->io_ctx->get_executor());
    impl_->threads.reserve(threads);
    for (std::size_t i = 0; i < threads; ++i) {
        impl_->threads.emplace_back([loop = impl_->io_ctx] { loop->run(); });
    }
}

void Runtime::stop() {
    impl_->release();
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

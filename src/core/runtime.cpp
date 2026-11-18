#include <mcp/core/async.hpp>
#include <mcp/core/runtime.hpp>
#include <mcp/detail/runtime_access.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>

#include <cstddef>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace mcp {

struct Runtime::Impl {
    boost::asio::io_context io_ctx;
};

Runtime::Runtime() : impl_(std::make_unique<Impl>()) {}
Runtime::~Runtime() = default;
Runtime::Runtime(Runtime&&) noexcept = default;
Runtime& Runtime::operator=(Runtime&&) noexcept = default;

void Runtime::run() { impl_->io_ctx.run(); }

void Runtime::run(std::size_t threads) {
    if (threads == 0) {
        throw std::invalid_argument("mcp::Runtime::run: threads must be at least 1");
    }
    auto& io_ctx = impl_->io_ctx;
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

void Runtime::stop() { impl_->io_ctx.stop(); }

Executor Runtime::executor() { return impl_->io_ctx.get_executor(); }

void Runtime::spawn(Task<void> task) {
    boost::asio::co_spawn(impl_->io_ctx, std::move(task), [](const std::exception_ptr& failure) {
        if (failure) {
            std::rethrow_exception(failure);
        }
    });
}

struct RuntimeAccess {
    static boost::asio::any_io_executor get_executor(Runtime& r) {
        return r.impl_->io_ctx.get_executor();
    }
};

namespace detail {

boost::asio::any_io_executor get_executor(Runtime& runtime) {
    return RuntimeAccess::get_executor(runtime);
}

}  // namespace detail
}  // namespace mcp

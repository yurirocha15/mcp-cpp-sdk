#pragma once

#include "strand_executor.hpp"

#include <mcp/core/async.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/associated_executor.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/execution/context.hpp>
#include <boost/asio/execution/outstanding_work.hpp>
#include <boost/asio/execution_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/prefer.hpp>
#include <boost/asio/query.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace mcp::detail {

/**
 * @brief Whether the execution context a coroutine waits on is still there, for work that hands
 *        that coroutine its result from another executor.
 *
 * @details One per execution context, which closes it as it is destroyed: a service is shut down
 * before the context's handlers are destroyed, and those of a context such as an io_context go
 * with the context.
 */
class LoopGate final : public boost::asio::execution_context::service {
   public:
    // Asio finds the service by this name.
    static inline boost::asio::execution_context::id id;  // NOLINT(readability-identifier-naming)

    struct State {
        std::mutex mutex;
        bool open = true;  // guarded by mutex
    };

    explicit LoopGate(boost::asio::execution_context& context)
        : boost::asio::execution_context::service(context) {}

    [[nodiscard]] const std::shared_ptr<State>& state() const { return state_; }

   private:
    void shutdown() override {
        std::lock_guard lock(state_->mutex);
        state_->open = false;
    }

    std::shared_ptr<State> state_ = std::make_shared<State>();
};

/// Keeps @p object, which can be neither used nor destroyed any more, for the rest of the program.
template <typename Object>
void keep_forever(Object object) {
    static std::mutex s_mutex;
    // Never destroyed, so what it holds is still reachable when a leak checker looks at exit.
    static auto* const s_kept = new std::vector<void*>();
    auto* held = new Object(std::move(object));
    std::lock_guard lock(s_mutex);
    s_kept->push_back(held);
}

/// The completion of the task run_on_pool() starts: resumes the waiting coroutine on its own
/// executor while that executor's context is there.
template <typename Handler, typename T>
class PoolReturn {
   public:
    PoolReturn(std::shared_ptr<LoopGate::State> gate, Handler handler,
               const boost::asio::any_io_executor& loop)
        : gate_(std::move(gate)),
          handler_(std::move(handler)),
          loop_(boost::asio::prefer(loop, boost::asio::execution::outstanding_work_t::tracked)) {}

    PoolReturn(PoolReturn&& other) noexcept
        : gate_(std::move(other.gate_)),
          handler_(std::move(other.handler_)),
          loop_(std::move(other.loop_)) {
        other.handler_.reset();
    }
    PoolReturn(const PoolReturn&) = delete;
    PoolReturn& operator=(const PoolReturn&) = delete;
    PoolReturn& operator=(PoolReturn&&) = delete;

    /// Destroyed without having been called when the pool's executor ends before the task does,
    /// such as when the Runtime that runs it is destroyed: the waiting coroutine gets an error.
    ~PoolReturn() {
        if (!handler_) {
            return;
        }
        resume(std::make_exception_ptr(
            std::runtime_error("The handler executor ended before the handler returned")));
    }

    void operator()(std::exception_ptr error)
        requires std::is_void_v<T>
    {
        resume(std::move(error));
    }

    void operator()(std::exception_ptr error, T value)
        requires(!std::is_void_v<T>)
    {
        resume(std::move(error), std::move(value));
    }

   private:
    template <typename... Values>
    void resume(std::exception_ptr error, Values... values) {
        std::lock_guard lock(gate_->mutex);
        if (!gate_->open) {
            // The waiting coroutine's context is gone. Its frames belonged to that context and
            // can be neither destroyed after it nor resumed.
            keep_forever(std::move(*handler_));
            handler_.reset();
            keep_forever(std::move(loop_));
            return;
        }
        if constexpr (std::is_void_v<T>) {
            boost::asio::post(loop_, [handler = std::move(*handler_), error]() mutable {
                std::move(handler)(error);
            });
        } else {
            T result{};
            if constexpr (sizeof...(Values) == 1) {
                result = std::move(values...);
            }
            boost::asio::post(
                loop_, [handler = std::move(*handler_), error, result = std::move(result)]() mutable {
                    std::move(handler)(error, std::move(result));
                });
        }
        handler_.reset();
        // Done under the gate as well: ending the work reaches the context too.
        loop_ = boost::asio::any_io_executor();
    }

    std::shared_ptr<LoopGate::State> gate_;
    std::optional<Handler> handler_;
    /// The waiting coroutine's executor, which this keeps from running out of work.
    boost::asio::any_io_executor loop_;
};

/// The initiation of run_on_pool().
struct PoolInitiation {
    template <typename Handler, typename T>
    void operator()(Handler&& handler, const boost::asio::any_io_executor& pool, Task<T> task) const {
        boost::asio::any_io_executor loop = boost::asio::get_associated_executor(handler);
        auto gate = boost::asio::use_service<LoopGate>(
                        boost::asio::query(loop, boost::asio::execution::context))
                        .state();
        // The strand is made here, so that only the pool's side holds it: the waiting coroutine
        // may resume after the pool's context is gone, and a strand cannot be destroyed then.
        boost::asio::co_spawn(make_strand_executor(pool), std::move(task),
                              PoolReturn<std::decay_t<Handler>, T>(
                                  std::move(gate), std::forward<Handler>(handler), loop));
    }
};

/**
 * @brief co_spawn(strand of pool, task, use_awaitable) for a tool handler on
 *        Server::set_handler_executor()'s executor.
 *
 * @details The task returns to the awaiting coroutine through that coroutine's execution context
 * only while the context exists. run_stdio() and run_http() destroy theirs once the shutdown
 * deadline passes, and a handler on the pool may still be running then; when it returns, or when
 * the pool's executor ends first, the coroutine that waited for it is kept, unresumed, instead.
 */
template <typename T>
Task<T> run_on_pool(const boost::asio::any_io_executor& pool, Task<T> task) {
    if constexpr (std::is_void_v<T>) {
        return boost::asio::async_initiate<const boost::asio::use_awaitable_t<>&,
                                           void(std::exception_ptr)>(
            PoolInitiation{}, boost::asio::use_awaitable, pool, std::move(task));
    } else {
        return boost::asio::async_initiate<const boost::asio::use_awaitable_t<>&,
                                           void(std::exception_ptr, T)>(
            PoolInitiation{}, boost::asio::use_awaitable, pool, std::move(task));
    }
}

}  // namespace mcp::detail

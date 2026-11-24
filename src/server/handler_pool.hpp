#pragma once

#include "mcp/detail/async/strand_executor.hpp"

#include <mcp/core/async.hpp>
#include <mcp/core/context.hpp>

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

#include <condition_variable>
#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace mcp::detail {

/**
 * @brief Whether the execution context a coroutine waits on still takes results, for work that
 *        hands that coroutine its result from another executor.
 *
 * @details One per execution context. Its shutdown, which comes before the context's handlers are
 * destroyed, closes the gate, ends the Context calls under way on the context for coroutines of
 * other contexts, and then waits for the work that other contexts run for this context's
 * coroutines: that work holds them, and they are destroyed as the context's other handlers are
 * once it returns. A context on an I/O completion port would wait for that work as well, since a
 * waiting coroutine tracks work on its context, so every platform waits the same way.
 */
class LoopGate final : public boost::asio::execution_context::service {
   public:
    // Asio finds the service by this name; its constructor does nothing.
    // NOLINTNEXTLINE(readability-identifier-naming,bugprone-throwing-static-initialization)
    static inline boost::asio::execution_context::id id;

    /// A Context call that runs on this gate's context for a coroutine of another one.
    class Call {
       public:
        Call() = default;
        Call(const Call&) = delete;
        Call& operator=(const Call&) = delete;
        Call(Call&&) = delete;
        Call& operator=(Call&&) = delete;
        /// Hands the waiting coroutine an error, unless it has its result already.
        virtual void fail() = 0;

       protected:
        ~Call() = default;
    };

    struct State {
        std::mutex mutex;
        std::condition_variable settled;
        bool open = true;  // guarded by mutex
        /// The results that work on other contexts still owes coroutines of this one.
        std::size_t awaited = 0;  // guarded by mutex
        /// The calls under way on this context; expired ones are dropped as others are added.
        std::vector<std::weak_ptr<Call>> calls;  // guarded by mutex
    };

    explicit LoopGate(boost::asio::execution_context& context)
        : boost::asio::execution_context::service(context) {}

    [[nodiscard]] const std::shared_ptr<State>& state() const { return state_; }

   private:
    void shutdown() override {
        std::vector<std::weak_ptr<Call>> calls;
        {
            const std::scoped_lock lock(state_->mutex);
            state_->open = false;
            calls.swap(state_->calls);
        }
        // The calls end first: a handler that waits for one can then return.
        for (const auto& weak : calls) {
            if (const auto call = weak.lock()) {
                call->fail();
            }
        }
        std::unique_lock lock(state_->mutex);
        state_->settled.wait(lock, [this] { return state_->awaited == 0; });
    }

    std::shared_ptr<State> state_ = std::make_shared<State>();
};

/// What a HandBack hands a result to: the waiting coroutine, resumed on its own executor while
/// that executor's context takes results and destroyed otherwise, once.
template <typename Handler, typename T>
class HandBackCore final : public LoopGate::Call {
   public:
    /// T, or a stand-in that keeps the value overload well-formed when T is void.
    struct NoValue {};
    using Value = std::conditional_t<std::is_void_v<T>, NoValue, T>;

    /// @p ended is the error the coroutine gets when the work is dropped before it finishes.
    /// @p elsewhere is whether the work runs on another execution context than @p waiter's, whose
    /// shutdown then waits for this.
    HandBackCore(std::shared_ptr<LoopGate::State> gate, Handler handler,
                 const boost::asio::any_io_executor& waiter, const char* ended, bool elsewhere)
        : gate_(std::move(gate)),
          handler_(std::move(handler)),
          waiter_(boost::asio::prefer(waiter, boost::asio::execution::outstanding_work_t::tracked)),
          ended_(ended),
          awaited_(elsewhere) {
        if (awaited_) {
            const std::scoped_lock lock(gate_->mutex);
            ++gate_->awaited;
        }
    }
    HandBackCore(const HandBackCore&) = delete;
    HandBackCore& operator=(const HandBackCore&) = delete;
    HandBackCore(HandBackCore&&) = delete;
    HandBackCore& operator=(HandBackCore&&) = delete;
    ~HandBackCore() = default;

    void fail() override { resume(std::make_exception_ptr(std::runtime_error(ended_))); }

    /// Hands the coroutine @p error, and @p values when there is no error; does nothing once it
    /// has been handed something.
    template <typename... Values>
    void resume(std::exception_ptr error, Values... values) {
        std::unique_lock lock(gate_->mutex);
        if (!handler_) {
            return;
        }
        std::optional<Handler> handler(std::move(handler_));
        handler_.reset();
        if (!gate_->open) {
            // The waiting coroutine's context is shutting down, and is still there: its shutdown
            // waits for this when the work runs elsewhere, and otherwise this is one of its own
            // handlers. The coroutine is destroyed as the context's pending handlers are, outside
            // the gate, since destroying it may settle other work of this context.
            auto waiter = std::move(waiter_);
            lock.unlock();
            handler.reset();
            waiter = boost::asio::any_io_executor();
            settle();
            return;
        }
        if constexpr (std::is_void_v<T>) {
            boost::asio::post(waiter_,
                              [handler = std::move(*handler), error = std::move(error)]() mutable {
                                  std::move(handler)(std::move(error));
                              });
        } else {
            T result{};
            if constexpr (sizeof...(Values) == 1) {
                result = std::move(values...);
            }
            boost::asio::post(waiter_, [handler = std::move(*handler), error = std::move(error),
                                        result = std::move(result)]() mutable {
                std::move(handler)(std::move(error), std::move(result));
            });
        }
        // Done under the gate as well: ending the work reaches the context too.
        waiter_ = boost::asio::any_io_executor();
        lock.unlock();
        settle();
    }

   private:
    /// Tells the waiting coroutine's context that this owes it nothing more.
    void settle() {
        if (!awaited_) {
            return;
        }
        awaited_ = false;
        {
            const std::scoped_lock lock(gate_->mutex);
            --gate_->awaited;
        }
        gate_->settled.notify_all();
    }

    std::shared_ptr<LoopGate::State> gate_;
    std::optional<Handler> handler_;  // guarded by gate_->mutex
    /// The waiting coroutine's executor, which this keeps from running out of work.
    boost::asio::any_io_executor waiter_;  // guarded by gate_->mutex
    const char* ended_;
    /// Whether the waiting coroutine's context counts this among the results it waits for. Read
    /// and written only by the call of resume() that took the handler.
    bool awaited_;
};

/// The completion of work started for a coroutine on another executor, by run_on_pool() or
/// run_on_loop(). Destroyed without having been called when the executor that runs the work ends
/// before the work does, such as when the Runtime that runs it is destroyed, it hands the waiting
/// coroutine an error.
template <typename Handler, typename T>
class HandBack {
   public:
    using Core = HandBackCore<Handler, T>;

    explicit HandBack(std::shared_ptr<Core> core) : core_(std::move(core)) {}
    HandBack(HandBack&& other) noexcept = default;
    HandBack(const HandBack&) = delete;
    HandBack& operator=(const HandBack&) = delete;
    HandBack& operator=(HandBack&&) = delete;
    ~HandBack() {
        if (!core_) {
            return;
        }
        try {
            core_->fail();
        } catch (...) {
            // A destructor has no caller to report the failure to.
        }
    }

    void operator()(std::exception_ptr error)
        requires std::is_void_v<T>
    {
        std::exchange(core_, nullptr)->resume(std::move(error));
    }

    void operator()(std::exception_ptr error, Core::Value value)
        requires(!std::is_void_v<T>)
    {
        std::exchange(core_, nullptr)->resume(std::move(error), std::move(value));
    }

   private:
    std::shared_ptr<Core> core_;
};

/// The gate of the execution context that @p executor belongs to.
inline std::shared_ptr<LoopGate::State> gate_of(const boost::asio::any_io_executor& executor) {
    return boost::asio::use_service<LoopGate>(
               boost::asio::query(executor, boost::asio::execution::context))
        .state();
}

/// Whether @p first and @p second belong to different execution contexts.
inline bool apart(const boost::asio::any_io_executor& first,
                  const boost::asio::any_io_executor& second) {
    return &boost::asio::query(first, boost::asio::execution::context) !=
           &boost::asio::query(second, boost::asio::execution::context);
}

/// Whether work on other contexts still owes results to coroutines of @p executor's context, whose
/// destruction then waits for that work.
inline bool awaits_elsewhere(const boost::asio::any_io_executor& executor) {
    const auto gate = gate_of(executor);
    const std::scoped_lock lock(gate->mutex);
    return gate->awaited != 0;
}

/// The initiation of run_on_pool().
struct PoolInitiation {
    template <typename Handler, typename T>
    void operator()(Handler&& handler, const boost::asio::any_io_executor& pool, Task<T> task) const {
        const boost::asio::any_io_executor loop = boost::asio::get_associated_executor(handler);
        // The strand is made here, so that only the pool's side holds it: the waiting coroutine
        // may resume after the pool's context is gone, and a strand cannot be destroyed then.
        using Back = HandBack<std::decay_t<Handler>, T>;
        boost::asio::co_spawn(
            make_strand_executor(pool), std::move(task),
            Back(std::make_shared<typename Back::Core>(
                gate_of(loop), std::forward<Handler>(handler), loop,
                "The handler executor ended before the handler returned", apart(pool, loop))));
    }
};

/**
 * @brief co_spawn(strand of pool, task, use_awaitable) for a handler on
 *        Server::set_handler_executor()'s executor.
 *
 * @details The task returns to the awaiting coroutine through that coroutine's execution context
 * while the context takes results. The context's destruction waits for the task (see LoopGate) and
 * then destroys the coroutine unresumed; so does the pool's executor ending first.
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

/// The coroutine run_on_loop() starts on the loop. A named coroutine, so that @p call lives in its
/// frame until the task it makes has finished.
template <typename T>
Task<T> call_on_loop(std::function<Task<T>()> call) {
    if constexpr (std::is_void_v<T>) {
        co_await call();
    } else {
        co_return co_await call();
    }
}

/// The initiation of run_on_loop().
struct LoopInitiation {
    template <typename Handler, typename T>
    void operator()(Handler&& handler, const std::shared_ptr<LoopGate::State>& gate,
                    const boost::asio::any_io_executor& loop, std::function<Task<T>()> call) const {
        const boost::asio::any_io_executor waiter = boost::asio::get_associated_executor(handler);
        using Back = HandBack<std::decay_t<Handler>, T>;
        // Should the loop's context go before the call finishes, this hands the waiting coroutine
        // the error on its own executor.
        auto core = std::make_shared<typename Back::Core>(
            gate_of(waiter), std::forward<Handler>(handler), waiter, "The server has stopped",
            apart(loop, waiter));
        Back back(core);
        const std::scoped_lock lock(gate->mutex);
        if (!gate->open) {
            return;
        }
        // The loop's shutdown ends the call, which it may otherwise destroy only after waiting
        // for the handler that waits for the call.
        std::erase_if(gate->calls,
                      [](const std::weak_ptr<LoopGate::Call>& held) { return held.expired(); });
        gate->calls.emplace_back(core);
        // Posted while the gate is held, so the loop's context cannot go away meanwhile. Never run
        // in place, as co_spawn() alone would on the loop's thread: what the call does must not
        // run under the gate.
        boost::asio::post(loop, [loop, call = std::move(call), back = std::move(back)]() mutable {
            boost::asio::co_spawn(loop, call_on_loop<T>(std::move(call)), std::move(back));
        });
    }
};

/**
 * @brief Runs @p call on @p loop, whose LoopGate state is @p gate, for a handler on the handler
 *        executor; the awaiting coroutine resumes on its own executor.
 *
 * @throws std::runtime_error When the loop's context is gone before @p call finishes; @p call is
 *         not called when the context is gone already.
 */
template <typename T>
Task<T> run_on_loop(const std::shared_ptr<LoopGate::State>& gate,
                    const boost::asio::any_io_executor& loop, std::function<Task<T>()> call) {
    if constexpr (std::is_void_v<T>) {
        return boost::asio::async_initiate<const boost::asio::use_awaitable_t<>&,
                                           void(std::exception_ptr)>(
            LoopInitiation{}, boost::asio::use_awaitable, gate, loop, std::move(call));
    } else {
        return boost::asio::async_initiate<const boost::asio::use_awaitable_t<>&,
                                           void(std::exception_ptr, T)>(
            LoopInitiation{}, boost::asio::use_awaitable, gate, loop, std::move(call));
    }
}

/// Context::message_sender_ of a Context that ContextRouting::through_loop() changed.
struct LoopMessageSender {
    std::shared_ptr<LoopGate::State> gate;
    boost::asio::any_io_executor loop;
    std::shared_ptr<const MessageSender> inner;

    Task<void> operator()(std::string_view message) const {
        auto text = std::make_shared<const std::string>(message);
        return run_on_loop<void>(gate, loop, [inner = inner, text] { return (*inner)(*text); });
    }
};

/// Context::sender_ of a Context that ContextRouting::through_loop() changed.
struct LoopRequestSender {
    std::shared_ptr<LoopGate::State> gate;
    boost::asio::any_io_executor loop;
    std::shared_ptr<const RequestSender> inner;

    Task<nlohmann::json> operator()(std::string method, std::optional<nlohmann::json> params,
                                    std::optional<std::string> key) const {
        struct Request {
            std::string method;
            std::optional<nlohmann::json> params;
            std::optional<std::string> key;
        };
        auto request = std::make_shared<const Request>(
            Request{.method = std::move(method), .params = std::move(params), .key = std::move(key)});
        return run_on_loop<nlohmann::json>(gate, loop, [inner = inner, request] {
            return (*inner)(request->method, request->params, request->key);
        });
    }
};

/// Changes a Context for a handler that runs on the handler executor.
struct ContextRouting {
    /// Limits the progress and log notifications @p context sends to @p budget.
    static void set_notification_budget(Context& context, std::shared_ptr<NotificationBudget> budget) {
        context.budget_ = std::move(budget);
    }

    /**
     * @brief Sends @p context's notifications and requests to the client from @p loop, the
     *        executor the server serves the request on, while that loop's context exists.
     *
     * @details Once the loop's context is being destroyed, as when run_stdio() or run_http() have
     * given up waiting for the handler, its calls throw instead of reaching the loop's objects. A
     * Context already sent through @p loop's context is left as it is, as when a middleware calls
     * the next handler more than once.
     */
    static void through_loop(Context& context, const boost::asio::any_io_executor& loop) {
        auto gate = gate_of(loop);
        const auto* routed_message = context.message_sender_.target<LoopMessageSender>();
        if (context.message_sender_ && (routed_message == nullptr || routed_message->gate != gate)) {
            context.message_sender_ = LoopMessageSender{
                .gate = gate,
                .loop = loop,
                .inner = std::make_shared<const MessageSender>(std::move(context.message_sender_)),
            };
        }
        const auto* routed_request = context.sender_.target<LoopRequestSender>();
        if (context.sender_ && (routed_request == nullptr || routed_request->gate != gate)) {
            context.sender_ = LoopRequestSender{
                .gate = gate,
                .loop = loop,
                .inner = std::make_shared<const RequestSender>(std::move(context.sender_)),
            };
        }
    }
};

}  // namespace mcp::detail

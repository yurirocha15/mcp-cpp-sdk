#pragma once

#include "strand_executor.hpp"

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

/// The completion of work started for a coroutine on another executor, by run_on_pool() or
/// run_on_loop(): resumes that coroutine on its own executor while that executor's context is
/// there.
template <typename Handler, typename T>
class HandBack {
    /// T, or a stand-in that keeps the value overload of operator() well-formed when T is void.
    struct NoValue {};
    using Value = std::conditional_t<std::is_void_v<T>, NoValue, T>;

   public:
    /// @p ended is the error the coroutine gets when the work is dropped before it finishes.
    HandBack(std::shared_ptr<LoopGate::State> gate, Handler handler,
             const boost::asio::any_io_executor& waiter, const char* ended)
        : gate_(std::move(gate)),
          handler_(std::move(handler)),
          waiter_(boost::asio::prefer(waiter, boost::asio::execution::outstanding_work_t::tracked)),
          ended_(ended) {}

    HandBack(HandBack&& other) noexcept
        : gate_(std::move(other.gate_)),
          handler_(std::move(other.handler_)),
          waiter_(std::move(other.waiter_)),
          ended_(other.ended_) {
        other.handler_.reset();
    }
    HandBack(const HandBack&) = delete;
    HandBack& operator=(const HandBack&) = delete;
    HandBack& operator=(HandBack&&) = delete;

    /// Destroyed without having been called when the executor that runs the work ends before the
    /// work does, such as when the Runtime that runs it is destroyed: the waiting coroutine gets
    /// an error.
    ~HandBack() {
        if (!handler_) {
            return;
        }
        resume(std::make_exception_ptr(std::runtime_error(ended_)));
    }

    void operator()(std::exception_ptr error)
        requires std::is_void_v<T>
    {
        resume(std::move(error));
    }

    void operator()(std::exception_ptr error, Value value)
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
            keep_forever(std::move(waiter_));
            return;
        }
        if constexpr (std::is_void_v<T>) {
            boost::asio::post(waiter_, [handler = std::move(*handler_), error]() mutable {
                std::move(handler)(error);
            });
        } else {
            T result{};
            if constexpr (sizeof...(Values) == 1) {
                result = std::move(values...);
            }
            boost::asio::post(
                waiter_, [handler = std::move(*handler_), error, result = std::move(result)]() mutable {
                    std::move(handler)(error, std::move(result));
                });
        }
        handler_.reset();
        // Done under the gate as well: ending the work reaches the context too.
        waiter_ = boost::asio::any_io_executor();
    }

    std::shared_ptr<LoopGate::State> gate_;
    std::optional<Handler> handler_;
    /// The waiting coroutine's executor, which this keeps from running out of work.
    boost::asio::any_io_executor waiter_;
    const char* ended_;
};

/// The gate of the execution context that @p executor belongs to.
inline std::shared_ptr<LoopGate::State> gate_of(const boost::asio::any_io_executor& executor) {
    return boost::asio::use_service<LoopGate>(
               boost::asio::query(executor, boost::asio::execution::context))
        .state();
}

/// The initiation of run_on_pool().
struct PoolInitiation {
    template <typename Handler, typename T>
    void operator()(Handler&& handler, const boost::asio::any_io_executor& pool, Task<T> task) const {
        const boost::asio::any_io_executor loop = boost::asio::get_associated_executor(handler);
        // The strand is made here, so that only the pool's side holds it: the waiting coroutine
        // may resume after the pool's context is gone, and a strand cannot be destroyed then.
        boost::asio::co_spawn(
            make_strand_executor(pool), std::move(task),
            HandBack<std::decay_t<Handler>, T>(gate_of(loop), std::forward<Handler>(handler), loop,
                                               "The handler executor ended before the handler "
                                               "returned"));
    }
};

/**
 * @brief co_spawn(strand of pool, task, use_awaitable) for a handler on
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
        // Should the loop's context go before the call finishes, this hands the waiting coroutine
        // the error on its own executor.
        HandBack<std::decay_t<Handler>, T> back(gate_of(waiter), std::forward<Handler>(handler), waiter,
                                                "The server has stopped");
        const std::lock_guard lock(gate->mutex);
        if (!gate->open) {
            return;
        }
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
            Request{std::move(method), std::move(params), std::move(key)});
        return run_on_loop<nlohmann::json>(gate, loop, [inner = inner, request] {
            return (*inner)(request->method, request->params, request->key);
        });
    }
};

/// Changes a Context for a handler that runs on the handler executor.
struct ContextRouting {
    /**
     * @brief Sends @p context's notifications and requests to the client from @p loop, the
     *        executor the server serves the request on, while that loop's context exists.
     *
     * @details A handler may outlive the loop, as when run_stdio() or run_http() give up waiting
     * for it; its calls then throw instead of reaching the loop's destroyed objects. A Context
     * already sent through @p loop's context is left as it is, as when a middleware calls the
     * next handler more than once.
     */
    static void through_loop(Context& context, const boost::asio::any_io_executor& loop) {
        auto gate = gate_of(loop);
        const auto* routed_message = context.message_sender_.target<LoopMessageSender>();
        if (context.message_sender_ && (routed_message == nullptr || routed_message->gate != gate)) {
            context.message_sender_ = LoopMessageSender{
                gate, loop, std::make_shared<const MessageSender>(std::move(context.message_sender_))};
        }
        const auto* routed_request = context.sender_.target<LoopRequestSender>();
        if (context.sender_ && (routed_request == nullptr || routed_request->gate != gate)) {
            context.sender_ = LoopRequestSender{
                gate, loop, std::make_shared<const RequestSender>(std::move(context.sender_))};
        }
    }
};

}  // namespace mcp::detail

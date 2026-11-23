#pragma once

#include "mcp/core/async.hpp"

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/execution_context.hpp>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <nlohmann/json_fwd.hpp>
#include <string>
#include <type_traits>
#include <utility>

// Test coroutines are started through the functions below instead of boost::asio::co_spawn. Each
// distinct callable passed to co_spawn instantiates Asio's spawn and executor machinery again;
// these functions are compiled once, in spawn.cpp, so a test file only converts its callable.

namespace mcp_test {

/// A move-only function object, so a callable that owns a move-only value can be stored where
/// std::function, which needs a copyable callable, cannot.
template <typename Signature>
class UniqueFunction;

template <typename Result, typename... Args>
class UniqueFunction<Result(Args...)> {
   public:
    template <typename Callable>
        requires(!std::is_same_v<std::remove_cvref_t<Callable>, UniqueFunction> &&
                 std::is_invocable_r_v<Result, std::remove_reference_t<Callable>&, Args...>)
    UniqueFunction(Callable&& callable)  // NOLINT(google-explicit-constructor)
        : target_(std::make_unique<Target<std::remove_cvref_t<Callable>>>(
              std::forward<Callable>(callable))) {}

    Result operator()(Args... args) { return target_->call(std::forward<Args>(args)...); }

   private:
    struct TargetBase {
        virtual ~TargetBase() = default;
        virtual Result call(Args... args) = 0;
    };

    template <typename Callable>
    struct Target final : TargetBase {
        explicit Target(Callable&& callable) : callable_(std::move(callable)) {}
        explicit Target(const Callable& callable) : callable_(callable) {}
        Result call(Args... args) override { return callable_(std::forward<Args>(args)...); }
        Callable callable_;
    };

    std::unique_ptr<TargetBase> target_;
};

/// A coroutine that is started by calling it. The spawn functions keep it alive until the coroutine
/// it returns has finished, so its captures stay valid across every suspension.
using TaskFunction = UniqueFunction<mcp::Task<void>()>;

/// Where a spawned coroutine runs: an execution context such as io_context or thread_pool, or any
/// executor that converts to any_io_executor.
class SpawnExecutor {
   public:
    template <typename Context>
        requires std::is_base_of_v<boost::asio::execution_context, Context>
    SpawnExecutor(Context& context)  // NOLINT(google-explicit-constructor)
        : executor_(context.get_executor()) {}

    template <typename Executor>
        requires(std::is_convertible_v<Executor, boost::asio::any_io_executor> &&
                 !std::is_same_v<std::remove_cvref_t<Executor>, SpawnExecutor>)
    SpawnExecutor(Executor executor)  // NOLINT(google-explicit-constructor)
        : executor_(executor) {}

    [[nodiscard]] const boost::asio::any_io_executor& get() const { return executor_; }

   private:
    boost::asio::any_io_executor executor_;
};

/// Runs @p task on @p executor and drops its result. An exception that escapes it is dropped too,
/// as with boost::asio::detached.
void spawn_detached(const SpawnExecutor& executor, mcp::Task<void> task);

/// Calls @p function on @p executor and runs the coroutine it returns, like spawn_detached.
void spawn_detached(const SpawnExecutor& executor, TaskFunction function);

/// Runs @p task on @p executor and calls @p completion there, with the exception that escaped the
/// task or nullptr. An exception that @p completion throws leaves io_context::run().
void spawn_then(const SpawnExecutor& executor, mcp::Task<void> task,
                UniqueFunction<void(std::exception_ptr)> completion);
void spawn_then(const SpawnExecutor& executor, TaskFunction function,
                UniqueFunction<void(std::exception_ptr)> completion);

/// The same for a task that returns a value: @p completion gets a default-constructed value when
/// the task threw.
void spawn_then(const SpawnExecutor& executor, mcp::Task<std::string> task,
                UniqueFunction<void(std::exception_ptr, std::string)> completion);
void spawn_then(const SpawnExecutor& executor, mcp::Task<nlohmann::json> task,
                UniqueFunction<void(std::exception_ptr, nlohmann::json)> completion);

/// Runs @p task on @p executor. The future holds its result, or the exception that escaped it.
std::future<void> spawn_future(const SpawnExecutor& executor, mcp::Task<void> task);
std::future<void> spawn_future(const SpawnExecutor& executor, TaskFunction function);
std::future<bool> spawn_future(const SpawnExecutor& executor, mcp::Task<bool> task);
std::future<bool> spawn_future(const SpawnExecutor& executor,
                               UniqueFunction<mcp::Task<bool>()> function);
std::future<std::string> spawn_future(const SpawnExecutor& executor, mcp::Task<std::string> task);
std::future<nlohmann::json> spawn_future(const SpawnExecutor& executor, mcp::Task<nlohmann::json> task);

}  // namespace mcp_test

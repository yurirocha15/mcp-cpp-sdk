#include "mcp/core/async.hpp"

#include <gtest/gtest.h>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>
#include <chrono>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

#if __has_include(<boost/asio/experimental/awaitable_operators.hpp>)
#include <boost/asio/experimental/awaitable_operators.hpp>
#define MCP_TEST_HAS_AWAITABLE_OPERATORS 1
#endif

namespace task_class_test {

static_assert(std::is_base_of_v<boost::asio::awaitable<int>, mcp::Task<int>>);
static_assert(std::is_nothrow_move_constructible_v<mcp::Task<int>>);

mcp::Task<int> forty_two() { co_return 42; }

mcp::Task<std::unique_ptr<int>> boxed(int value) { co_return std::make_unique<int>(value); }

boost::asio::awaitable<int> plain_awaitable(int value) { co_return value; }

boost::asio::awaitable<int> plain_awaitable_awaiting_task() { co_return co_await forty_two() + 1; }

mcp::Task<int> task_awaiting_plain_awaitable() { co_return co_await plain_awaitable(7) * 2; }

mcp::Task<void> throws_runtime_error() {
    co_await forty_two();
    throw std::runtime_error("task failed");
}

// Not a coroutine: hands back the awaitable that co_spawn returns for use_awaitable.
mcp::Task<int> spawned_through_use_awaitable() {
    auto executor = co_await boost::asio::this_coro::executor;
    co_return co_await boost::asio::co_spawn(executor, forty_two(), boost::asio::use_awaitable);
}

mcp::Task<int> converted_from_awaitable() { return plain_awaitable(5); }

template <typename T>
T run_to_completion(mcp::Task<T> task) {
    boost::asio::io_context io_ctx;
    auto result = boost::asio::co_spawn(io_ctx, std::move(task), boost::asio::use_future);
    io_ctx.run();
    return result.get();
}

TEST(TaskClassTest, ProducesAValue) { EXPECT_EQ(run_to_completion(forty_two()), 42); }

TEST(TaskClassTest, ProducesAMoveOnlyValue) {
    auto value = run_to_completion(boxed(9));
    ASSERT_NE(value, nullptr);
    EXPECT_EQ(*value, 9);
}

TEST(TaskClassTest, AwaitsAndIsAwaitedByPlainAwaitables) {
    EXPECT_EQ(run_to_completion(task_awaiting_plain_awaitable()), 14);

    boost::asio::io_context io_ctx;
    auto result =
        boost::asio::co_spawn(io_ctx, plain_awaitable_awaiting_task(), boost::asio::use_future);
    io_ctx.run();
    EXPECT_EQ(result.get(), 43);
}

TEST(TaskClassTest, SpawnsDetached) {
    boost::asio::io_context io_ctx;
    bool finished = false;
    auto mark_finished = [](bool* flag) -> mcp::Task<void> {
        co_await forty_two();
        *flag = true;
    };
    boost::asio::co_spawn(io_ctx, mark_finished(&finished), boost::asio::detached);
    io_ctx.run();
    EXPECT_TRUE(finished);
}

TEST(TaskClassTest, CarriesAnExceptionToTheAwaiter) {
    EXPECT_THROW(run_to_completion(throws_runtime_error()), std::runtime_error);
}

TEST(TaskClassTest, TakesOverAnAwaitable) {
    EXPECT_EQ(run_to_completion(spawned_through_use_awaitable()), 42);
    EXPECT_EQ(run_to_completion(converted_from_awaitable()), 5);
}

TEST(TaskClassTest, CoSpawnTakesACallableThatReturnsATask) {
    boost::asio::io_context io_ctx;
    int observed = 0;
    boost::asio::co_spawn(
        io_ctx, [&observed]() -> mcp::Task<void> { observed = co_await forty_two(); },
        boost::asio::detached);
    auto doubled = boost::asio::co_spawn(
        io_ctx.get_executor(), []() -> mcp::Task<int> { co_return co_await forty_two() * 2; },
        boost::asio::use_future);
    io_ctx.run();
    EXPECT_EQ(observed, 42);
    EXPECT_EQ(doubled.get(), 84);
}

TEST(TaskClassTest, CoSpawnKeepsTheCallableAliveWhileItsTaskWaits) {
    boost::asio::io_context io_ctx;
    auto kept = boost::asio::co_spawn(
        io_ctx,
        // Longer than the small-string buffer, which GCC 11 mishandles in a coroutine frame.
        [text = std::string("kept alive by co_spawn")]() -> mcp::Task<std::string> {
            const auto executor = co_await boost::asio::this_coro::executor;
            boost::asio::steady_timer timer(executor, std::chrono::milliseconds(1));
            co_await timer.async_wait(boost::asio::use_awaitable);
            co_return text;
        },
        boost::asio::use_future);
    io_ctx.run();
    EXPECT_EQ(kept.get(), "kept alive by co_spawn");
}

TEST(TaskClassTest, CoSpawnHandsTheExceptionOfACallablesTaskToTheHandler) {
    boost::asio::io_context io_ctx;
    std::exception_ptr failure;
    boost::asio::co_spawn(
        io_ctx, []() -> mcp::Task<void> { co_await throws_runtime_error(); },
        [&failure](std::exception_ptr error) { failure = std::move(error); });
    io_ctx.run();
    ASSERT_NE(failure, nullptr);
    EXPECT_THROW(std::rethrow_exception(failure), std::runtime_error);
}

mcp::Task<int> awaited_callable_spawn() {
    auto executor = co_await boost::asio::this_coro::executor;
    co_return co_await boost::asio::co_spawn(
        executor, []() -> mcp::Task<int> { co_return co_await forty_two() + 2; },
        boost::asio::use_awaitable);
}

TEST(TaskClassTest, CoSpawnOfACallableCanBeAwaited) {
    EXPECT_EQ(run_to_completion(awaited_callable_spawn()), 44);
}

#ifdef MCP_TEST_HAS_AWAITABLE_OPERATORS
mcp::Task<int> never_in_time() {
    boost::asio::steady_timer timer(co_await boost::asio::this_coro::executor, std::chrono::hours(1));
    co_await timer.async_wait(boost::asio::use_awaitable);
    co_return 0;
}

mcp::Task<int> both_and_either() {
    using namespace boost::asio::experimental::awaitable_operators;
    auto [left, right] = co_await (forty_two() && plain_awaitable(1));
    auto either = co_await (never_in_time() || forty_two());
    co_return left + right + static_cast<int>(either.index());
}

TEST(TaskClassTest, CombinesWithAwaitableOperators) {
    // forty_two() wins the || race (index 1) and never_in_time() is cancelled.
    EXPECT_EQ(run_to_completion(both_and_either()), 44);
}
#endif

}  // namespace task_class_test

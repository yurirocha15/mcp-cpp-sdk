#include "mcp/client/client.hpp"
#include "mcp/core/async.hpp"
#include "mcp/server/server.hpp"
#include "mcp/transport/memory.hpp"

#include <gtest/gtest.h>

#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/thread_pool.hpp>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace executor_test {

namespace {
struct NotAnExecutor {};
}  // namespace

// Asio executors convert to an Executor; other class types do not, so an overload that takes an
// Executor is not chosen for them.
static_assert(std::is_convertible_v<boost::asio::io_context::executor_type, mcp::Executor>);
static_assert(std::is_convertible_v<boost::asio::any_io_executor, mcp::Executor>);
static_assert(std::is_convertible_v<boost::asio::strand<boost::asio::any_io_executor>, mcp::Executor>);
static_assert(std::is_convertible_v<boost::asio::thread_pool::executor_type, mcp::Executor>);
static_assert(!std::is_convertible_v<std::string, mcp::Executor>);
static_assert(!std::is_convertible_v<std::vector<int>, mcp::Executor>);
static_assert(!std::is_convertible_v<NotAnExecutor, mcp::Executor>);

TEST(ExecutorTest, RunsWorkOnTheWrappedContext) {
    boost::asio::io_context io_ctx;
    const mcp::Executor executor = io_ctx.get_executor();
    ASSERT_TRUE(executor);

    bool ran = false;
    boost::asio::post(executor.get(), [&ran] { ran = true; });
    io_ctx.run();
    EXPECT_TRUE(ran);
}

TEST(ExecutorTest, CopiesHoldTheSameExecutor) {
    boost::asio::io_context io_ctx;
    const mcp::Executor executor = io_ctx.get_executor();
    const mcp::Executor copy = executor;  // NOLINT(performance-unnecessary-copy-initialization)
    EXPECT_EQ(copy.get(), executor.get());
    EXPECT_EQ(copy.get(), boost::asio::any_io_executor(io_ctx.get_executor()));
}

TEST(ExecutorTest, WrapsAStrand) {
    boost::asio::io_context io_ctx;
    auto strand = boost::asio::make_strand(io_ctx.get_executor());
    const mcp::Executor executor = strand;
    EXPECT_EQ(executor.get(), boost::asio::any_io_executor(strand));
}

TEST(ExecutorTest, EmptyExecutorIsRefused) {
    const mcp::Executor empty;
    EXPECT_FALSE(empty);
    EXPECT_THROW(static_cast<void>(empty.get()), std::invalid_argument);
}

TEST(ExecutorTest, AnEmptyAsioExecutorMakesAnEmptyExecutor) {
    const mcp::Executor empty = boost::asio::any_io_executor();
    EXPECT_FALSE(empty);
    EXPECT_THROW(static_cast<void>(empty.get()), std::invalid_argument);
    mcp::Server server("executor-test", "1.0");
    EXPECT_THROW(server.set_handler_executor(empty), std::invalid_argument);
}

TEST(ExecutorTest, ClientReturnsItsConstructorExecutor) {
    boost::asio::io_context io_ctx;
    auto [client_end, server_end] = mcp::make_memory_transport_pair(io_ctx.get_executor());
    mcp::Client client(client_end, io_ctx.get_executor());
    EXPECT_EQ(client.executor().get(), boost::asio::any_io_executor(io_ctx.get_executor()));
}

}  // namespace executor_test

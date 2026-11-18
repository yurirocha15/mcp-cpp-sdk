#include "mcp/client/client.hpp"
#include "mcp/core/async.hpp"
#include "mcp/transport/memory.hpp"

#include <gtest/gtest.h>

#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>
#include <stdexcept>

namespace executor_test {

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

TEST(ExecutorTest, ClientReturnsItsConstructorExecutor) {
    boost::asio::io_context io_ctx;
    auto [client_end, server_end] = mcp::create_memory_transport_pair(io_ctx.get_executor());
    mcp::Client client(client_end, io_ctx.get_executor());
    EXPECT_EQ(client.executor().get(), boost::asio::any_io_executor(io_ctx.get_executor()));
}

}  // namespace executor_test

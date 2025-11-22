#include "mcp/core.hpp"

#include <gtest/gtest.h>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>

class CoreTest : public ::testing::Test {
   protected:
    boost::asio::io_context io_ctx;
};

mcp::Task<void> simple_task() { co_return; }

TEST_F(CoreTest, CoroutineCompilation) {
    bool executed = false;
    boost::asio::co_spawn(
        io_ctx,
        [&]() -> mcp::Task<void> {
            co_await simple_task();
            executed = true;
        },
        boost::asio::detached);

    io_ctx.run();
    EXPECT_TRUE(executed);
}

TEST_F(CoreTest, LogLevelEnum) {
    mcp::LogLevel level = mcp::LogLevel::Info;
    EXPECT_EQ(level, mcp::LogLevel::Info);
}

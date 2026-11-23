#include "mcp/core/async.hpp"
#include "support/spawn.hpp"

#include <gtest/gtest.h>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>

namespace core_test {

class CoreTest : public ::testing::Test {
   protected:
    boost::asio::io_context io_ctx;
};

mcp::Task<void> simple_task() { co_return; }

TEST_F(CoreTest, CoroutineCompilation) {
    bool executed = false;
    mcp_test::spawn_detached(io_ctx, [&]() -> mcp::Task<void> {
        co_await simple_task();
        executed = true;
    });

    io_ctx.run();
    EXPECT_TRUE(executed);
}

}  // namespace core_test

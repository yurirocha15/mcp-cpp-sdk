#include "mcp/runtime.hpp"
#include "mcp/detail/runtime_access.hpp"

#include <gtest/gtest.h>
#include <atomic>
#include <boost/asio/any_io_executor.hpp>
#include <thread>

TEST(RuntimeTest, ConstructAndDestroy) {
    EXPECT_NO_THROW({
        mcp::Runtime rt;
        (void)rt;
    });
}

TEST(RuntimeTest, DetailGetExecutor) {
    mcp::Runtime rt;
    boost::asio::any_io_executor executor = mcp::detail::get_executor(rt);
    EXPECT_TRUE(static_cast<bool>(executor));
}

TEST(RuntimeTest, RunReturnsWhenNoWork) {
    mcp::Runtime rt;
    EXPECT_NO_THROW(rt.run());
}

TEST(RuntimeTest, StopCausesRunToReturn) {
    mcp::Runtime rt;
    std::atomic<bool> running{false};

    std::thread t([&] {
        running = true;
        EXPECT_NO_THROW(rt.run());
    });

    while (!running) {
        std::this_thread::yield();
    }
    EXPECT_NO_THROW(rt.stop());
    if (t.joinable()) {
        t.join();
    }
}

TEST(RuntimeTest, MoveConstruct) {
    mcp::Runtime rt1;
    mcp::Runtime rt2(std::move(rt1));
    EXPECT_NO_THROW(rt2.run());
}

TEST(RuntimeTest, MoveAssign) {
    mcp::Runtime rt1;
    mcp::Runtime rt2;
    rt2 = std::move(rt1);
    EXPECT_NO_THROW(rt2.run());
}

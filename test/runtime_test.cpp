#include "mcp/runtime.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <thread>

TEST(RuntimeTest, ConstructAndDestroy) { mcp::Runtime rt; }

TEST(RuntimeTest, RunReturnsWhenNoWork) {
    mcp::Runtime rt;
    rt.run();
}

TEST(RuntimeTest, StopCausesRunToReturn) {
    mcp::Runtime rt;
    std::atomic<bool> running{false};

    std::thread t([&] {
        running = true;
        rt.run();
    });

    while (!running) {
        std::this_thread::yield();
    }
    rt.stop();
    t.join();
}

TEST(RuntimeTest, MoveConstruct) {
    mcp::Runtime rt1;
    mcp::Runtime rt2(std::move(rt1));
    rt2.run();
}

TEST(RuntimeTest, MoveAssign) {
    mcp::Runtime rt1;
    mcp::Runtime rt2;
    rt2 = std::move(rt1);
    rt2.run();
}

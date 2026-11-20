#include "mcp/core/runtime.hpp"
#include "mcp/core/async.hpp"
#include "mcp/detail/runtime_access.hpp"

#include <gtest/gtest.h>
#include <atomic>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/post.hpp>
#include <chrono>
#include <future>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>

namespace runtime_test {

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

TEST(RuntimeTest, SpawnedTaskAndExecutorWorkRunOnRun) {
    mcp::Runtime rt;
    bool ran = false;
    rt.spawn([](bool& flag) -> mcp::Task<void> {
        flag = true;
        co_return;
    }(ran));
    const auto executor = rt.executor();
    ASSERT_TRUE(static_cast<bool>(executor));
    bool posted = false;
    boost::asio::post(executor.get(), [&posted] { posted = true; });
    EXPECT_FALSE(ran);
    EXPECT_FALSE(posted);
    rt.run();
    EXPECT_TRUE(ran);
    EXPECT_TRUE(posted);
}

TEST(RuntimeTest, ExceptionOfASpawnedTaskComesOutOfRun) {
    mcp::Runtime rt;
    rt.spawn([]() -> mcp::Task<void> {
        throw std::runtime_error("task failed");
        co_return;
    }());
    EXPECT_THROW(rt.run(), std::runtime_error);

    mcp::Runtime threaded;
    threaded.spawn([]() -> mcp::Task<void> {
        throw std::runtime_error("task failed");
        co_return;
    }());
    EXPECT_THROW(threaded.run(3), std::runtime_error);
}

TEST(RuntimeTest, SpawnRefusesAnEmptyTask) {
    mcp::Runtime rt;
    EXPECT_THROW(rt.spawn(mcp::Task<void>()), std::invalid_argument);
}

TEST(RuntimeTest, RunWithThreadsRunsTheLoopOnThatManyThreads) {
    EXPECT_THROW(mcp::Runtime().run(0), std::invalid_argument);

    // Each task waits until the other has started, which only two threads can both reach.
    mcp::Runtime rt;
    std::atomic<int> started{0};
    std::atomic<int> met{0};
    std::mutex threads_mutex;
    std::set<std::thread::id> threads;
    for (int i = 0; i < 2; ++i) {
        boost::asio::post(rt.executor().get(), [&] {
            {
                std::lock_guard lock(threads_mutex);
                threads.insert(std::this_thread::get_id());
            }
            ++started;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (started.load() < 2 && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::yield();
            }
            if (started.load() == 2) {
                ++met;
            }
        });
    }
    rt.run(2);
    EXPECT_EQ(met.load(), 2);
    EXPECT_EQ(threads.size(), 2U);
}

TEST(RuntimeTest, StartedThreadsWaitForWorkUntilStop) {
    EXPECT_THROW(mcp::Runtime().start(0), std::invalid_argument);

    mcp::Runtime pool;
    pool.start(2);
    EXPECT_THROW(pool.start(1), std::logic_error);
    // Work posted after the loop has been idle still runs, and on two threads at once.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    std::atomic<int> started{0};
    std::promise<int> first;
    std::promise<int> second;
    auto meet = [&started](std::promise<int>& done) {
        ++started;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (started.load() < 2 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        done.set_value(started.load());
    };
    boost::asio::post(pool.executor().get(), [&] { meet(first); });
    boost::asio::post(pool.executor().get(), [&] { meet(second); });
    EXPECT_EQ(first.get_future().get(), 2);
    EXPECT_EQ(second.get_future().get(), 2);
    pool.stop();
}

TEST(RuntimeTest, DestroyingAStartedRuntimeJoinsItsThreads) {
    std::atomic<bool> ran{false};
    {
        mcp::Runtime pool;
        pool.start(1);
        boost::asio::post(pool.executor().get(), [&ran] { ran = true; });
        while (!ran.load()) {
            std::this_thread::yield();
        }
    }
    EXPECT_TRUE(ran.load());
}

TEST(RuntimeTest, RunRunsAgainAfterTheLoopEnded) {
    int runs = 0;
    const auto count = [](int& counter) -> mcp::Task<void> {
        ++counter;
        co_return;
    };
    mcp::Runtime rt;
    rt.spawn(count(runs));
    rt.run();
    rt.spawn(count(runs));
    rt.run();
    EXPECT_EQ(runs, 2);

    // After a task that threw, and after stop(), on one thread and on several.
    rt.spawn([]() -> mcp::Task<void> {
        throw std::runtime_error("task failed");
        co_return;
    }());
    EXPECT_THROW(rt.run(2), std::runtime_error);
    rt.spawn(count(runs));
    rt.run(2);
    EXPECT_EQ(runs, 3);
    rt.stop();
    rt.spawn(count(runs));
    rt.run();
    EXPECT_EQ(runs, 4);
}

TEST(RuntimeTest, ARuntimeMayBeDestroyedByATaskOnItsOwnThread) {
    auto* pool = new mcp::Runtime;
    pool->start(2);
    std::promise<void> destroyed;
    auto done = destroyed.get_future();
    pool->spawn([](mcp::Runtime* self, std::promise<void>& finished) -> mcp::Task<void> {
        delete self;  // NOLINT(cppcoreguidelines-owning-memory)
        finished.set_value();
        co_return;
    }(pool, destroyed));
    done.get();
    // The thread that destroyed it returns from the loop it still runs, which outlives the
    // Runtime until then.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

}  // namespace runtime_test

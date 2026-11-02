#include "../../src/detail/strand_executor.hpp"

#include <gtest/gtest.h>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/execution.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/prefer.hpp>
#include <boost/asio/require.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/system_executor.hpp>
#include <boost/asio/thread_pool.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <future>
#include <memory>
#include <thread>
#include <typeinfo>
#include <utility>
#include <vector>

namespace {

namespace asio = boost::asio;
namespace execution = boost::asio::execution;

using IoStrand = asio::strand<asio::io_context::executor_type>;
using GenericStrand = asio::strand<asio::any_io_executor>;

/// The room an any_io_executor has for a target before it falls back to a reference-counted heap
/// block: Boost.Asio's any_executor_base stores a shared_ptr and one more pointer.
constexpr std::size_t kInlineBytes = sizeof(std::shared_ptr<void>) + sizeof(void*);

template <typename Target>
constexpr bool fits_inline() {
    return sizeof(Target) <= kInlineBytes && alignof(Target) <= alignof(std::shared_ptr<void>);
}

/// Whether @p executor holds a target of exactly type @p Target. any_executor::target<T>() is not
/// enough: Boost 1.74 returns the stored pointer for every T.
template <typename Target>
bool holds(const asio::any_io_executor& executor) {
    return executor.target_type() == typeid(Target);
}

/// Whether @p executor keeps its target of type @p Target inside its own object. A target in the
/// heap block is found elsewhere, so this is the one place a test can tell the two apart.
template <typename Target>
bool holds_inline(const asio::any_io_executor& executor) {
    if (!holds<Target>(executor)) {
        return false;
    }
    const auto* target = executor.target<Target>();
    const auto* object = reinterpret_cast<const unsigned char*>(&executor);
    const auto* stored = reinterpret_cast<const unsigned char*>(target);
    return stored >= object && stored < object + sizeof(executor);
}

asio::execution_context& context_of(const asio::any_io_executor& executor) {
    return asio::query(executor, execution::context_as<asio::execution_context&>);
}

/// Posts handlers through an executor that each check that none is already running and append
/// their index to a plain vector, so an executor that let two run at once would be a data race as
/// well as a failed check.
struct SerialRun {
    std::atomic<bool> running{false};
    std::atomic<int> overlaps{0};
    std::vector<int> order;

    void post_all(const asio::any_io_executor& executor, int count) {
        for (int index = 0; index < count; ++index) {
            asio::post(executor, [this, index] {
                if (running.exchange(true, std::memory_order_acq_rel)) {
                    overlaps.fetch_add(1, std::memory_order_relaxed);
                }
                order.push_back(index);
                std::this_thread::yield();
                running.store(false, std::memory_order_release);
            });
        }
    }

    [[nodiscard]] bool in_posted_order(int count) const {
        if (static_cast<int>(order.size()) != count) {
            return false;
        }
        for (int index = 0; index < count; ++index) {
            if (order[static_cast<std::size_t>(index)] != index) {
                return false;
            }
        }
        return true;
    }
};

void run_on_threads(asio::io_context& context, int thread_count) {
    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(thread_count));
    for (int index = 0; index < thread_count; ++index) {
        threads.emplace_back([&context] { context.run(); });
    }
    for (auto& thread : threads) {
        thread.join();
    }
}

TEST(StrandExecutorTest, AnIoContextExecutorGetsTheStrandOfThatExecutor) {
    asio::io_context io_context;
    const auto strand = mcp::detail::make_strand_executor(io_context.get_executor());

    EXPECT_TRUE(holds<IoStrand>(strand));
    EXPECT_FALSE(holds<GenericStrand>(strand));
    EXPECT_EQ(holds_inline<IoStrand>(strand), fits_inline<IoStrand>());
    EXPECT_EQ(&context_of(strand), static_cast<asio::execution_context*>(&io_context));
}

TEST(StrandExecutorTest, TheStoredTypeIsTheOnlyOneAnExecutorIsTakenToHold) {
    asio::io_context io_context;
    asio::thread_pool pool(1);
    const asio::any_io_executor empty;
    const asio::any_io_executor plain{io_context.get_executor()};
    const asio::any_io_executor stranded{asio::make_strand(io_context.get_executor())};
    const asio::any_io_executor pooled{pool.get_executor()};

    using IoExecutor = asio::io_context::executor_type;
    EXPECT_FALSE(mcp::detail::holds_executor_of_type<IoExecutor>(empty));
    EXPECT_FALSE(mcp::detail::holds_executor_of_type<IoStrand>(empty));

    EXPECT_TRUE(mcp::detail::holds_executor_of_type<IoExecutor>(plain));
    EXPECT_FALSE(mcp::detail::holds_executor_of_type<IoStrand>(plain));

    EXPECT_FALSE(mcp::detail::holds_executor_of_type<IoExecutor>(stranded));
    EXPECT_TRUE(mcp::detail::holds_executor_of_type<IoStrand>(stranded));

    EXPECT_FALSE(mcp::detail::holds_executor_of_type<IoExecutor>(pooled));
    EXPECT_FALSE(mcp::detail::holds_executor_of_type<IoStrand>(pooled));
    pool.join();
}

TEST(StrandExecutorTest, AStrandIsNeverTheExecutorItIsMadeOver) {
    asio::io_context io_context;
    const asio::any_io_executor plain{io_context.get_executor()};
    const auto strand = mcp::detail::make_strand_executor(plain);

    EXPECT_FALSE(strand == plain);
    EXPECT_FALSE(plain == strand);
}

TEST(StrandExecutorTest, ACopyIsTheSameStrandAndAnotherCallMakesAnotherStrand) {
    asio::io_context io_context;
    const auto strand = mcp::detail::make_strand_executor(io_context.get_executor());
    const asio::any_io_executor copy = strand;
    const auto other = mcp::detail::make_strand_executor(io_context.get_executor());

    EXPECT_TRUE(strand == copy);
    EXPECT_FALSE(strand == other);
}

TEST(StrandExecutorTest, AStrandIsNotWrappedInAnotherStrand) {
    asio::io_context io_context;
    const auto inner = mcp::detail::make_strand_executor(io_context.get_executor());
    const auto outer = mcp::detail::make_strand_executor(inner);

    EXPECT_TRUE(outer == inner);
    EXPECT_TRUE(holds<IoStrand>(outer));
}

TEST(StrandExecutorTest, PreferAndRequireKeepTheStrandInline) {
    asio::io_context io_context;
    const auto strand = mcp::detail::make_strand_executor(io_context.get_executor());

    // The executors Boost.Asio makes for an operation: work tracking while it is pending, and the
    // never-inline, fork pair that post() asks for. Each is the strand with a property applied.
    using Tracked =
        decltype(asio::prefer(std::declval<const IoStrand&>(), execution::outstanding_work.tracked));
    using Never = decltype(asio::require(std::declval<const IoStrand&>(), execution::blocking.never));
    using Posted = decltype(asio::prefer(std::declval<const Never&>(), execution::relationship.fork));

    const asio::any_io_executor tracked = asio::prefer(strand, execution::outstanding_work.tracked);
    const asio::any_io_executor never = asio::require(strand, execution::blocking.never);
    const asio::any_io_executor posted = asio::prefer(never, execution::relationship.fork);

    EXPECT_TRUE(holds<Tracked>(tracked));
    EXPECT_EQ(holds_inline<Tracked>(tracked), fits_inline<Tracked>());
    EXPECT_TRUE(holds<Never>(never));
    EXPECT_EQ(holds_inline<Never>(never), fits_inline<Never>());
    EXPECT_TRUE(holds<Posted>(posted));
    EXPECT_EQ(holds_inline<Posted>(posted), fits_inline<Posted>());
    EXPECT_EQ(&context_of(tracked), &context_of(strand));
    EXPECT_EQ(&context_of(never), &context_of(strand));
    EXPECT_EQ(&context_of(posted), &context_of(strand));
}

TEST(StrandExecutorTest, ATrackedStrandKeepsTheContextRunningUntilItIsGone) {
    asio::io_context io_context;
    const auto strand = mcp::detail::make_strand_executor(io_context.get_executor());
    auto tracked = std::make_unique<asio::any_io_executor>(
        asio::prefer(strand, execution::outstanding_work.tracked));

    std::atomic<bool> returned{false};
    std::thread runner([&] {
        io_context.run();
        returned.store(true, std::memory_order_release);
    });

    // No amount of waiting shows that run() will never return, so the test shows that the context
    // still serves work after a round trip through it, and that it returns once the tracked
    // strand is gone.
    std::promise<void> round_trip;
    asio::post(*tracked, [&round_trip] { round_trip.set_value(); });
    round_trip.get_future().wait();
    EXPECT_FALSE(returned.load(std::memory_order_acquire));

    tracked.reset();
    runner.join();
    EXPECT_TRUE(returned.load(std::memory_order_acquire));
}

TEST(StrandExecutorTest, AnUntrackedStrandDoesNotKeepTheContextRunning) {
    asio::io_context io_context;
    const auto strand = mcp::detail::make_strand_executor(io_context.get_executor());

    bool ran = false;
    asio::post(strand, [&ran] { ran = true; });
    io_context.run();

    EXPECT_TRUE(ran);
    EXPECT_TRUE(io_context.stopped());
}

TEST(StrandExecutorTest, BlockingNeverQueuesWhereDispatchRunsInline) {
    asio::io_context io_context;
    const auto strand = mcp::detail::make_strand_executor(io_context.get_executor());

    bool dispatched_inline = false;
    bool queued_ran_early = true;
    bool queued_ran = false;
    asio::post(strand, [&] {
        asio::dispatch(strand, [&] { dispatched_inline = true; });
        const asio::any_io_executor never = asio::require(strand, execution::blocking.never);
        never.execute([&] { queued_ran = true; });
        queued_ran_early = queued_ran;
    });
    io_context.run();

    EXPECT_TRUE(dispatched_inline);
    EXPECT_FALSE(queued_ran_early);
    EXPECT_TRUE(queued_ran);
}

TEST(StrandExecutorTest, RunsHandlersOneAtATimeInPostedOrder) {
    constexpr int kHandlers = 4000;
    asio::io_context io_context;
    const auto strand = mcp::detail::make_strand_executor(io_context.get_executor());

    SerialRun run;
    run.post_all(strand, kHandlers);
    run_on_threads(io_context, 4);

    EXPECT_EQ(run.overlaps.load(), 0);
    EXPECT_TRUE(run.in_posted_order(kHandlers));
}

TEST(StrandExecutorTest, TwoStrandsOnOneContextRunAtTheSameTime) {
    asio::io_context io_context;
    const auto first = mcp::detail::make_strand_executor(io_context.get_executor());
    const auto second = mcp::detail::make_strand_executor(io_context.get_executor());

    // The first handler of each strand waits for the other to start, which can only happen when
    // the strands do not exclude each other. The wait is bounded so that strands that did would
    // fail the test instead of hanging it.
    std::promise<void> first_started;
    std::promise<void> second_started;
    auto first_seen = first_started.get_future().share();
    auto second_seen = second_started.get_future().share();
    std::atomic<int> met{0};
    constexpr auto kLimit = std::chrono::seconds(60);
    asio::post(first, [&] {
        first_started.set_value();
        if (second_seen.wait_for(kLimit) == std::future_status::ready) {
            met.fetch_add(1);
        }
    });
    asio::post(second, [&] {
        second_started.set_value();
        if (first_seen.wait_for(kLimit) == std::future_status::ready) {
            met.fetch_add(1);
        }
    });
    run_on_threads(io_context, 2);

    EXPECT_EQ(met.load(), 2);
}

TEST(StrandExecutorTest, AThreadPoolExecutorGetsAGenericStrandThatStillSerializes) {
    constexpr int kHandlers = 2000;
    asio::thread_pool pool(4);
    const auto strand = mcp::detail::make_strand_executor(pool.get_executor());

    // A strand over a thread_pool executor is larger than an any_io_executor stores inline, so it
    // is made like the strand of any executor that is not an io_context's.
    EXPECT_TRUE(holds<GenericStrand>(strand));
    EXPECT_FALSE(holds<IoStrand>(strand));

    SerialRun run;
    run.post_all(strand, kHandlers);
    pool.join();

    EXPECT_EQ(run.overlaps.load(), 0);
    EXPECT_TRUE(run.in_posted_order(kHandlers));
}

TEST(StrandExecutorTest, AnyOtherExecutorGetsAGenericStrandThatStillSerializes) {
    constexpr int kHandlers = 2000;
    // Not an io_context executor, so there is no small strand to build.
    const asio::any_io_executor over_system{asio::system_executor()};
    const auto strand = mcp::detail::make_strand_executor(over_system);

    EXPECT_TRUE(holds<GenericStrand>(strand));
    EXPECT_FALSE(holds<IoStrand>(strand));

    SerialRun run;
    run.post_all(strand, kHandlers);
    // system_executor runs handlers on threads of its own; wait for the last one.
    std::promise<void> done;
    asio::post(strand, [&done] { done.set_value(); });
    done.get_future().wait();

    EXPECT_EQ(run.overlaps.load(), 0);
    EXPECT_TRUE(run.in_posted_order(kHandlers));
}

TEST(StrandExecutorTest, ATrackedIoContextExecutorGetsAGenericStrandThatStillSerializes) {
    constexpr int kHandlers = 500;
    asio::io_context io_context;
    // A tracked executor is a different type from the plain one make_strand_executor() knows.
    const asio::any_io_executor tracked{
        asio::prefer(io_context.get_executor(), execution::outstanding_work.tracked)};
    const auto strand = mcp::detail::make_strand_executor(tracked);

    EXPECT_TRUE(holds<GenericStrand>(strand));

    SerialRun run;
    run.post_all(strand, kHandlers);
    std::promise<void> done;
    asio::post(strand, [&done] { done.set_value(); });

    // The tracked base keeps run() from returning, so stop the context once the strand is idle.
    std::vector<std::thread> runners;
    for (int index = 0; index < 3; ++index) {
        runners.emplace_back([&io_context] { io_context.run(); });
    }
    done.get_future().wait();
    io_context.stop();
    for (auto& runner : runners) {
        runner.join();
    }

    EXPECT_EQ(run.overlaps.load(), 0);
    EXPECT_TRUE(run.in_posted_order(kHandlers));
}

}  // namespace

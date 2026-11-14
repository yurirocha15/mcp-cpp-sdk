#include "../../src/detail/strand_executor.hpp"

#include <mcp/detail/serialized_transport_writer.hpp>
#include <mcp/transport/transport.hpp>

#include <gtest/gtest.h>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/system_error.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

class OverlapDetectingTransport final : public mcp::ITransport {
   public:
    mcp::Task<std::string> read_message() override {
        throw std::runtime_error("read_message is not used by this test");
        co_return std::string{};
    }

    mcp::Task<void> write_message(std::string_view message) override {
        const auto executor = co_await boost::asio::this_coro::executor;

        const int active_writes = active_writes_.fetch_add(1, std::memory_order_acq_rel) + 1;
        int previous_max = max_active_writes_.load(std::memory_order_acquire);
        while (active_writes > previous_max &&
               !max_active_writes_.compare_exchange_weak(previous_max, active_writes,
                                                         std::memory_order_acq_rel)) {
        }
        {
            std::lock_guard lock(messages_mutex_);
            messages_.emplace_back(message);
        }

        boost::asio::steady_timer delay(executor, std::chrono::milliseconds(1));
        co_await delay.async_wait(boost::asio::use_awaitable);
        active_writes_.fetch_sub(1, std::memory_order_acq_rel);
    }

    void close() override {}

    [[nodiscard]] int max_active_writes() const {
        return max_active_writes_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::vector<std::string> messages() const {
        std::lock_guard lock(messages_mutex_);
        return messages_;
    }

   private:
    std::atomic<int> active_writes_{0};
    std::atomic<int> max_active_writes_{0};
    mutable std::mutex messages_mutex_;
    std::vector<std::string> messages_;
};

TEST(SerializedTransportWriterTest, OwnsMessagesAndSerializesWritesInFifoOrder) {
    boost::asio::io_context io_context;
    auto transport = std::make_shared<OverlapDetectingTransport>();
    mcp::detail::SerializedTransportWriter writer(transport, io_context.get_executor());

    boost::asio::co_spawn(
        io_context,
        [writer]() mutable -> mcp::Task<void> { co_await writer.write_message(std::string{"first"}); },
        boost::asio::detached);
    boost::asio::co_spawn(
        io_context,
        [writer]() mutable -> mcp::Task<void> { co_await writer.write_message(std::string{"second"}); },
        boost::asio::detached);
    boost::asio::co_spawn(
        io_context,
        [writer]() mutable -> mcp::Task<void> {
            co_await writer.write_message(std::make_shared<const std::string>("third"));
        },
        boost::asio::detached);

    io_context.run();

    EXPECT_EQ(transport->max_active_writes(), 1);
    EXPECT_EQ(transport->messages(), (std::vector<std::string>{"first", "second", "third"}));
}

TEST(SerializedTransportWriterTest, ConcurrentCallersStaySerializedAcrossThreadPool) {
    constexpr int round_count = 4;
    constexpr int message_count = 128;

    for (int round = 0; round < round_count; ++round) {
        boost::asio::io_context io_context;
        auto transport = std::make_shared<OverlapDetectingTransport>();
        mcp::detail::SerializedTransportWriter writer(transport, io_context.get_executor());

        std::atomic<int> completions{0};
        std::atomic<int> failures{0};
        std::vector<std::string> expected;
        expected.reserve(message_count);

        for (int index = 0; index < message_count; ++index) {
            auto message = "round-" + std::to_string(round) + "-message-" + std::to_string(index);
            expected.push_back(message);
            boost::asio::co_spawn(
                io_context,
                [writer, message = std::move(message)]() mutable -> mcp::Task<void> {
                    co_await writer.write_message(message);
                },
                [&completions, &failures](std::exception_ptr error) {
                    if (error) {
                        failures.fetch_add(1, std::memory_order_relaxed);
                    }
                    completions.fetch_add(1, std::memory_order_release);
                });
        }

        std::vector<std::thread> workers;
        workers.reserve(4);
        for (int index = 0; index < 4; ++index) {
            workers.emplace_back([&io_context]() { io_context.run(); });
        }
        for (auto& worker : workers) {
            worker.join();
        }

        auto actual = transport->messages();
        std::sort(actual.begin(), actual.end());
        std::sort(expected.begin(), expected.end());

        EXPECT_EQ(failures.load(std::memory_order_acquire), 0) << "round " << round;
        EXPECT_EQ(completions.load(std::memory_order_acquire), message_count) << "round " << round;
        EXPECT_EQ(transport->max_active_writes(), 1) << "round " << round;
        EXPECT_EQ(actual, expected) << "round " << round;
    }
}

TEST(SerializedTransportWriterTest, PendingWriteRetainsStateAfterWriterDestruction) {
    boost::asio::io_context io_context;
    auto transport = std::make_shared<OverlapDetectingTransport>();

    auto pending_write = [&]() {
        mcp::detail::SerializedTransportWriter writer(transport, io_context.get_executor());
        return writer.write_message("retained message");
    }();

    std::exception_ptr error;
    boost::asio::co_spawn(io_context, std::move(pending_write),
                          [&error](std::exception_ptr result) { error = std::move(result); });
    io_context.run();

    EXPECT_EQ(error, nullptr);
    EXPECT_EQ(transport->messages(), (std::vector<std::string>{"retained message"}));
}

// A message stops counting in queued_bytes() once it is written, and so does one called off
// before its write started.
TEST(SerializedTransportWriterTest, WrittenAndCanceledMessagesStopCounting) {
    boost::asio::io_context io_context;
    auto transport = std::make_shared<OverlapDetectingTransport>();
    mcp::detail::SerializedTransportWriter writer(transport, io_context.get_executor());
    auto canceled = std::make_shared<std::atomic<bool>>(true);

    std::exception_ptr first_error;
    std::exception_ptr canceled_error;
    boost::asio::co_spawn(io_context,
                          writer.write_message(std::make_shared<const std::string>("first")),
                          [&](std::exception_ptr error) { first_error = std::move(error); });
    boost::asio::co_spawn(
        io_context, writer.write_message(std::make_shared<const std::string>("called off"), canceled),
        [&](std::exception_ptr error) { canceled_error = std::move(error); });
    io_context.run();

    EXPECT_EQ(first_error, nullptr);
    EXPECT_NE(canceled_error, nullptr);
    EXPECT_EQ(transport->messages(), (std::vector<std::string>{"first"}));
    EXPECT_EQ(writer.queued_bytes(), 0U);
}

// The server runs a session on one strand and gives that strand to its writer, which then queues
// and drains on it instead of on a strand of its own.
TEST(SerializedTransportWriterTest, WritesStaySerializedOnAStrandTheWriterIsGiven) {
    constexpr int context_writers = 64;

    boost::asio::io_context io_context;
    const auto strand = mcp::detail::make_strand_executor(io_context.get_executor());
    auto transport = std::make_shared<OverlapDetectingTransport>();
    mcp::detail::SerializedTransportWriter writer(transport, strand);

    std::atomic<int> failures{0};
    const auto count_failure = [&failures](std::exception_ptr error) {
        if (error) {
            failures.fetch_add(1, std::memory_order_relaxed);
        }
    };

    boost::asio::co_spawn(
        strand,
        [writer]() mutable -> mcp::Task<void> {
            co_await writer.write_message(std::string{"first"});
            co_await writer.write_message(std::string{"second"});
        },
        count_failure);
    for (int index = 0; index < context_writers; ++index) {
        boost::asio::co_spawn(
            io_context,
            [writer, message = "context-" + std::to_string(index)]() mutable -> mcp::Task<void> {
                co_await writer.write_message(message);
            },
            count_failure);
    }

    std::vector<std::thread> workers;
    workers.reserve(4);
    for (int index = 0; index < 4; ++index) {
        workers.emplace_back([&io_context]() { io_context.run(); });
    }
    for (auto& worker : workers) {
        worker.join();
    }

    const auto messages = transport->messages();
    const auto first = std::find(messages.begin(), messages.end(), "first");
    const auto second = std::find(messages.begin(), messages.end(), "second");
    EXPECT_EQ(failures.load(std::memory_order_acquire), 0);
    EXPECT_EQ(transport->max_active_writes(), 1);
    EXPECT_EQ(messages.size(), static_cast<std::size_t>(context_writers + 2));
    ASSERT_NE(first, messages.end());
    ASSERT_NE(second, messages.end());
    EXPECT_LT(first, second);
}

/// Records each message with the writer's queued bytes at the time, and fails the writes it is told
/// to fail. Writes suspend for a millisecond.
class RecordingTransport final : public mcp::ITransport {
   public:
    mcp::Task<std::string> read_message() override {
        throw std::runtime_error("read_message is not used by this test");
        co_return std::string{};
    }

    mcp::Task<void> write_message(std::string_view message) override {
        const auto executor = co_await boost::asio::this_coro::executor;
        if (closed) {
            throw std::runtime_error("transport closed");
        }
        if (++active > max_active) {
            max_active = active;
        }
        messages.emplace_back(message);
        queued.push_back(writer != nullptr ? writer->queued_bytes() : 0);
        if (on_write) {
            on_write(messages.back());
        }
        boost::asio::steady_timer delay(executor, write_time);
        delays.push_back(&delay);
        boost::system::error_code error;
        co_await delay.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, error));
        delays.erase(std::find(delays.begin(), delays.end(), &delay));
        if (error == boost::asio::error::operation_aborted) {
            ++cut_short;
        }
        --active;
        if (closed) {
            throw std::runtime_error("transport closed during the write");
        }
        if (message == fail_message) {
            throw std::runtime_error("write failed");
        }
    }

    /// Fails the write that is running and every later one.
    void close() override {
        closed = true;
        for (auto* delay : delays) {
            delay->cancel();
        }
    }

    const mcp::detail::SerializedTransportWriter* writer = nullptr;
    std::string fail_message;
    std::chrono::milliseconds write_time{1};
    /// Called on the strand as each write starts, with its message.
    std::function<void(const std::string&)> on_write;
    std::vector<std::string> messages;
    std::vector<std::size_t> queued;
    int active = 0;
    int max_active = 0;
    /// Writes that close() ended before their time was up.
    int cut_short = 0;
    bool closed = false;

   private:
    std::vector<boost::asio::steady_timer*> delays;
};

// Callers on the writer's strand write at once when nothing is ahead of them; those that arrive
// while a write runs queue behind it in the order they called.
TEST(SerializedTransportWriterTest, CallersOnTheStrandWriteInTheOrderTheyCall) {
    constexpr int writer_count = 16;
    boost::asio::io_context io_context;
    const auto strand = mcp::detail::make_strand_executor(io_context.get_executor());
    auto transport = std::make_shared<RecordingTransport>();
    mcp::detail::SerializedTransportWriter writer(transport, strand);
    transport->writer = &writer;

    std::vector<std::string> expected;
    for (int index = 0; index < writer_count; ++index) {
        expected.push_back("message-" + std::to_string(index));
        boost::asio::co_spawn(
            strand,
            [writer, message = expected.back()]() mutable -> mcp::Task<void> {
                co_await writer.write_message(message);
            },
            boost::asio::detached);
    }
    io_context.run();

    EXPECT_EQ(transport->messages, expected);
    // A message counts in queued_bytes() while it is written, written at once or queued.
    ASSERT_FALSE(transport->queued.empty());
    EXPECT_GE(transport->queued.front(), expected.front().size());
    EXPECT_EQ(writer.queued_bytes(), 0U);
}

// A message posted with post_message() is written before one that a caller on the strand writes
// after it, even while the post has not yet reached the strand.
TEST(SerializedTransportWriterTest, PostedMessageIsWrittenBeforeALaterWriteOnTheStrand) {
    boost::asio::io_context io_context;
    const auto strand = mcp::detail::make_strand_executor(io_context.get_executor());
    auto transport = std::make_shared<RecordingTransport>();
    mcp::detail::SerializedTransportWriter writer(transport, strand);

    boost::asio::co_spawn(
        strand,
        [writer]() mutable -> mcp::Task<void> {
            writer.post_message(std::make_shared<const std::string>("posted"));
            co_await writer.write_message(std::string{"written"});
        },
        boost::asio::detached);
    io_context.run();

    EXPECT_EQ(transport->messages, (std::vector<std::string>{"posted", "written"}));
}

// On the strand as off it, a failed write fails only its own call, and a message cancelled before
// its write starts is not written.
TEST(SerializedTransportWriterTest, FailedOrCancelledWriteOnTheStrandFailsOnlyItsCall) {
    boost::asio::io_context io_context;
    const auto strand = mcp::detail::make_strand_executor(io_context.get_executor());
    auto transport = std::make_shared<RecordingTransport>();
    transport->fail_message = "fails";
    mcp::detail::SerializedTransportWriter writer(transport, strand);

    bool failed = false;
    bool cancelled = false;
    bool later_written = false;
    boost::asio::co_spawn(
        strand,
        [&, writer]() mutable -> mcp::Task<void> {
            try {
                co_await writer.write_message(std::string{"fails"});
            } catch (const std::runtime_error&) {
                failed = true;
            }
            auto cancel = std::make_shared<std::atomic<bool>>(true);
            try {
                co_await writer.write_message(std::make_shared<const std::string>("cancelled"), cancel);
            } catch (const boost::system::system_error& error) {
                cancelled = error.code() == boost::asio::error::operation_aborted;
            }
            co_await writer.write_message(std::string{"later"});
            later_written = true;
        },
        boost::asio::detached);
    io_context.run();

    EXPECT_TRUE(failed);
    EXPECT_TRUE(cancelled);
    EXPECT_TRUE(later_written);
    EXPECT_EQ(transport->messages, (std::vector<std::string>{"fails", "later"}));
    EXPECT_EQ(writer.queued_bytes(), 0U);
}

// A write the transport starts from inside a write, by posting a message or by a coroutine that
// writes, queues behind the write that is running instead of overlapping it.
TEST(SerializedTransportWriterTest, WritesStartedFromInsideAWriteQueueBehindIt) {
    boost::asio::io_context io_context;
    const auto strand = mcp::detail::make_strand_executor(io_context.get_executor());
    auto transport = std::make_shared<RecordingTransport>();
    mcp::detail::SerializedTransportWriter writer(transport, strand);
    transport->writer = &writer;
    bool inner_written = false;
    transport->on_write = [&](const std::string& message) {
        if (message != "outer") {
            return;
        }
        writer.post_message(std::make_shared<const std::string>("posted-inside"));
        boost::asio::co_spawn(
            strand,
            [&inner_written, writer]() mutable -> mcp::Task<void> {
                co_await writer.write_message(std::string{"written-inside"});
                inner_written = true;
            },
            boost::asio::detached);
    };

    bool outer_written = false;
    boost::asio::co_spawn(
        strand,
        [&outer_written, writer]() mutable -> mcp::Task<void> {
            co_await writer.write_message(std::string{"outer"});
            outer_written = true;
        },
        boost::asio::detached);
    io_context.run();

    EXPECT_TRUE(outer_written);
    EXPECT_TRUE(inner_written);
    EXPECT_EQ(transport->messages,
              (std::vector<std::string>{"outer", "posted-inside", "written-inside"}));
    EXPECT_EQ(transport->max_active, 1);
    EXPECT_EQ(writer.queued_bytes(), 0U);
}

// A queued message cancelled while a write made in place runs is not written; the messages queued
// with it still are.
TEST(SerializedTransportWriterTest, CancellingAMessageQueuedBehindAWriteInPlaceSkipsOnlyIt) {
    boost::asio::io_context io_context;
    const auto strand = mcp::detail::make_strand_executor(io_context.get_executor());
    auto transport = std::make_shared<RecordingTransport>();
    mcp::detail::SerializedTransportWriter writer(transport, strand);
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    bool cancelled = false;
    bool after_written = false;
    transport->on_write = [&](const std::string& message) {
        if (message != "first") {
            return;
        }
        boost::asio::co_spawn(
            strand,
            [&cancelled, writer, cancel]() mutable -> mcp::Task<void> {
                try {
                    co_await writer.write_message(std::make_shared<const std::string>("cancelled"),
                                                  cancel);
                } catch (const boost::system::system_error& error) {
                    cancelled = error.code() == boost::asio::error::operation_aborted;
                }
            },
            boost::asio::detached);
        boost::asio::co_spawn(
            strand,
            [&after_written, writer]() mutable -> mcp::Task<void> {
                co_await writer.write_message(std::string{"after"});
                after_written = true;
            },
            boost::asio::detached);
        cancel->store(true);
    };

    boost::asio::co_spawn(
        strand,
        [writer]() mutable -> mcp::Task<void> { co_await writer.write_message(std::string{"first"}); },
        boost::asio::detached);
    io_context.run();

    EXPECT_TRUE(cancelled);
    EXPECT_TRUE(after_written);
    EXPECT_EQ(transport->messages, (std::vector<std::string>{"first", "after"}));
    EXPECT_EQ(writer.queued_bytes(), 0U);
}

// A write made in place that fails fails only its own call: what queued behind it is written.
TEST(SerializedTransportWriterTest, FailedWriteInPlaceStillWritesWhatQueuedBehindIt) {
    boost::asio::io_context io_context;
    const auto strand = mcp::detail::make_strand_executor(io_context.get_executor());
    auto transport = std::make_shared<RecordingTransport>();
    transport->fail_message = "fails";
    mcp::detail::SerializedTransportWriter writer(transport, strand);
    int queued_written = 0;
    transport->on_write = [&](const std::string& message) {
        if (message != "fails") {
            return;
        }
        writer.post_message(std::make_shared<const std::string>("posted"));
        for (const auto* text : {"queued-1", "queued-2"}) {
            boost::asio::co_spawn(
                strand,
                [&queued_written, writer, text = std::string(text)]() mutable -> mcp::Task<void> {
                    co_await writer.write_message(text);
                    ++queued_written;
                },
                boost::asio::detached);
        }
    };

    bool failed = false;
    boost::asio::co_spawn(
        strand,
        [&failed, writer]() mutable -> mcp::Task<void> {
            try {
                co_await writer.write_message(std::string{"fails"});
            } catch (const std::runtime_error&) {
                failed = true;
            }
        },
        boost::asio::detached);
    io_context.run();

    EXPECT_TRUE(failed);
    EXPECT_EQ(queued_written, 2);
    EXPECT_EQ(transport->messages,
              (std::vector<std::string>{"fails", "posted", "queued-1", "queued-2"}));
    EXPECT_EQ(writer.queued_bytes(), 0U);
}

// Closing the transport while a write made in place runs fails that write and each one queued
// behind it; none of them is left waiting.
TEST(SerializedTransportWriterTest, CloseDuringAWriteInPlaceFailsItAndTheQueue) {
    boost::asio::io_context io_context;
    const auto strand = mcp::detail::make_strand_executor(io_context.get_executor());
    auto transport = std::make_shared<RecordingTransport>();
    transport->write_time = std::chrono::milliseconds(10000);
    mcp::detail::SerializedTransportWriter writer(transport, strand);
    int failures = 0;
    const auto write = [&failures, writer](std::shared_ptr<const std::string> text) -> mcp::Task<void> {
        try {
            co_await writer.write_message(text);
        } catch (const std::runtime_error&) {
            ++failures;
        }
    };
    transport->on_write = [&](const std::string& message) {
        if (message != "first") {
            return;
        }
        boost::asio::co_spawn(strand, write(std::make_shared<const std::string>("second")),
                              boost::asio::detached);
        boost::asio::co_spawn(strand, write(std::make_shared<const std::string>("third")),
                              boost::asio::detached);
        boost::asio::post(strand, [transport] { transport->close(); });
    };

    boost::asio::co_spawn(strand, write(std::make_shared<const std::string>("first")),
                          boost::asio::detached);
    io_context.run();

    EXPECT_EQ(failures, 3);
    EXPECT_EQ(transport->messages, (std::vector<std::string>{"first"}));
    EXPECT_EQ(writer.queued_bytes(), 0U);
    // The close ended the write, rather than its own ten seconds running out.
    EXPECT_EQ(transport->cut_short, 1);
}

}  // namespace

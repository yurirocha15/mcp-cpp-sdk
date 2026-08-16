#include <mcp/detail/serialized_transport_writer.hpp>
#include <mcp/transport/transport.hpp>

#include <gtest/gtest.h>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
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

}  // namespace

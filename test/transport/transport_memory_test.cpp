#include "mcp/transport/memory.hpp"

#include <gtest/gtest.h>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

class MemoryTransportTest : public ::testing::Test {
   protected:
    boost::asio::io_context io_ctx_;
};

TEST_F(MemoryTransportTest, CreatePair) {
    auto [transport_a, transport_b] = mcp::create_memory_transport_pair(io_ctx_.get_executor());
    ASSERT_NE(transport_a, nullptr);
    ASSERT_NE(transport_b, nullptr);
}

TEST_F(MemoryTransportTest, SendFromAReceiveOnB) {
    auto [transport_a, transport_b] = mcp::create_memory_transport_pair(io_ctx_.get_executor());

    bool success = false;
    std::string received_message;

    boost::asio::co_spawn(
        io_ctx_,
        [ta = std::move(transport_a), tb = std::move(transport_b), &success,
         &received_message]() -> mcp::Task<void> {
            co_await ta->write_message("hello from A");
            received_message = co_await tb->read_message();
            success = true;
        },
        boost::asio::detached);

    io_ctx_.run();

    EXPECT_TRUE(success);
    EXPECT_EQ(received_message, "hello from A");
}

TEST_F(MemoryTransportTest, SendFromBReceiveOnA) {
    auto [transport_a, transport_b] = mcp::create_memory_transport_pair(io_ctx_.get_executor());

    bool success = false;
    std::string received_message;

    boost::asio::co_spawn(
        io_ctx_,
        [ta = std::move(transport_a), tb = std::move(transport_b), &success,
         &received_message]() -> mcp::Task<void> {
            co_await tb->write_message("hello from B");
            received_message = co_await ta->read_message();
            success = true;
        },
        boost::asio::detached);

    io_ctx_.run();

    EXPECT_TRUE(success);
    EXPECT_EQ(received_message, "hello from B");
}

TEST_F(MemoryTransportTest, MultipleMessagesInOrder) {
    auto [transport_a, transport_b] = mcp::create_memory_transport_pair(io_ctx_.get_executor());

    bool success = false;
    std::string msg1, msg2, msg3;

    boost::asio::co_spawn(
        io_ctx_,
        [ta = std::move(transport_a), tb = std::move(transport_b), &success, &msg1, &msg2,
         &msg3]() -> mcp::Task<void> {
            co_await ta->write_message("message1");
            co_await ta->write_message("message2");
            co_await ta->write_message("message3");
            msg1 = co_await tb->read_message();
            msg2 = co_await tb->read_message();
            msg3 = co_await tb->read_message();
            success = true;
        },
        boost::asio::detached);

    io_ctx_.run();

    EXPECT_TRUE(success);
    EXPECT_EQ(msg1, "message1");
    EXPECT_EQ(msg2, "message2");
    EXPECT_EQ(msg3, "message3");
}

TEST_F(MemoryTransportTest, BidirectionalCommunication) {
    auto [transport_a, transport_b] = mcp::create_memory_transport_pair(io_ctx_.get_executor());

    bool success = false;
    std::string a_received, b_received;

    boost::asio::co_spawn(
        io_ctx_,
        [ta = std::move(transport_a), tb = std::move(transport_b), &success, &a_received,
         &b_received]() -> mcp::Task<void> {
            co_await ta->write_message("A to B");
            co_await tb->write_message("B to A");
            a_received = co_await ta->read_message();
            b_received = co_await tb->read_message();
            success = true;
        },
        boost::asio::detached);

    io_ctx_.run();

    EXPECT_TRUE(success);
    EXPECT_EQ(a_received, "B to A");
    EXPECT_EQ(b_received, "A to B");
}

TEST_F(MemoryTransportTest, CloseStopsRead) {
    auto [transport_a, transport_b] = mcp::create_memory_transport_pair(io_ctx_.get_executor());
    (void)transport_b;

    bool exception_thrown = false;

    boost::asio::co_spawn(
        io_ctx_,
        [ta = std::move(transport_a), &exception_thrown]() -> mcp::Task<void> {
            ta->close();
            try {
                co_await ta->read_message();
            } catch (const std::runtime_error& e) {
                exception_thrown = true;
                EXPECT_STREQ(e.what(), "transport closed");
            }
        },
        boost::asio::detached);

    io_ctx_.run();

    EXPECT_TRUE(exception_thrown);
}

TEST_F(MemoryTransportTest, CloseOnOneSideAffectsOther) {
    auto [transport_a, transport_b] = mcp::create_memory_transport_pair(io_ctx_.get_executor());

    bool exception_thrown = false;

    boost::asio::co_spawn(
        io_ctx_,
        [ta = std::move(transport_a), tb = std::move(transport_b),
         &exception_thrown]() -> mcp::Task<void> {
            ta->close();
            try {
                co_await tb->read_message();
            } catch (const std::runtime_error& e) {
                exception_thrown = true;
                EXPECT_STREQ(e.what(), "transport closed");
            }
        },
        boost::asio::detached);

    io_ctx_.run();

    EXPECT_TRUE(exception_thrown);
}

TEST_F(MemoryTransportTest, CloseIsIdempotent) {
    auto [transport_a, transport_b] = mcp::create_memory_transport_pair(io_ctx_.get_executor());
    (void)transport_b;

    transport_a->close();
    transport_a->close();
    transport_a->close();

    bool exception_thrown = false;

    boost::asio::co_spawn(
        io_ctx_,
        [ta = std::move(transport_a), &exception_thrown]() -> mcp::Task<void> {
            try {
                co_await ta->read_message();
            } catch (const std::runtime_error& e) {
                exception_thrown = true;
                EXPECT_STREQ(e.what(), "transport closed");
            }
        },
        boost::asio::detached);

    io_ctx_.run();

    EXPECT_TRUE(exception_thrown);
}

TEST_F(MemoryTransportTest, WriteAfterCloseThrows) {
    auto [transport_a, transport_b] = mcp::create_memory_transport_pair(io_ctx_.get_executor());
    (void)transport_b;

    bool exception_thrown = false;

    boost::asio::co_spawn(
        io_ctx_,
        [ta = std::move(transport_a), &exception_thrown]() -> mcp::Task<void> {
            ta->close();
            try {
                co_await ta->write_message("test");
            } catch (const std::runtime_error& e) {
                exception_thrown = true;
                EXPECT_STREQ(e.what(), "transport closed");
            }
        },
        boost::asio::detached);

    io_ctx_.run();

    EXPECT_TRUE(exception_thrown);
}

TEST_F(MemoryTransportTest, WriteOwnsStringViewBeforeTaskIsAwaited) {
    auto [transport_a, transport_b] = mcp::create_memory_transport_pair(io_ctx_.get_executor());

    std::string source = "original message";
    std::string received_message;

    boost::asio::co_spawn(
        io_ctx_,
        [ta = std::move(transport_a), tb = std::move(transport_b), &source,
         &received_message]() mutable -> mcp::Task<void> {
            auto pending_write = ta->write_message(source);
            source.assign("changed after write_message returned");

            co_await std::move(pending_write);
            received_message = co_await tb->read_message();
        },
        boost::asio::detached);

    io_ctx_.run();

    EXPECT_EQ(received_message, "original message");
}

TEST_F(MemoryTransportTest, PendingReadRetainsEndpointAfterWrapperDestruction) {
    auto [transport_a, transport_b] = mcp::create_memory_transport_pair(io_ctx_.get_executor());

    std::string received_message;

    boost::asio::co_spawn(
        io_ctx_,
        [ta = std::move(transport_a), tb = std::move(transport_b),
         &received_message]() mutable -> mcp::Task<void> {
            auto pending_read = tb->read_message();
            tb.reset();

            co_await ta->write_message("message for retained endpoint");
            received_message = co_await std::move(pending_read);
        },
        boost::asio::detached);

    io_ctx_.run();

    EXPECT_EQ(received_message, "message for retained endpoint");
}

TEST_F(MemoryTransportTest, DestroyingIdleEndpointWakesPeerRead) {
    auto [transport_a, transport_b] = mcp::create_memory_transport_pair(io_ctx_.get_executor());

    bool peer_closed = false;
    boost::asio::co_spawn(
        io_ctx_,
        [transport_a = std::move(transport_a), transport_b = std::move(transport_b),
         &peer_closed]() mutable -> mcp::Task<void> {
            auto pending_read = transport_b->read_message();
            transport_a.reset();

            try {
                static_cast<void>(co_await std::move(pending_read));
            } catch (const std::runtime_error& error) {
                peer_closed = true;
                EXPECT_STREQ(error.what(), "transport closed");
            }
        },
        boost::asio::detached);

    io_ctx_.run();

    EXPECT_TRUE(peer_closed);
}

TEST_F(MemoryTransportTest, JsonMessagesRoundTripWithoutSerializationAtTheWriter) {
    auto [base_a, base_b] = mcp::create_memory_transport_pair(io_ctx_.get_executor());
    auto transport_a = std::dynamic_pointer_cast<mcp::MemoryTransport>(base_a);
    auto transport_b = std::dynamic_pointer_cast<mcp::MemoryTransport>(base_b);
    ASSERT_NE(transport_a, nullptr);
    ASSERT_NE(transport_b, nullptr);

    const nlohmann::json expected = {{"jsonrpc", "2.0"}, {"id", 7}, {"result", {}}};
    nlohmann::json received;

    boost::asio::co_spawn(
        io_ctx_,
        [transport_a = std::move(transport_a), transport_b = std::move(transport_b), expected,
         &received]() mutable -> mcp::Task<void> {
            co_await transport_a->write_json(expected);
            received = co_await transport_b->read_json();
        },
        boost::asio::detached);

    io_ctx_.run();

    EXPECT_EQ(received, expected);
}

TEST_F(MemoryTransportTest, ConcurrentCloseWakesPendingReadExactlyOnce) {
    auto [transport_a, transport_b] = mcp::create_memory_transport_pair(io_ctx_.get_executor());

    int close_errors = 0;
    boost::asio::co_spawn(
        io_ctx_,
        [transport_a, &close_errors]() -> mcp::Task<void> {
            try {
                (void)co_await transport_a->read_message();
            } catch (const std::runtime_error& error) {
                EXPECT_STREQ(error.what(), "transport closed");
                ++close_errors;
            }
        },
        boost::asio::detached);

    io_ctx_.poll();

    std::vector<std::thread> closers;
    closers.reserve(8);
    for (int index = 0; index < 8; ++index) {
        auto transport = index % 2 == 0 ? transport_a : transport_b;
        closers.emplace_back([transport]() { transport->close(); });
    }
    for (auto& closer : closers) {
        closer.join();
    }

    io_ctx_.restart();
    io_ctx_.run();

    EXPECT_EQ(close_errors, 1);
}

TEST_F(MemoryTransportTest, ConcurrentReadWriteIsSafeAcrossThreadPool) {
    constexpr int round_count = 4;
    constexpr int message_count = 256;

    for (int round = 0; round < round_count; ++round) {
        boost::asio::io_context io_context;
        auto [transport_a, transport_b] = mcp::create_memory_transport_pair(io_context.get_executor());

        std::atomic<int> write_completions{0};
        std::atomic<int> failures{0};
        std::vector<std::string> received;
        received.reserve(message_count);

        boost::asio::co_spawn(
            io_context,
            [transport_b, &received]() -> mcp::Task<void> {
                for (int index = 0; index < message_count; ++index) {
                    received.push_back(co_await transport_b->read_message());
                }
            },
            [&failures](std::exception_ptr error) {
                if (error) {
                    failures.fetch_add(1, std::memory_order_relaxed);
                }
            });

        std::vector<std::string> expected;
        expected.reserve(message_count);
        for (int index = 0; index < message_count; ++index) {
            auto message = "round-" + std::to_string(round) + "-message-" + std::to_string(index);
            expected.push_back(message);
            boost::asio::co_spawn(
                io_context,
                [transport_a, message = std::move(message)]() -> mcp::Task<void> {
                    co_await transport_a->write_message(message);
                },
                [&write_completions, &failures](std::exception_ptr error) {
                    if (error) {
                        failures.fetch_add(1, std::memory_order_relaxed);
                    }
                    write_completions.fetch_add(1, std::memory_order_release);
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

        std::sort(received.begin(), received.end());
        std::sort(expected.begin(), expected.end());
        EXPECT_EQ(failures.load(std::memory_order_acquire), 0) << "round " << round;
        EXPECT_EQ(write_completions.load(std::memory_order_acquire), message_count)
            << "round " << round;
        EXPECT_EQ(received, expected) << "round " << round;
    }
}

TEST_F(MemoryTransportTest, MultiplePendingReadersReceiveDistinctMessages) {
    boost::asio::io_context io_context;
    auto [transport_a, transport_b] = mcp::create_memory_transport_pair(io_context.get_executor());

    constexpr int reader_count = 64;
    std::atomic<int> failures{0};
    std::vector<std::string> received(reader_count);

    for (int index = 0; index < reader_count; ++index) {
        boost::asio::co_spawn(
            io_context,
            [transport_b, &received, index]() -> mcp::Task<void> {
                received[index] = co_await transport_b->read_message();
            },
            [&failures](std::exception_ptr error) {
                if (error) {
                    failures.fetch_add(1, std::memory_order_relaxed);
                }
            });
    }

    // Install every pending read before any message is delivered. This specifically
    // exercises independent waiters rather than the already-queued fast path.
    io_context.poll();
    io_context.restart();

    std::vector<std::string> expected;
    expected.reserve(reader_count);
    for (int index = 0; index < reader_count; ++index) {
        auto message = "pending-reader-message-" + std::to_string(index);
        expected.push_back(message);
        boost::asio::co_spawn(
            io_context,
            [transport_a, message = std::move(message)]() -> mcp::Task<void> {
                co_await transport_a->write_message(message);
            },
            [&failures](std::exception_ptr error) {
                if (error) {
                    failures.fetch_add(1, std::memory_order_relaxed);
                }
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

    std::sort(received.begin(), received.end());
    std::sort(expected.begin(), expected.end());
    EXPECT_EQ(failures.load(std::memory_order_acquire), 0);
    EXPECT_EQ(received, expected);
}

TEST_F(MemoryTransportTest, ConcurrentCloseWakesReadersOnRunningThreadPool) {
    boost::asio::io_context io_context;
    auto work_guard = boost::asio::make_work_guard(io_context);
    auto [transport_a, transport_b] = mcp::create_memory_transport_pair(io_context.get_executor());

    constexpr int reader_count = 32;
    std::atomic<int> started{0};
    std::atomic<int> close_errors{0};
    std::atomic<int> unexpected_completions{0};
    std::mutex started_mutex;
    std::condition_variable started_condition;

    for (int index = 0; index < reader_count; ++index) {
        boost::asio::co_spawn(
            io_context,
            [transport_a, &started, &close_errors, &unexpected_completions,
             &started_condition]() -> mcp::Task<void> {
                started.fetch_add(1, std::memory_order_release);
                started_condition.notify_one();
                try {
                    (void)co_await transport_a->read_message();
                    unexpected_completions.fetch_add(1, std::memory_order_relaxed);
                } catch (const std::runtime_error& error) {
                    if (std::string_view(error.what()) == "transport closed") {
                        close_errors.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        unexpected_completions.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            },
            boost::asio::detached);
    }

    std::vector<std::thread> workers;
    workers.reserve(4);
    for (int index = 0; index < 4; ++index) {
        workers.emplace_back([&io_context]() { io_context.run(); });
    }

    {
        std::unique_lock lock(started_mutex);
        started_condition.wait(
            lock, [&started]() { return started.load(std::memory_order_acquire) == reader_count; });
    }

    std::vector<std::thread> closers;
    closers.reserve(8);
    for (int index = 0; index < 8; ++index) {
        auto transport = index % 2 == 0 ? transport_a : transport_b;
        closers.emplace_back([transport]() { transport->close(); });
    }
    for (auto& closer : closers) {
        closer.join();
    }

    work_guard.reset();
    for (auto& worker : workers) {
        worker.join();
    }

    EXPECT_EQ(close_errors.load(std::memory_order_acquire), reader_count);
    EXPECT_EQ(unexpected_completions.load(std::memory_order_acquire), 0);
}

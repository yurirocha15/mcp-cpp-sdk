#include "mcp/transport/memory.hpp"

#include <gtest/gtest.h>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

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

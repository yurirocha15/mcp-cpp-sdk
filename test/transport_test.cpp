#include "mcp/transport.hpp"

#include <gtest/gtest.h>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <memory>
#include <queue>
#include <string>
#include <string_view>

class MockTransport final : public mcp::ITransport {
   public:
    void enqueue(std::string msg) { messages_.push(std::move(msg)); }

    mcp::Task<std::string> read_message() override {
        co_await boost::asio::this_coro::executor;
        if (messages_.empty()) {
            throw std::runtime_error("no messages");
        }
        auto msg = std::move(messages_.front());
        messages_.pop();
        co_return msg;
    }

    mcp::Task<void> write_message(std::string_view message) override {
        co_await boost::asio::this_coro::executor;
        written_.emplace_back(message);
        co_return;
    }

    void close() override { closed_ = true; }

    [[nodiscard]] bool closed() const { return closed_; }
    [[nodiscard]] const std::vector<std::string>& written() const { return written_; }

   private:
    std::queue<std::string> messages_;
    std::vector<std::string> written_;
    bool closed_ = false;
};

class TransportTest : public ::testing::Test {
   protected:
    boost::asio::io_context io_ctx_;
};

TEST_F(TransportTest, MockImplementsInterface) {
    std::unique_ptr<mcp::ITransport> transport = std::make_unique<MockTransport>();
    EXPECT_NE(transport, nullptr);
}

TEST_F(TransportTest, ReadMessageReturnsEnqueued) {
    auto mock = std::make_unique<MockTransport>();
    mock->enqueue(R"({"jsonrpc":"2.0","method":"ping"})");

    std::string result;
    boost::asio::co_spawn(
        io_ctx_, [&]() -> mcp::Task<void> { result = co_await mock->read_message(); },
        boost::asio::detached);
    io_ctx_.run();

    EXPECT_EQ(result, R"({"jsonrpc":"2.0","method":"ping"})");
}

TEST_F(TransportTest, WriteMessageStoresOutput) {
    auto mock = std::make_unique<MockTransport>();

    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            co_await mock->write_message(R"({"jsonrpc":"2.0","id":1,"result":{}})");
        },
        boost::asio::detached);
    io_ctx_.run();

    ASSERT_EQ(mock->written().size(), 1);
    EXPECT_EQ(mock->written()[0], R"({"jsonrpc":"2.0","id":1,"result":{}})");
}

TEST_F(TransportTest, CloseMarksTransportClosed) {
    MockTransport mock;
    EXPECT_FALSE(mock.closed());
    mock.close();
    EXPECT_TRUE(mock.closed());
}

TEST_F(TransportTest, ReadEmptyThrows) {
    auto mock = std::make_unique<MockTransport>();

    std::exception_ptr ex;
    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            try {
                co_await mock->read_message();
            } catch (...) {
                ex = std::current_exception();
            }
        },
        boost::asio::detached);
    io_ctx_.run();

    ASSERT_NE(ex, nullptr);
    EXPECT_THROW(std::rethrow_exception(ex), std::runtime_error);
}

TEST_F(TransportTest, PolymorphicThroughBasePointer) {
    std::unique_ptr<mcp::ITransport> transport = std::make_unique<MockTransport>();
    auto* mock = dynamic_cast<MockTransport*>(transport.get());
    mock->enqueue("hello");

    std::string result;
    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            result = co_await transport->read_message();
            co_await transport->write_message("world");
        },
        boost::asio::detached);
    io_ctx_.run();

    EXPECT_EQ(result, "hello");
    ASSERT_EQ(mock->written().size(), 1);
    EXPECT_EQ(mock->written()[0], "world");

    transport->close();
    EXPECT_TRUE(mock->closed());
}

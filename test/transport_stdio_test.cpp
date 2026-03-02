#include "mcp/transport/stdio.hpp"

#include <gtest/gtest.h>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <sstream>
#include <string>

class StdioTransportTest : public ::testing::Test {
   protected:
    boost::asio::io_context io_ctx_;
};

TEST_F(StdioTransportTest, ReadMessageReturnsPipedInput) {
    std::istringstream input("hello world\n");
    std::ostringstream output;

    mcp::StdioTransport transport(io_ctx_.get_executor(), input, output);

    std::string result;
    boost::asio::co_spawn(
        io_ctx_, [&]() -> mcp::Task<void> { result = co_await transport.read_message(); },
        boost::asio::detached);
    io_ctx_.run();

    EXPECT_EQ(result, "hello world");
}

TEST_F(StdioTransportTest, ReadMultipleMessages) {
    std::istringstream input("first\nsecond\nthird\n");
    std::ostringstream output;

    mcp::StdioTransport transport(io_ctx_.get_executor(), input, output);

    std::vector<std::string> results;
    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            results.push_back(co_await transport.read_message());
            results.push_back(co_await transport.read_message());
            results.push_back(co_await transport.read_message());
        },
        boost::asio::detached);
    io_ctx_.run();

    ASSERT_EQ(results.size(), 3);
    EXPECT_EQ(results[0], "first");
    EXPECT_EQ(results[1], "second");
    EXPECT_EQ(results[2], "third");
}

TEST_F(StdioTransportTest, WriteMessageAppendsNewline) {
    std::istringstream input;
    std::ostringstream output;

    mcp::StdioTransport transport(io_ctx_.get_executor(), input, output);

    boost::asio::co_spawn(
        io_ctx_, [&]() -> mcp::Task<void> { co_await transport.write_message("test output"); },
        boost::asio::detached);
    io_ctx_.run();

    EXPECT_EQ(output.str(), "test output\n");
}

TEST_F(StdioTransportTest, WriteMultipleMessages) {
    std::istringstream input;
    std::ostringstream output;

    mcp::StdioTransport transport(io_ctx_.get_executor(), input, output);

    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            co_await transport.write_message("line1");
            co_await transport.write_message("line2");
        },
        boost::asio::detached);
    io_ctx_.run();

    EXPECT_EQ(output.str(), "line1\nline2\n");
}

TEST_F(StdioTransportTest, CloseIsIdempotent) {
    std::istringstream input;
    std::ostringstream output;

    mcp::StdioTransport transport(io_ctx_.get_executor(), input, output);
    io_ctx_.run();

    transport.close();
    transport.close();
}

TEST_F(StdioTransportTest, DestructorDoesNotHang) {
    std::istringstream input;
    std::ostringstream output;

    {
        mcp::StdioTransport transport(io_ctx_.get_executor(), input, output);
        io_ctx_.run();
    }
}

TEST_F(StdioTransportTest, ReadJsonRpcMessage) {
    std::istringstream input(R"({"jsonrpc":"2.0","method":"initialize","id":1})"
                             "\n");
    std::ostringstream output;

    mcp::StdioTransport transport(io_ctx_.get_executor(), input, output);

    std::string result;
    boost::asio::co_spawn(
        io_ctx_, [&]() -> mcp::Task<void> { result = co_await transport.read_message(); },
        boost::asio::detached);
    io_ctx_.run();

    EXPECT_EQ(result, R"({"jsonrpc":"2.0","method":"initialize","id":1})");
}

TEST_F(StdioTransportTest, PolymorphicThroughBasePointer) {
    std::istringstream input("polymorphic\n");
    std::ostringstream output;

    std::unique_ptr<mcp::ITransport> transport =
        std::make_unique<mcp::StdioTransport>(io_ctx_.get_executor(), input, output);

    std::string result;
    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            result = co_await transport->read_message();
            co_await transport->write_message("response");
        },
        boost::asio::detached);
    io_ctx_.run();

    EXPECT_EQ(result, "polymorphic");
    EXPECT_EQ(output.str(), "response\n");

    transport->close();
}

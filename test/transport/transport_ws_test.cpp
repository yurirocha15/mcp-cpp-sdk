#include "mcp/transport/websocket.hpp"

#include <gtest/gtest.h>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <memory>
#include <string>
#include <vector>

namespace {

namespace asio = boost::asio;
using tcp = asio::ip::tcp;

mcp::Task<void> echo_server(tcp::acceptor& acceptor, int echo_count) {
    auto socket = co_await acceptor.async_accept(asio::use_awaitable);
    mcp::WebSocketServerTransport transport(std::move(socket));

    for (int i = 0; i < echo_count; ++i) {
        auto msg = co_await transport.read_message();
        co_await transport.write_message(msg);
    }
}

}  // namespace

class WebSocketTransportTest : public ::testing::Test {
   protected:
    asio::io_context io_ctx_;
};

TEST_F(WebSocketTransportTest, ReadAndWriteSingleMessage) {
    tcp::acceptor acceptor(io_ctx_, tcp::endpoint(tcp::v4(), 0));
    auto port = acceptor.local_endpoint().port();

    asio::co_spawn(io_ctx_, echo_server(acceptor, 1), asio::detached);

    std::string result;
    asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            mcp::WebSocketClientTransport transport(io_ctx_.get_executor(), "127.0.0.1",
                                                    std::to_string(port));
            co_await transport.write_message("hello websocket");
            result = co_await transport.read_message();
            transport.close();
        },
        asio::detached);

    io_ctx_.run();

    EXPECT_EQ(result, "hello websocket");
}

TEST_F(WebSocketTransportTest, ReadAndWriteMultipleMessages) {
    tcp::acceptor acceptor(io_ctx_, tcp::endpoint(tcp::v4(), 0));
    auto port = acceptor.local_endpoint().port();

    asio::co_spawn(io_ctx_, echo_server(acceptor, 3), asio::detached);

    std::vector<std::string> results;
    asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            mcp::WebSocketClientTransport transport(io_ctx_.get_executor(), "127.0.0.1",
                                                    std::to_string(port));
            co_await transport.write_message("msg1");
            results.push_back(co_await transport.read_message());

            co_await transport.write_message("msg2");
            results.push_back(co_await transport.read_message());

            co_await transport.write_message("msg3");
            results.push_back(co_await transport.read_message());

            transport.close();
        },
        asio::detached);

    io_ctx_.run();

    ASSERT_EQ(results.size(), 3);
    EXPECT_EQ(results[0], "msg1");
    EXPECT_EQ(results[1], "msg2");
    EXPECT_EQ(results[2], "msg3");
}

TEST_F(WebSocketTransportTest, ReadJsonRpcMessage) {
    tcp::acceptor acceptor(io_ctx_, tcp::endpoint(tcp::v4(), 0));
    auto port = acceptor.local_endpoint().port();

    asio::co_spawn(io_ctx_, echo_server(acceptor, 1), asio::detached);

    std::string result;
    asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            mcp::WebSocketClientTransport transport(io_ctx_.get_executor(), "127.0.0.1",
                                                    std::to_string(port));
            co_await transport.write_message(R"({"jsonrpc":"2.0","method":"initialize","id":1})");
            result = co_await transport.read_message();
            transport.close();
        },
        asio::detached);

    io_ctx_.run();

    EXPECT_EQ(result, R"({"jsonrpc":"2.0","method":"initialize","id":1})");
}

TEST_F(WebSocketTransportTest, CloseIsIdempotent) {
    asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            mcp::WebSocketClientTransport transport(io_ctx_.get_executor(), "127.0.0.1", "9");
            transport.close();
            transport.close();
            co_return;
        },
        asio::detached);

    io_ctx_.run();
}

TEST_F(WebSocketTransportTest, PolymorphicThroughBasePointer) {
    tcp::acceptor acceptor(io_ctx_, tcp::endpoint(tcp::v4(), 0));
    auto port = acceptor.local_endpoint().port();

    asio::co_spawn(io_ctx_, echo_server(acceptor, 1), asio::detached);

    std::string result;
    asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            std::shared_ptr<mcp::ITransport> transport =
                std::make_shared<mcp::WebSocketClientTransport>(io_ctx_.get_executor(), "127.0.0.1",
                                                                std::to_string(port));
            co_await transport->write_message("polymorphic");
            result = co_await transport->read_message();
            transport->close();
        },
        asio::detached);

    io_ctx_.run();

    EXPECT_EQ(result, "polymorphic");
}

TEST_F(WebSocketTransportTest, ServerTransportAcceptsRawSocket) {
    tcp::acceptor acceptor(io_ctx_, tcp::endpoint(tcp::v4(), 0));
    auto port = acceptor.local_endpoint().port();

    asio::co_spawn(io_ctx_, echo_server(acceptor, 1), asio::detached);

    std::string result;
    asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            mcp::WebSocketClientTransport transport(io_ctx_.get_executor(), "127.0.0.1",
                                                    std::to_string(port));
            co_await transport.write_message("from client");
            result = co_await transport.read_message();
            transport.close();
        },
        asio::detached);

    io_ctx_.run();

    EXPECT_EQ(result, "from client");
}

#include "mcp/transport/websocket.hpp"

#include <gtest/gtest.h>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/websocket/stream.hpp>
#include <memory>
#include <string>
#include <vector>

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace ws = beast::websocket;
using tcp = asio::ip::tcp;
using WsStream = ws::stream<beast::tcp_stream>;

mcp::Task<void> echo_server(tcp::acceptor& acceptor, int echo_count) {
    auto socket = co_await acceptor.async_accept(asio::use_awaitable);

    WsStream ws_stream(std::move(socket));
    co_await ws_stream.async_accept(asio::use_awaitable);

    for (int i = 0; i < echo_count; ++i) {
        beast::flat_buffer buffer;
        co_await ws_stream.async_read(buffer, asio::use_awaitable);
        ws_stream.text(ws_stream.got_text());
        co_await ws_stream.async_write(buffer.data(), asio::use_awaitable);
    }

    co_await ws_stream.async_close(ws::close_code::normal, asio::use_awaitable);
}

mcp::Task<WsStream> connect_client(const asio::any_io_executor& executor, unsigned short port) {
    tcp::resolver resolver(executor);
    auto results =
        co_await resolver.async_resolve("127.0.0.1", std::to_string(port), asio::use_awaitable);

    WsStream ws_stream(executor);
    auto& tcp_layer = ws_stream.next_layer();
    co_await tcp_layer.async_connect(*results.begin(), asio::use_awaitable);
    co_await ws_stream.async_handshake("127.0.0.1:" + std::to_string(port), "/", asio::use_awaitable);

    co_return std::move(ws_stream);
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
            auto ws = co_await connect_client(io_ctx_.get_executor(), port);
            mcp::WebSocketTransport transport(std::move(ws));

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
            auto ws = co_await connect_client(io_ctx_.get_executor(), port);
            mcp::WebSocketTransport transport(std::move(ws));

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
            auto ws = co_await connect_client(io_ctx_.get_executor(), port);
            mcp::WebSocketTransport transport(std::move(ws));

            co_await transport.write_message(R"({"jsonrpc":"2.0","method":"initialize","id":1})");
            result = co_await transport.read_message();

            transport.close();
        },
        asio::detached);

    io_ctx_.run();

    EXPECT_EQ(result, R"({"jsonrpc":"2.0","method":"initialize","id":1})");
}

TEST_F(WebSocketTransportTest, CloseIsIdempotent) {
    tcp::acceptor acceptor(io_ctx_, tcp::endpoint(tcp::v4(), 0));
    auto port = acceptor.local_endpoint().port();

    asio::co_spawn(io_ctx_, echo_server(acceptor, 0), asio::detached);

    asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            auto ws = co_await connect_client(io_ctx_.get_executor(), port);
            mcp::WebSocketTransport transport(std::move(ws));

            transport.close();
            transport.close();
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
            auto ws = co_await connect_client(io_ctx_.get_executor(), port);
            std::unique_ptr<mcp::ITransport> transport =
                std::make_unique<mcp::WebSocketTransport>(std::move(ws));

            co_await transport->write_message("polymorphic");
            result = co_await transport->read_message();

            transport->close();
        },
        asio::detached);

    io_ctx_.run();

    EXPECT_EQ(result, "polymorphic");
}

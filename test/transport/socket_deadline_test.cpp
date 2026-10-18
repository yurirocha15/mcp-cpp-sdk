#include "../../src/detail/socket_deadline.hpp"

#include <gtest/gtest.h>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/streambuf.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/http/empty_body.hpp>
#include <boost/beast/http/read.hpp>
#include <boost/beast/http/string_body.hpp>
#include <boost/beast/http/write.hpp>
#include <boost/system/system_error.hpp>
#include <chrono>
#include <future>
#include <string>
#include <thread>

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;
using namespace std::chrono_literals;

struct ExchangeOutcome {
    bool failed{false};
    bool expired{false};
    bool socket_open{false};
};

/// One HTTP request and its response on `port`, under a SocketDeadline of `timeout`.
asio::awaitable<ExchangeOutcome> exchange(unsigned short port, std::chrono::milliseconds timeout) {
    auto executor = co_await asio::this_coro::executor;
    beast::tcp_stream stream(executor);
    co_await stream.async_connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), port),
                                  asio::use_awaitable);
    ExchangeOutcome outcome;
    {
        const mcp::detail::SocketDeadline deadline(executor, stream.socket(), timeout);
        http::request<http::empty_body> request{http::verb::get, "/", 11};
        request.set(http::field::host, "127.0.0.1");
        try {
            co_await http::async_write(stream, request, asio::use_awaitable);
            beast::flat_buffer buffer;
            http::response<http::string_body> response;
            co_await http::async_read(stream, buffer, response, asio::use_awaitable);
        } catch (const boost::system::system_error&) {
            outcome.failed = true;
        }
        outcome.expired = deadline.expired();
    }
    // A deadline that is gone must not close the socket later.
    asio::steady_timer pause(executor, timeout + 100ms);
    co_await pause.async_wait(asio::use_awaitable);
    outcome.socket_open = stream.socket().is_open();
    co_return outcome;
}

}  // namespace

// The response is several reads, and the deadline passes between two of them: the client's only io
// thread is busy when the first part of the response arrives. The exchange fails, as one cut off
// by a closed socket does, and the process carries on.
TEST(SocketDeadlineTest, ADeadlinePassingBetweenTwoReadsFailsTheExchange) {
    asio::io_context client_io;
    asio::io_context server_io;
    tcp::acceptor acceptor(server_io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const auto port = acceptor.local_endpoint().port();

    std::thread server([&]() {
        tcp::socket socket(server_io);
        acceptor.accept(socket);
        asio::streambuf request;
        asio::read_until(socket, request, "\r\n\r\n");
        asio::post(client_io, []() { std::this_thread::sleep_for(600ms); });
        std::this_thread::sleep_for(50ms);
        asio::write(socket, asio::buffer(std::string("HTTP/1.1 200 OK\r\n")));
        std::this_thread::sleep_for(1s);
    });

    auto result = asio::co_spawn(client_io, exchange(port, 200ms), asio::use_future);
    std::thread runner([&]() { client_io.run(); });
    const bool finished = result.wait_for(5s) == std::future_status::ready;
    client_io.stop();
    runner.join();
    server.join();

    ASSERT_TRUE(finished);
    const auto outcome = result.get();
    EXPECT_TRUE(outcome.failed);
    EXPECT_TRUE(outcome.expired);
}

// An exchange that completes in time is untouched, and the deadline leaves the socket open after
// it is gone, even once its time has passed.
TEST(SocketDeadlineTest, AnExchangeThatCompletesInTimeKeepsItsSocket) {
    asio::io_context client_io;
    asio::io_context server_io;
    tcp::acceptor acceptor(server_io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const auto port = acceptor.local_endpoint().port();

    std::thread server([&]() {
        tcp::socket socket(server_io);
        acceptor.accept(socket);
        asio::streambuf request;
        asio::read_until(socket, request, "\r\n\r\n");
        asio::write(socket,
                    asio::buffer(std::string("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok")));
        std::this_thread::sleep_for(1s);
    });

    auto result = asio::co_spawn(client_io, exchange(port, 200ms), asio::use_future);
    std::thread runner([&]() { client_io.run(); });
    const bool finished = result.wait_for(5s) == std::future_status::ready;
    client_io.stop();
    runner.join();
    server.join();

    ASSERT_TRUE(finished);
    const auto outcome = result.get();
    EXPECT_FALSE(outcome.failed);
    EXPECT_FALSE(outcome.expired);
    EXPECT_TRUE(outcome.socket_open);
}

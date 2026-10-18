#include "mcp/transport/websocket.hpp"

#include <gtest/gtest.h>

#include "../support/resolve_gate.hpp"
#include "../support/stalling_server.hpp"

#include <algorithm>
#include <atomic>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/bind_executor.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/streambuf.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/core/error.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/http/field.hpp>
#include <boost/beast/websocket/stream.hpp>
#include <boost/system/system_error.hpp>
#include <chrono>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace asio = boost::asio;
using tcp = asio::ip::tcp;
using namespace std::chrono_literals;

struct WriteCompletionGroup {
    WriteCompletionGroup(const asio::any_io_executor& executor, std::size_t count)
        : signal(executor), remaining(count) {
        signal.expires_at(std::chrono::steady_clock::time_point::max());
    }

    void complete(std::exception_ptr operation_error) {
        if (operation_error && !error) {
            error = operation_error;
        }
        if (--remaining == 0) {
            boost::system::error_code ignored;
            signal.cancel(ignored);
        }
    }

    mcp::Task<void> wait() {
        if (remaining != 0) {
            boost::system::error_code error_code;
            co_await signal.async_wait(asio::redirect_error(asio::use_awaitable, error_code));
            if (error_code && error_code != asio::error::operation_aborted) {
                throw boost::system::system_error(error_code);
            }
        }
        if (error) {
            std::rethrow_exception(error);
        }
    }

    asio::steady_timer signal;
    std::size_t remaining;
    std::exception_ptr error;
};

struct ReadCompletion {
    explicit ReadCompletion(const asio::any_io_executor& executor) : signal(executor) {
        signal.expires_at(std::chrono::steady_clock::time_point::max());
    }

    void complete(std::exception_ptr operation_error, std::string operation_result) {
        error = operation_error;
        result = std::move(operation_result);
        done = true;
        boost::system::error_code ignored;
        signal.cancel(ignored);
    }

    mcp::Task<void> wait() {
        if (done) {
            co_return;
        }
        boost::system::error_code error_code;
        co_await signal.async_wait(asio::redirect_error(asio::use_awaitable, error_code));
        if (error_code && error_code != asio::error::operation_aborted) {
            throw boost::system::system_error(error_code);
        }
    }

    asio::steady_timer signal;
    std::optional<std::string> result;
    std::exception_ptr error;
    bool done{false};
};

struct VoidCompletion {
    explicit VoidCompletion(const asio::any_io_executor& executor) : signal(executor) {
        signal.expires_at(std::chrono::steady_clock::time_point::max());
    }

    void complete(std::exception_ptr operation_error) {
        error = operation_error;
        done = true;
        boost::system::error_code ignored;
        signal.cancel(ignored);
    }

    mcp::Task<void> wait() {
        if (done) {
            co_return;
        }
        boost::system::error_code error_code;
        co_await signal.async_wait(asio::redirect_error(asio::use_awaitable, error_code));
        if (error_code && error_code != asio::error::operation_aborted) {
            throw boost::system::system_error(error_code);
        }
    }

    asio::steady_timer signal;
    std::exception_ptr error;
    bool done{false};
};

mcp::Task<void> wait_for(std::chrono::steady_clock::duration duration) {
    auto executor = co_await asio::this_coro::executor;
    asio::steady_timer timer(executor, duration);
    co_await timer.async_wait(asio::use_awaitable);
}

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
    void run_on_threads(std::size_t count = 4) {
        std::vector<std::thread> threads;
        threads.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            threads.emplace_back([this]() { io_ctx_.run(); });
        }
        for (auto& thread : threads) {
            thread.join();
        }
    }

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

TEST_F(WebSocketTransportTest, SerializesConcurrentClientWritesAcrossThreads) {
    constexpr std::size_t message_count = 32;
    tcp::acceptor acceptor(io_ctx_, tcp::endpoint(tcp::v4(), 0));
    auto port = acceptor.local_endpoint().port();

    std::vector<std::string> received;
    auto server = asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            auto socket = co_await acceptor.async_accept(asio::use_awaitable);
            mcp::WebSocketServerTransport transport(std::move(socket));
            for (std::size_t i = 0; i < message_count; ++i) {
                received.push_back(co_await transport.read_message());
            }
            transport.close();
        },
        asio::use_future);

    auto client = std::make_shared<mcp::WebSocketClientTransport>(io_ctx_.get_executor(), "127.0.0.1",
                                                                  std::to_string(port));
    std::vector<std::future<void>> writes;
    writes.reserve(message_count);
    for (std::size_t i = 0; i < message_count; ++i) {
        writes.push_back(asio::co_spawn(io_ctx_, client->write_message("message-" + std::to_string(i)),
                                        asio::use_future));
    }

    run_on_threads();
    for (auto& write : writes) {
        EXPECT_NO_THROW(write.get());
    }
    EXPECT_NO_THROW(server.get());

    std::sort(received.begin(), received.end());
    std::vector<std::string> expected;
    expected.reserve(message_count);
    for (std::size_t i = 0; i < message_count; ++i) {
        expected.push_back("message-" + std::to_string(i));
    }
    std::sort(expected.begin(), expected.end());
    EXPECT_EQ(received, expected);
    client->close();
}

TEST_F(WebSocketTransportTest, SerializesConcurrentServerWritesDuringHandshake) {
    constexpr std::size_t message_count = 32;
    tcp::acceptor acceptor(io_ctx_, tcp::endpoint(tcp::v4(), 0));
    auto port = acceptor.local_endpoint().port();
    auto coordinator = asio::make_strand(io_ctx_);

    auto server = asio::co_spawn(
        coordinator,
        [&]() -> mcp::Task<void> {
            auto socket = co_await acceptor.async_accept(asio::use_awaitable);
            auto transport = std::make_shared<mcp::WebSocketServerTransport>(std::move(socket));
            auto writes = std::make_shared<WriteCompletionGroup>(coordinator, message_count);
            for (std::size_t i = 0; i < message_count; ++i) {
                asio::co_spawn(
                    coordinator, transport->write_message("server-" + std::to_string(i)),
                    asio::bind_executor(coordinator, [writes, transport](std::exception_ptr error) {
                        writes->complete(error);
                    }));
            }

            EXPECT_EQ(co_await transport->read_message(), "ack");
            co_await writes->wait();
            transport->close();
        },
        asio::use_future);

    std::vector<std::string> received;
    auto client = asio::co_spawn(
        coordinator,
        [&]() -> mcp::Task<void> {
            mcp::WebSocketClientTransport transport(io_ctx_.get_executor(), "127.0.0.1",
                                                    std::to_string(port));
            for (std::size_t i = 0; i < message_count; ++i) {
                received.push_back(co_await transport.read_message());
            }
            co_await transport.write_message("ack");
            transport.close();
        },
        asio::use_future);

    run_on_threads();
    EXPECT_NO_THROW(client.get());
    EXPECT_NO_THROW(server.get());

    std::sort(received.begin(), received.end());
    std::vector<std::string> expected;
    expected.reserve(message_count);
    for (std::size_t i = 0; i < message_count; ++i) {
        expected.push_back("server-" + std::to_string(i));
    }
    std::sort(expected.begin(), expected.end());
    EXPECT_EQ(received, expected);
}

TEST_F(WebSocketTransportTest, RejectsSecondOutstandingClientRead) {
    tcp::acceptor acceptor(io_ctx_, tcp::endpoint(tcp::v4(), 0));
    auto port = acceptor.local_endpoint().port();
    auto coordinator = asio::make_strand(io_ctx_);

    auto server = asio::co_spawn(
        coordinator,
        [&]() -> mcp::Task<void> {
            auto socket = co_await acceptor.async_accept(asio::use_awaitable);
            mcp::WebSocketServerTransport transport(std::move(socket));
            EXPECT_EQ(co_await transport.read_message(), "ready");
            co_await wait_for(40ms);
            co_await transport.write_message("response");
            transport.close();
        },
        asio::use_future);

    bool rejected = false;
    auto client = asio::co_spawn(
        coordinator,
        [&]() -> mcp::Task<void> {
            mcp::WebSocketClientTransport transport(io_ctx_.get_executor(), "127.0.0.1",
                                                    std::to_string(port));
            co_await transport.write_message("ready");

            auto first_read = std::make_shared<ReadCompletion>(coordinator);
            asio::co_spawn(coordinator, transport.read_message(),
                           asio::bind_executor(
                               coordinator, [first_read](std::exception_ptr error, std::string result) {
                                   first_read->complete(error, std::move(result));
                               }));
            co_await wait_for(5ms);
            try {
                (void)co_await transport.read_message();
            } catch (const std::logic_error&) {
                rejected = true;
            }

            co_await first_read->wait();
            if (first_read->error) {
                std::rethrow_exception(first_read->error);
            }
            EXPECT_EQ(first_read->result, "response");
            transport.close();
        },
        asio::use_future);

    run_on_threads();
    EXPECT_NO_THROW(client.get());
    EXPECT_NO_THROW(server.get());
    EXPECT_TRUE(rejected);
}

TEST_F(WebSocketTransportTest, PendingClientReadOwnsStateAfterTransportDestruction) {
    tcp::acceptor acceptor(io_ctx_, tcp::endpoint(tcp::v4(), 0));
    auto port = acceptor.local_endpoint().port();
    auto coordinator = asio::make_strand(io_ctx_);

    auto server = asio::co_spawn(
        coordinator,
        [&]() -> mcp::Task<void> {
            auto socket = co_await acceptor.async_accept(asio::use_awaitable);
            mcp::WebSocketServerTransport transport(std::move(socket));
            EXPECT_EQ(co_await transport.read_message(), "ready");
            co_await wait_for(50ms);
            transport.close();
        },
        asio::use_future);

    bool read_failed = false;
    auto client = asio::co_spawn(
        coordinator,
        [&]() -> mcp::Task<void> {
            auto transport = std::make_unique<mcp::WebSocketClientTransport>(
                io_ctx_.get_executor(), "127.0.0.1", std::to_string(port));
            co_await transport->write_message("ready");

            auto pending_read = std::make_shared<ReadCompletion>(coordinator);
            asio::co_spawn(coordinator, transport->read_message(),
                           asio::bind_executor(coordinator, [pending_read](std::exception_ptr error,
                                                                           std::string result) {
                               pending_read->complete(error, std::move(result));
                           }));
            co_await wait_for(5ms);
            transport.reset();
            co_await pending_read->wait();
            read_failed = pending_read->error != nullptr;
        },
        asio::use_future);

    run_on_threads();
    EXPECT_NO_THROW(client.get());
    EXPECT_NO_THROW(server.get());
    EXPECT_TRUE(read_failed);
}

TEST_F(WebSocketTransportTest, PendingServerReadOwnsStateAfterTransportDestruction) {
    tcp::acceptor acceptor(io_ctx_, tcp::endpoint(tcp::v4(), 0));
    auto port = acceptor.local_endpoint().port();
    auto coordinator = asio::make_strand(io_ctx_);

    bool read_failed = false;
    auto server = asio::co_spawn(
        coordinator,
        [&]() -> mcp::Task<void> {
            auto socket = co_await acceptor.async_accept(asio::use_awaitable);
            auto transport = std::make_unique<mcp::WebSocketServerTransport>(std::move(socket));
            EXPECT_EQ(co_await transport->read_message(), "ready");

            auto pending_read = std::make_shared<ReadCompletion>(coordinator);
            asio::co_spawn(coordinator, transport->read_message(),
                           asio::bind_executor(coordinator, [pending_read](std::exception_ptr error,
                                                                           std::string result) {
                               pending_read->complete(error, std::move(result));
                           }));
            co_await wait_for(5ms);
            transport.reset();
            co_await pending_read->wait();
            read_failed = pending_read->error != nullptr;
        },
        asio::use_future);

    auto client = asio::co_spawn(
        coordinator,
        [&]() -> mcp::Task<void> {
            mcp::WebSocketClientTransport transport(io_ctx_.get_executor(), "127.0.0.1",
                                                    std::to_string(port));
            co_await transport.write_message("ready");
            co_await wait_for(50ms);
            transport.close();
        },
        asio::use_future);

    run_on_threads();
    EXPECT_NO_THROW(client.get());
    EXPECT_NO_THROW(server.get());
    EXPECT_TRUE(read_failed);
}

TEST_F(WebSocketTransportTest, CanceledQueuedWriteDoesNotBlockSubsequentWrites) {
    constexpr std::size_t payload_size = 16 * 1024 * 1024;
    tcp::acceptor acceptor(io_ctx_, tcp::endpoint(tcp::v4(), 0));
    auto port = acceptor.local_endpoint().port();
    auto coordinator = asio::make_strand(io_ctx_);

    auto server = asio::co_spawn(
        coordinator,
        [&]() -> mcp::Task<void> {
            auto socket = co_await acceptor.async_accept(asio::use_awaitable);
            mcp::WebSocketServerTransport transport(std::move(socket));
            co_await transport.write_message("ready");
            co_await wait_for(100ms);

            auto large_message = co_await transport.read_message();
            EXPECT_EQ(large_message.size(), payload_size);
            EXPECT_EQ(co_await transport.read_message(), "after-cancel");
            co_await transport.write_message("done");
            transport.close();
        },
        asio::use_future);

    bool canceled_write_failed = false;
    auto client = asio::co_spawn(
        coordinator,
        [&]() -> mcp::Task<void> {
            auto transport = std::make_shared<mcp::WebSocketClientTransport>(
                io_ctx_.get_executor(), "127.0.0.1", std::to_string(port));
            EXPECT_EQ(co_await transport->read_message(), "ready");

            auto watchdog = std::make_shared<asio::steady_timer>(coordinator, 30s);
            watchdog->async_wait(
                asio::bind_executor(coordinator, [transport](const boost::system::error_code& error) {
                    if (!error) {
                        transport->close();
                    }
                }));

            // Runs on every exit path, so a failure ends the test at once instead of when the
            // watchdog fires.
            struct StopOnExit {
                std::shared_ptr<asio::steady_timer> watchdog;
                std::shared_ptr<mcp::WebSocketClientTransport> transport;

                ~StopOnExit() {
                    boost::system::error_code ignored;
                    watchdog->cancel(ignored);
                    transport->close();
                }
            } stop_on_exit{watchdog, transport};

            auto first_write = std::make_shared<VoidCompletion>(coordinator);
            asio::co_spawn(coordinator, transport->write_message(std::string(payload_size, 'x')),
                           asio::bind_executor(coordinator, [first_write](std::exception_ptr error) {
                               first_write->complete(error);
                           }));
            co_await wait_for(10ms);

            asio::cancellation_signal cancel_queued_write;
            auto canceled_write = std::make_shared<VoidCompletion>(coordinator);
            asio::co_spawn(
                coordinator, transport->write_message("cancel-me"),
                asio::bind_cancellation_slot(
                    cancel_queued_write.slot(),
                    asio::bind_executor(coordinator, [canceled_write](std::exception_ptr error) {
                        canceled_write->complete(error);
                    })));
            co_await wait_for(10ms);
            cancel_queued_write.emit(asio::cancellation_type::all);
            co_await canceled_write->wait();
            canceled_write_failed = canceled_write->error != nullptr;

            co_await transport->write_message("after-cancel");
            co_await first_write->wait();
            if (first_write->error) {
                std::rethrow_exception(first_write->error);
            }
            EXPECT_EQ(co_await transport->read_message(), "done");
        },
        asio::use_future);

    run_on_threads();
    EXPECT_NO_THROW(client.get());
    EXPECT_NO_THROW(server.get());
    EXPECT_TRUE(canceled_write_failed);
}

// A peer that accepts the connection and then never answers the upgrade request must not hold a
// pending call forever: the connect timeout ends it.
TEST_F(WebSocketTransportTest, StalledHandshakeFailsAfterTheConnectTimeout) {
    StallingServer stalling(io_ctx_);
    stalling.accept_and_stall();

    mcp::WebSocketClientTransport transport(io_ctx_.get_executor(), "127.0.0.1",
                                            std::to_string(stalling.port()), "/",
                                            std::chrono::milliseconds(200));

    auto result = asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<bool> {
            try {
                co_await transport.write_message(R"({"jsonrpc":"2.0","id":1,"method":"ping"})");
            } catch (const boost::system::system_error& error) {
                co_return error.code() == boost::beast::error::timeout;
            }
            co_return false;
        },
        asio::use_future);

    std::thread runner([this]() { io_ctx_.run(); });
    const bool finished = result.wait_for(std::chrono::seconds(10)) == std::future_status::ready;
    io_ctx_.stop();
    runner.join();

    ASSERT_TRUE(finished) << "the stalled handshake was never cut off";
    EXPECT_TRUE(result.get()) << "the pending call must fail with a timeout";
    EXPECT_EQ(stalling.accepted(), 1);
}

// milliseconds::max() is the usual way to spell "no limit", and it does not fit the timer's
// nanosecond duration. It must mean no limit rather than overflow into a deadline already passed.
TEST_F(WebSocketTransportTest, ConnectTimeoutTooLongForTheClockMeansNoLimit) {
    StallingServer stalling(io_ctx_);
    stalling.accept_and_stall();

    mcp::WebSocketClientTransport transport(io_ctx_.get_executor(), "127.0.0.1",
                                            std::to_string(stalling.port()), "/",
                                            std::chrono::milliseconds::max());

    std::atomic<bool> timed_out{false};
    auto result = asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            try {
                co_await transport.write_message(R"({"jsonrpc":"2.0","id":1,"method":"ping"})");
            } catch (const boost::system::system_error& error) {
                timed_out = error.code() == boost::beast::error::timeout;
                throw;
            }
        },
        asio::use_future);

    std::thread runner([this]() { io_ctx_.run(); });
    // Still waiting on the stalled handshake after a while: no deadline fired.
    const bool still_pending =
        result.wait_for(std::chrono::milliseconds(300)) == std::future_status::timeout;
    transport.close();
    const bool finished = result.wait_for(std::chrono::seconds(10)) == std::future_status::ready;
    io_ctx_.stop();
    runner.join();

    EXPECT_TRUE(still_pending) << "the handshake was cut off although no limit was set";
    ASSERT_TRUE(finished) << "close() did not end the pending connect";
    EXPECT_ANY_THROW(result.get());
    EXPECT_FALSE(timed_out.load());
}

// The handshake is several socket operations, and the connect timeout can pass between two of them,
// here because the client's only io thread is busy when the first part of the response arrives.
// The pending call fails with a timeout like any other expired setup.
TEST_F(WebSocketTransportTest, ConnectTimeoutPassingBetweenTwoHandshakeReadsReportsATimeout) {
    asio::io_context server_io;
    tcp::acceptor acceptor(server_io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const auto port = acceptor.local_endpoint().port();

    std::thread server([&]() {
        tcp::socket socket(server_io);
        acceptor.accept(socket);
        asio::streambuf request;
        asio::read_until(socket, request, "\r\n\r\n");
        asio::post(io_ctx_, []() { std::this_thread::sleep_for(600ms); });
        std::this_thread::sleep_for(50ms);
        asio::write(socket, asio::buffer(std::string("HTTP/1.1 101 Switching Protocols\r\n")));
        std::this_thread::sleep_for(1s);
    });

    mcp::WebSocketClientTransport transport(io_ctx_.get_executor(), "127.0.0.1", std::to_string(port),
                                            "/", 200ms);
    auto result = asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            co_await transport.write_message(R"({"jsonrpc":"2.0","id":1,"method":"ping"})");
        },
        asio::use_future);
    std::thread runner([this]() { io_ctx_.run(); });
    const bool finished = result.wait_for(5s) == std::future_status::ready;
    transport.close();
    io_ctx_.stop();
    runner.join();
    server.join();

    ASSERT_TRUE(finished);
    try {
        result.get();
        ADD_FAILURE() << "the handshake succeeded";
    } catch (const boost::system::system_error& error) {
        EXPECT_EQ(error.code(), boost::beast::error::timeout) << error.code().message();
    }
}

#ifdef __linux__

// close() called from a thread that does not run the io_context, while the connect is suspended
// in its resolve. Asio's resolver thread checks its cancel token once, before it calls
// getaddrinfo(), so a close() that arrives while that call is in progress finds a resolve it can
// no longer cancel and a socket that is not open yet. The gate holds the lookup inside
// getaddrinfo() until close() has done its cancelling, so the connect resumes with usable addresses
// on a transport that is already closed. It must stop there. Without the closed check in
// ensure_connected() it connects to the stalling server and waits on a handshake that never ends.
TEST_F(WebSocketTransportTest, CloseWhileTheResolverIsPastItsCancelCheckOpensNoConnection) {
    StallingServer stalling(io_ctx_);
    stalling.accept_and_stall();

    auto transport = std::make_shared<mcp::WebSocketClientTransport>(
        io_ctx_.get_executor(), "127.0.0.1", std::to_string(stalling.port()));

    resolve_gate().arm(stalling.port());

    std::exception_ptr failure;
    std::promise<void> write_done;
    auto write_finished = write_done.get_future();
    asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            try {
                co_await transport->write_message(R"({"jsonrpc":"2.0","id":1,"method":"ping"})");
            } catch (...) {
                failure = std::current_exception();
            }
            write_done.set_value();
        },
        asio::detached);

    std::thread runner([this]() { io_ctx_.run(); });

    const auto limit = std::chrono::seconds(10);
    const bool lookup_held = resolve_gate().wait_until_entered(limit);

    // close() posts its cancelling to the transport's strand, which is one hop through the
    // io_context. Two further hops through the same queue cannot complete before it has.
    std::promise<void> cancel_done;
    auto cancel_finished = cancel_done.get_future();
    bool cancel_ran = false;
    if (lookup_held) {
        transport->close();
        asio::post(io_ctx_, [&]() { asio::post(io_ctx_, [&]() { cancel_done.set_value(); }); });
        cancel_ran = cancel_finished.wait_for(limit) == std::future_status::ready;
    }

    resolve_gate().release();
    const bool finished = write_finished.wait_for(limit) == std::future_status::ready;

    // Everything below reads state the runner thread wrote, so it stops first.
    io_ctx_.stop();
    runner.join();

    ASSERT_TRUE(lookup_held) << "the connect never reached getaddrinfo()";
    ASSERT_TRUE(cancel_ran) << "the cancelling posted by close() never ran on the io_context";
    EXPECT_TRUE(finished) << "close() was lost: the connect resumed from the lookup after the "
                             "transport closed and is still running";
    EXPECT_EQ(stalling.accepted(), 0) << "the transport connected after it had been closed";
    if (finished) {
        EXPECT_NE(failure, nullptr) << "a write cut short by close() must report an error";
    }
}

#endif  // __linux__

namespace {

struct OriginHandshakeResult {
    bool handshake_accepted{false};
    std::optional<std::string> server_received;
};

/// Opens a WebSocket to a fresh WebSocketServerTransport, sending `origin` with the upgrade when it
/// is set, and reports whether the handshake succeeded and what the server read.
OriginHandshakeResult handshake_with_origin(
    const std::optional<std::string>& origin,
    const std::function<void(mcp::WebSocketServerTransport&)>& configure,
    const std::function<void(mcp::WebSocketServerTransport&)>& after_first_read = {}) {
    asio::io_context io_ctx;
    tcp::acceptor acceptor(io_ctx, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const auto port = acceptor.local_endpoint().port();

    OriginHandshakeResult result;
    asio::co_spawn(
        io_ctx,
        [&]() -> mcp::Task<void> {
            auto socket = co_await acceptor.async_accept(asio::use_awaitable);
            mcp::WebSocketServerTransport transport(std::move(socket));
            configure(transport);
            result.server_received = co_await transport.read_message();
            if (after_first_read) {
                after_first_read(transport);
            }
            transport.close();
        },
        [](const std::exception_ptr&) {});

    asio::co_spawn(
        io_ctx,
        [&]() -> mcp::Task<void> {
            boost::beast::websocket::stream<boost::beast::tcp_stream> ws(io_ctx);
            co_await ws.next_layer().async_connect(
                tcp::endpoint(asio::ip::make_address("127.0.0.1"), port), asio::use_awaitable);
            ws.set_option(boost::beast::websocket::stream_base::decorator(
                [origin](boost::beast::websocket::request_type& request) {
                    if (origin) {
                        request.set(boost::beast::http::field::origin, *origin);
                    }
                }));
            co_await ws.async_handshake("127.0.0.1:" + std::to_string(port), "/", asio::use_awaitable);
            result.handshake_accepted = true;
            const std::string message = R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})";
            co_await ws.async_write(asio::buffer(message), asio::use_awaitable);
        },
        [](const std::exception_ptr&) {});

    io_ctx.run_for(5s);
    return result;
}

}  // namespace

// A browser lets any web page open a WebSocket to a local port and sends the page's Origin with the
// upgrade; WebSockets are not covered by CORS. Like the HTTP transports, the WebSocket server
// refuses an upgrade whose Origin it has not been told to allow.
TEST(WebSocketOriginTest, AnUpgradeFromAnOriginNotAllowedIsRefused) {
    const auto result =
        handshake_with_origin("https://elsewhere.example", [](mcp::WebSocketServerTransport&) {});
    EXPECT_FALSE(result.handshake_accepted);
    EXPECT_FALSE(result.server_received.has_value()) << *result.server_received;
}

TEST(WebSocketOriginTest, AnUpgradeFromAnAllowedOriginIsAccepted) {
    const auto result =
        handshake_with_origin("https://trusted.example", [](mcp::WebSocketServerTransport& server) {
            server.set_allowed_origins({"https://trusted.example"});
        });
    EXPECT_TRUE(result.handshake_accepted);
    EXPECT_TRUE(result.server_received.has_value());
}

TEST(WebSocketOriginTest, AllowingEveryOriginAcceptsAnyOrigin) {
    const auto result = handshake_with_origin(
        "https://elsewhere.example",
        [](mcp::WebSocketServerTransport& server) { server.set_allow_all_origins(true); });
    EXPECT_TRUE(result.handshake_accepted);
    EXPECT_TRUE(result.server_received.has_value());
}

// A non-browser client sends no Origin and is not affected by the allowlist.
TEST(WebSocketOriginTest, AnUpgradeWithoutAnOriginIsAccepted) {
    const auto result = handshake_with_origin(std::nullopt, [](mcp::WebSocketServerTransport&) {});
    EXPECT_TRUE(result.handshake_accepted);
    EXPECT_TRUE(result.server_received.has_value());
}

TEST(WebSocketOriginTest, OriginsCannotChangeOnceTheHandshakeHasStarted) {
    std::optional<std::string> refusal;
    const auto result = handshake_with_origin(
        std::nullopt, [](mcp::WebSocketServerTransport&) {},
        [&refusal](mcp::WebSocketServerTransport& server) {
            try {
                server.set_allowed_origins({"https://late.example"});
            } catch (const std::logic_error& error) {
                refusal = error.what();
            }
        });
    ASSERT_TRUE(result.server_received.has_value());
    EXPECT_TRUE(refusal.has_value());
}

#include "mcp/transport/websocket.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/bind_executor.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>
#include <chrono>
#include <exception>
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

            auto watchdog = std::make_shared<asio::steady_timer>(coordinator, 2s);
            watchdog->async_wait(
                asio::bind_executor(coordinator, [transport](const boost::system::error_code& error) {
                    if (!error) {
                        transport->close();
                    }
                }));

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

            boost::system::error_code ignored;
            watchdog->cancel(ignored);
            transport->close();
        },
        asio::use_future);

    run_on_threads();
    EXPECT_NO_THROW(client.get());
    EXPECT_NO_THROW(server.get());
    EXPECT_TRUE(canceled_write_failed);
}

#include "../test_utils.hpp"

#include "mcp/client/client.hpp"
#include "mcp/transport/memory.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <chrono>
#include <functional>
#include <memory>
#include <queue>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

nlohmann::json make_initialize_result() {
    return {{"protocolVersion", mcp::g_LATEST_PROTOCOL_VERSION},
            {"capabilities", nlohmann::json::object()},
            {"serverInfo", {{"name", "test-server"}, {"version", "1.0"}}}};
}

class ImmediateResponseTransport final : public mcp::ITransport {
   public:
    explicit ImmediateResponseTransport(const boost::asio::any_io_executor& executor)
        : executor_(executor), signal_(executor) {
        signal_.expires_at(std::chrono::steady_clock::time_point::max());
    }

    mcp::Task<std::string> read_message() override {
        for (;;) {
            if (!incoming_.empty()) {
                auto message = std::move(incoming_.front());
                incoming_.pop();
                co_return message;
            }
            if (closed_) {
                throw std::runtime_error("transport closed");
            }

            signal_.expires_at(std::chrono::steady_clock::time_point::max());
            try {
                co_await signal_.async_wait(boost::asio::use_awaitable);
            } catch (const boost::system::system_error& error) {
                if (error.code() != boost::asio::error::operation_aborted) {
                    throw;
                }
            }
        }
    }

    mcp::Task<void> write_message(std::string_view message) override {
        const auto request = nlohmann::json::parse(message);
        if (request.contains("id")) {
            const auto id = request.at("id").get<std::string>();
            if (request.value("method", "") == "initialize") {
                incoming_.push(make_result_response(id, make_initialize_result()).dump());
            } else {
                incoming_.push(make_result_response(id, {{"ok", true}}).dump());
            }
            signal_.cancel();

            // Let the read loop consume and dispatch the response before this
            // write completes. This reproduces completion-before-wait ordering.
            co_await boost::asio::post(executor_, boost::asio::use_awaitable);
            co_await boost::asio::post(executor_, boost::asio::use_awaitable);
        }
    }

    void close() override {
        closed_ = true;
        signal_.cancel();
    }

   private:
    boost::asio::any_io_executor executor_;
    boost::asio::steady_timer signal_;
    std::queue<std::string> incoming_;
    bool closed_{false};
};

class BlockingRequestWriteTransport final : public mcp::ITransport {
   public:
    explicit BlockingRequestWriteTransport(const boost::asio::any_io_executor& executor)
        : read_signal_(executor), write_signal_(executor) {
        read_signal_.expires_at(std::chrono::steady_clock::time_point::max());
        write_signal_.expires_at(std::chrono::steady_clock::time_point::max());
    }

    mcp::Task<std::string> read_message() override {
        for (;;) {
            if (!incoming_.empty()) {
                auto message = std::move(incoming_.front());
                incoming_.pop();
                co_return message;
            }
            if (closed_) {
                throw std::runtime_error("transport closed");
            }

            read_signal_.expires_at(std::chrono::steady_clock::time_point::max());
            try {
                co_await read_signal_.async_wait(boost::asio::use_awaitable);
            } catch (const boost::system::system_error& error) {
                if (error.code() != boost::asio::error::operation_aborted) {
                    throw;
                }
            }
        }
    }

    mcp::Task<void> write_message(std::string_view message) override {
        const auto request = nlohmann::json::parse(message);
        if (request.value("method", "") == "initialize") {
            incoming_.push(
                make_result_response(request.at("id").get<std::string>(), make_initialize_result())
                    .dump());
            read_signal_.cancel();
            co_return;
        }
        if (!request.contains("id")) {
            co_return;
        }

        blocked_write_started_.store(true, std::memory_order_release);
        write_signal_.expires_at(std::chrono::steady_clock::time_point::max());
        try {
            co_await write_signal_.async_wait(boost::asio::use_awaitable);
        } catch (const boost::system::system_error& error) {
            if (error.code() != boost::asio::error::operation_aborted) {
                throw;
            }
        }
        throw std::runtime_error("blocked write cancelled");
    }

    void close() override {
        closed_ = true;
        read_signal_.cancel();
        write_signal_.cancel();
    }

    [[nodiscard]] bool blocked_write_started() const {
        return blocked_write_started_.load(std::memory_order_acquire);
    }

   private:
    boost::asio::steady_timer read_signal_;
    boost::asio::steady_timer write_signal_;
    std::queue<std::string> incoming_;
    std::atomic_bool blocked_write_started_{false};
    bool closed_{false};
};

class QueuedWriteTransport final : public mcp::ITransport {
   public:
    explicit QueuedWriteTransport(const boost::asio::any_io_executor& executor)
        : read_signal_(executor), blocked_signal_(executor), release_signal_(executor) {
        read_signal_.expires_at(std::chrono::steady_clock::time_point::max());
        blocked_signal_.expires_at(std::chrono::steady_clock::time_point::max());
        release_signal_.expires_at(std::chrono::steady_clock::time_point::max());
    }

    mcp::Task<std::string> read_message() override {
        for (;;) {
            if (!incoming_.empty()) {
                auto message = std::move(incoming_.front());
                incoming_.pop();
                co_return message;
            }
            if (closed_) {
                throw std::runtime_error("transport closed");
            }

            read_signal_.expires_at(std::chrono::steady_clock::time_point::max());
            try {
                co_await read_signal_.async_wait(boost::asio::use_awaitable);
            } catch (const boost::system::system_error& error) {
                if (error.code() != boost::asio::error::operation_aborted) {
                    throw;
                }
            }
        }
    }

    mcp::Task<void> write_message(std::string_view message) override {
        const auto request = nlohmann::json::parse(message);
        const auto method = request.value("method", "");
        methods_.push_back(method);

        if (method == "initialize") {
            incoming_.push(
                make_result_response(request.at("id").get<std::string>(), make_initialize_result())
                    .dump());
            read_signal_.cancel();
            co_return;
        }
        if (method == "notifications/block") {
            blocked_.store(true, std::memory_order_release);
            blocked_signal_.cancel();
            release_signal_.expires_at(std::chrono::steady_clock::time_point::max());
            try {
                co_await release_signal_.async_wait(boost::asio::use_awaitable);
            } catch (const boost::system::system_error& error) {
                if (error.code() != boost::asio::error::operation_aborted) {
                    throw;
                }
            }
            if (closed_) {
                throw std::runtime_error("transport closed");
            }
            co_return;
        }
        if (request.contains("id")) {
            incoming_.push(
                make_result_response(request.at("id").get<std::string>(), nlohmann::json::object())
                    .dump());
            read_signal_.cancel();
        }
    }

    mcp::Task<void> wait_until_blocked() {
        while (!blocked_.load(std::memory_order_acquire)) {
            try {
                co_await blocked_signal_.async_wait(boost::asio::use_awaitable);
            } catch (const boost::system::system_error& error) {
                if (error.code() != boost::asio::error::operation_aborted) {
                    throw;
                }
            }
        }
    }

    void release_blocked_write() { release_signal_.cancel(); }

    void close() override {
        closed_ = true;
        read_signal_.cancel();
        blocked_signal_.cancel();
        release_signal_.cancel();
    }

    [[nodiscard]] const std::vector<std::string>& methods() const { return methods_; }

   private:
    boost::asio::steady_timer read_signal_;
    boost::asio::steady_timer blocked_signal_;
    boost::asio::steady_timer release_signal_;
    std::queue<std::string> incoming_;
    std::vector<std::string> methods_;
    std::atomic_bool blocked_{false};
    bool closed_{false};
};

void run_on_thread_pool(boost::asio::io_context& io_context, std::size_t thread_count = 4) {
    std::vector<std::thread> threads;
    threads.reserve(thread_count);
    for (std::size_t index = 0; index < thread_count; ++index) {
        threads.emplace_back([&io_context]() { io_context.run(); });
    }
    for (auto& thread : threads) {
        thread.join();
    }
}

}  // namespace

class ClientCoreTest : public ::testing::Test {
   protected:
    boost::asio::io_context io_ctx_;
};

TEST_F(ClientCoreTest, SendRequestAddsToMapAndReturnsResult) {
    auto transport = std::make_shared<ScriptedTransport>(io_ctx_.get_executor());
    auto* raw_transport = transport.get();

    raw_transport->set_on_write([raw_transport](std::string_view msg) {
        auto json_msg = nlohmann::json::parse(msg);
        if (json_msg.contains("id")) {
            auto id = json_msg["id"].get<std::string>();
            auto method = json_msg.value("method", "");
            if (method == "initialize") {
                raw_transport->enqueue_message(
                    make_result_response(id, make_initialize_result()).dump());
            } else {
                raw_transport->enqueue_message(make_result_response(id, {{"answer", 42}}).dump());
            }
        }
    });

    mcp::Client client(transport, io_ctx_.get_executor());

    nlohmann::json result;
    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            mcp::Implementation info;
            info.name = "test-client";
            info.version = "0.1";
            co_await client.connect(std::move(info), mcp::ClientCapabilities{});

            nlohmann::json req_params = {{"key", "value"}};
            result = co_await client.send_request("test/method", std::move(req_params));
            raw_transport->close();
        },
        boost::asio::detached);

    io_ctx_.run();

    EXPECT_EQ(result["answer"], 42);
    EXPECT_EQ(client.pending_request_count(), 0);
}

TEST_F(ClientCoreTest, RequestsFailImmediatelyBeforeConnect) {
    auto transport = std::make_shared<ScriptedTransport>(io_ctx_.get_executor());
    mcp::Client client(transport, io_ctx_.get_executor());

    EXPECT_THROW(static_cast<void>(client.send_request("ping", std::nullopt)), mcp::McpError);
    EXPECT_THROW(static_cast<void>(client.send_notification("notifications/test", std::nullopt)),
                 mcp::McpError);
    EXPECT_THROW(static_cast<void>(client.ping()), mcp::McpError);
}

TEST_F(ClientCoreTest, ConnectHandshakeFollowsMcpOrder) {
    auto transport = std::make_shared<ScriptedTransport>(io_ctx_.get_executor());
    auto* raw_transport = transport.get();

    raw_transport->set_on_write([raw_transport](std::string_view msg) {
        auto json_msg = nlohmann::json::parse(msg);
        if (json_msg.contains("id")) {
            auto id = json_msg["id"].get<std::string>();
            raw_transport->enqueue_message(make_result_response(id, make_initialize_result()).dump());
        }
    });

    mcp::Client client(transport, io_ctx_.get_executor());

    mcp::InitializeResult init_result;
    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            mcp::Implementation info;
            info.name = "test-client";
            info.version = "0.1";
            init_result = co_await client.connect(std::move(info), mcp::ClientCapabilities{});
            raw_transport->close();
        },
        boost::asio::detached);

    io_ctx_.run();

    ASSERT_GE(raw_transport->written().size(), 2);

    auto first = nlohmann::json::parse(raw_transport->written()[0]);
    EXPECT_EQ(first["method"], "initialize");
    EXPECT_TRUE(first.contains("id"));
    EXPECT_EQ(first["jsonrpc"], "2.0");
    EXPECT_EQ(first["params"]["protocolVersion"], std::string(mcp::g_LATEST_PROTOCOL_VERSION));
    EXPECT_EQ(first["params"]["clientInfo"]["name"], "test-client");
    EXPECT_EQ(first["params"]["clientInfo"]["version"], "0.1");

    auto second = nlohmann::json::parse(raw_transport->written()[1]);
    EXPECT_EQ(second["method"], "notifications/initialized");
    EXPECT_FALSE(second.contains("id"));
    EXPECT_EQ(second["jsonrpc"], "2.0");

    EXPECT_EQ(init_result.protocolVersion, std::string(mcp::g_LATEST_PROTOCOL_VERSION));
    EXPECT_EQ(init_result.serverInfo.name, "test-server");
    EXPECT_EQ(init_result.serverInfo.version, "1.0");
}

TEST_F(ClientCoreTest, NullIdErrorDoesNotStopReadLoop) {
    auto transport = std::make_shared<ScriptedTransport>(io_ctx_.get_executor());
    auto* raw_transport = transport.get();

    raw_transport->set_on_write([raw_transport](std::string_view msg) {
        const auto json_msg = nlohmann::json::parse(msg);
        if (!json_msg.contains("id")) {
            return;
        }

        raw_transport->enqueue_message(
            nlohmann::json{{"jsonrpc", "2.0"},
                           {"id", nullptr},
                           {"error", {{"code", mcp::g_PARSE_ERROR}, {"message", "parse error"}}}}
                .dump());
        raw_transport->enqueue_message(
            make_result_response(json_msg.at("id").get<std::string>(), make_initialize_result())
                .dump());
    });

    mcp::Client client(transport, io_ctx_.get_executor());
    mcp::InitializeResult result;
    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            result = co_await client.connect("test-client", "0.1");
            raw_transport->close();
        },
        boost::asio::detached);

    io_ctx_.run();

    EXPECT_EQ(result.serverInfo.name, "test-server");
}

TEST_F(ClientCoreTest, SendRequestErrorResponseThrows) {
    auto transport = std::make_shared<ScriptedTransport>(io_ctx_.get_executor());
    auto* raw_transport = transport.get();

    raw_transport->set_on_write([raw_transport](std::string_view msg) {
        auto json_msg = nlohmann::json::parse(msg);
        if (json_msg.contains("id") && json_msg["method"] == "initialize") {
            auto id = json_msg["id"].get<std::string>();
            raw_transport->enqueue_message(make_result_response(id, make_initialize_result()).dump());
        } else if (json_msg.contains("id")) {
            auto id = json_msg["id"].get<std::string>();
            raw_transport->enqueue_message(
                make_error_response(id, mcp::g_METHOD_NOT_FOUND, "method not found").dump());
        }
    });

    mcp::Client client(transport, io_ctx_.get_executor());

    std::exception_ptr captured_ex;
    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            mcp::Implementation info;
            info.name = "test-client";
            info.version = "0.1";
            co_await client.connect(std::move(info), mcp::ClientCapabilities{});

            try {
                co_await client.send_request("nonexistent/method", std::nullopt);
            } catch (...) {
                captured_ex = std::current_exception();
            }
            raw_transport->close();
        },
        boost::asio::detached);

    io_ctx_.run();

    ASSERT_NE(captured_ex, nullptr);
    try {
        std::rethrow_exception(captured_ex);
    } catch (const std::runtime_error& err) {
        std::string msg = err.what();
        EXPECT_TRUE(msg.find(std::to_string(mcp::g_METHOD_NOT_FOUND)) != std::string::npos);
        EXPECT_TRUE(msg.find("method not found") != std::string::npos);
    }
}

TEST_F(ClientCoreTest, SendNotificationDoesNotExpectResponse) {
    auto transport = std::make_shared<ScriptedTransport>(io_ctx_.get_executor());
    auto* raw_transport = transport.get();

    raw_transport->set_on_write([raw_transport](std::string_view msg) {
        auto json_msg = nlohmann::json::parse(msg);
        if (json_msg.contains("id")) {
            auto id = json_msg["id"].get<std::string>();
            raw_transport->enqueue_message(make_result_response(id, make_initialize_result()).dump());
        }
    });

    mcp::Client client(transport, io_ctx_.get_executor());

    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            mcp::Implementation info;
            info.name = "test-client";
            info.version = "0.1";
            co_await client.connect(std::move(info), mcp::ClientCapabilities{});

            nlohmann::json progress_params = {{"token", "abc"}};
            co_await client.send_notification("notifications/progress", std::move(progress_params));
            raw_transport->close();
        },
        boost::asio::detached);

    io_ctx_.run();

    ASSERT_GE(raw_transport->written().size(), 3);
    auto notif = nlohmann::json::parse(raw_transport->written()[2]);
    EXPECT_EQ(notif["method"], "notifications/progress");
    EXPECT_FALSE(notif.contains("id"));
    EXPECT_EQ(notif["params"]["token"], "abc");

    EXPECT_EQ(client.pending_request_count(), 0);
}

TEST_F(ClientCoreTest, MultipleRequestsDispatchCorrectly) {
    auto transport = std::make_shared<ScriptedTransport>(io_ctx_.get_executor());
    auto* raw_transport = transport.get();

    raw_transport->set_on_write([raw_transport](std::string_view msg) {
        auto json_msg = nlohmann::json::parse(msg);
        if (json_msg.contains("id")) {
            auto id = json_msg["id"].get<std::string>();
            auto method = json_msg.value("method", "");
            if (method == "initialize") {
                raw_transport->enqueue_message(
                    make_result_response(id, make_initialize_result()).dump());
            } else {
                raw_transport->enqueue_message(
                    make_result_response(id, {{"method_echo", method}}).dump());
            }
        }
    });

    mcp::Client client(transport, io_ctx_.get_executor());

    nlohmann::json result_a;
    nlohmann::json result_b;

    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            mcp::Implementation info;
            info.name = "test-client";
            info.version = "0.1";
            co_await client.connect(std::move(info), mcp::ClientCapabilities{});

            result_a = co_await client.send_request("tools/list", std::nullopt);
            result_b = co_await client.send_request("resources/list", std::nullopt);
            raw_transport->close();
        },
        boost::asio::detached);

    io_ctx_.run();

    EXPECT_EQ(result_a["method_echo"], "tools/list");
    EXPECT_EQ(result_b["method_echo"], "resources/list");
    EXPECT_EQ(client.pending_request_count(), 0);
}

TEST_F(ClientCoreTest, ReadLoopIgnoresServerNotifications) {
    auto transport = std::make_shared<ScriptedTransport>(io_ctx_.get_executor());
    auto* raw_transport = transport.get();

    raw_transport->set_on_write([raw_transport](std::string_view msg) {
        auto json_msg = nlohmann::json::parse(msg);
        if (json_msg.contains("id")) {
            auto id = json_msg["id"].get<std::string>();
            auto method = json_msg.value("method", "");
            if (method == "initialize") {
                raw_transport->enqueue_message(
                    make_result_response(id, make_initialize_result()).dump());
            } else {
                nlohmann::json server_notif = {{"jsonrpc", "2.0"},
                                               {"method", "notifications/tools/list_changed"}};
                raw_transport->enqueue_message(server_notif.dump());
                raw_transport->enqueue_message(make_result_response(id, {{"status", "ok"}}).dump());
            }
        }
    });

    mcp::Client client(transport, io_ctx_.get_executor());

    nlohmann::json result;
    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            mcp::Implementation info;
            info.name = "test-client";
            info.version = "0.1";
            co_await client.connect(std::move(info), mcp::ClientCapabilities{});

            nlohmann::json call_params = {{"name", "test"}};
            result = co_await client.send_request("tools/call", std::move(call_params));
            raw_transport->close();
        },
        boost::asio::detached);

    io_ctx_.run();

    EXPECT_EQ(result["status"], "ok");
}

TEST_F(ClientCoreTest, PendingRequestCountReflectsInFlightRequests) {
    auto transport = std::make_shared<ScriptedTransport>(io_ctx_.get_executor());
    auto* raw_transport = transport.get();

    std::size_t count_during_flight = 0;

    mcp::Client client(transport, io_ctx_.get_executor());

    raw_transport->set_on_write([raw_transport, &count_during_flight, &client](std::string_view msg) {
        auto json_msg = nlohmann::json::parse(msg);
        if (json_msg.contains("id")) {
            auto id = json_msg["id"].get<std::string>();
            auto method = json_msg.value("method", "");
            if (method == "initialize") {
                raw_transport->enqueue_message(
                    make_result_response(id, make_initialize_result()).dump());
            } else {
                count_during_flight = client.pending_request_count();
                raw_transport->enqueue_message(
                    make_result_response(id, {{"tools", nlohmann::json::array()}}).dump());
            }
        }
    });

    EXPECT_EQ(client.pending_request_count(), 0);

    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            mcp::Implementation info;
            info.name = "test-client";
            info.version = "0.1";
            co_await client.connect(std::move(info), mcp::ClientCapabilities{});

            co_await client.send_request("tools/list", std::nullopt);

            EXPECT_EQ(client.pending_request_count(), 0);
            raw_transport->close();
        },
        boost::asio::detached);

    io_ctx_.run();

    EXPECT_EQ(count_during_flight, 1);
    EXPECT_EQ(client.pending_request_count(), 0);
}

TEST_F(ClientCoreTest, ConnectReturnsServerCapabilities) {
    auto transport = std::make_shared<ScriptedTransport>(io_ctx_.get_executor());
    auto* raw_transport = transport.get();

    auto init_result_json = make_initialize_result();
    init_result_json["capabilities"]["tools"] = {{"listChanged", true}};
    init_result_json["instructions"] = "Welcome to the test server";

    raw_transport->set_on_write(
        [raw_transport, init_result_json = std::move(init_result_json)](std::string_view msg) {
            auto json_msg = nlohmann::json::parse(msg);
            if (json_msg.contains("id")) {
                auto id = json_msg["id"].get<std::string>();
                raw_transport->enqueue_message(make_result_response(id, init_result_json).dump());
            }
        });

    mcp::Client client(transport, io_ctx_.get_executor());

    mcp::InitializeResult init_result;
    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            mcp::Implementation info;
            info.name = "test-client";
            info.version = "0.1";
            init_result = co_await client.connect(std::move(info), mcp::ClientCapabilities{});
            raw_transport->close();
        },
        boost::asio::detached);

    io_ctx_.run();

    EXPECT_EQ(init_result.protocolVersion, std::string(mcp::g_LATEST_PROTOCOL_VERSION));
    EXPECT_EQ(init_result.serverInfo.name, "test-server");
    ASSERT_TRUE(init_result.instructions.has_value());
    EXPECT_EQ(*init_result.instructions, "Welcome to the test server");
    ASSERT_TRUE(init_result.capabilities.tools.has_value());
    ASSERT_TRUE(init_result.capabilities.tools->listChanged.has_value());
    EXPECT_TRUE(*init_result.capabilities.tools->listChanged);
}

TEST_F(ClientCoreTest, PingSendsRequestAndReceivesResponse) {
    auto transport = std::make_shared<ScriptedTransport>(io_ctx_.get_executor());
    auto* raw_transport = transport.get();

    raw_transport->set_on_write([raw_transport](std::string_view msg) {
        auto json_msg = nlohmann::json::parse(msg);
        if (json_msg.contains("id")) {
            auto id = json_msg["id"].get<std::string>();
            auto method = json_msg.value("method", "");
            if (method == "initialize") {
                raw_transport->enqueue_message(
                    make_result_response(id, make_initialize_result()).dump());
            } else if (method == "ping") {
                raw_transport->enqueue_message(
                    make_result_response(id, nlohmann::json::object()).dump());
            }
        }
    });

    mcp::Client client(transport, io_ctx_.get_executor());

    bool ping_completed = false;
    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            mcp::Implementation info;
            info.name = "test-client";
            info.version = "0.1";
            co_await client.connect(std::move(info), mcp::ClientCapabilities{});

            co_await client.ping();
            ping_completed = true;
            raw_transport->close();
        },
        boost::asio::detached);

    io_ctx_.run();

    EXPECT_TRUE(ping_completed);
    EXPECT_EQ(client.pending_request_count(), 0);
}

TEST_F(ClientCoreTest, RequestTimeoutThrowsStructuredMcpError) {
    auto transport = std::make_shared<ScriptedTransport>(io_ctx_.get_executor());
    auto* raw_transport = transport.get();

    raw_transport->set_on_write([raw_transport](std::string_view msg) {
        auto json_msg = nlohmann::json::parse(msg);
        if (json_msg.contains("id") && json_msg.value("method", "") == "initialize") {
            auto id = json_msg["id"].get<std::string>();
            raw_transport->enqueue_message(make_result_response(id, make_initialize_result()).dump());
        }
    });

    mcp::Client client(transport, io_ctx_.get_executor());
    std::optional<int> error_code;

    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            co_await client.connect("test-client", "0.1");
            try {
                mcp::RequestOptions options;
                options.timeout = std::chrono::milliseconds(5);
                co_await client.send_request("tools/list", std::nullopt, options);
            } catch (const mcp::McpError& error) {
                error_code = error.code();
            }
            client.close();
        },
        boost::asio::detached);

    io_ctx_.run();

    ASSERT_TRUE(error_code.has_value());
    EXPECT_EQ(*error_code, mcp::g_REQUEST_TIMEOUT);
    EXPECT_EQ(client.pending_request_count(), 0);
}

TEST_F(ClientCoreTest, RequestTimeoutIncludesTimeQueuedInTransportWrite) {
    auto transport = std::make_shared<BlockingRequestWriteTransport>(io_ctx_.get_executor());
    mcp::Client client(transport, io_ctx_.get_executor());

    std::optional<int> error_code;
    bool request_completed = false;
    bool watchdog_fired = false;
    boost::asio::steady_timer watchdog(io_ctx_);
    watchdog.expires_after(std::chrono::milliseconds(250));
    watchdog.async_wait([&](const boost::system::error_code& error) {
        if (!error) {
            watchdog_fired = true;
            client.close();
        }
    });

    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            co_await client.connect("test-client", "0.1");
            try {
                mcp::RequestOptions options;
                options.timeout = std::chrono::milliseconds(10);
                co_await client.send_request("tools/list", std::nullopt, options);
            } catch (const mcp::McpError& error) {
                error_code = error.code();
            }
            request_completed = true;
            watchdog.cancel();
            client.close();
        },
        boost::asio::detached);

    io_ctx_.run();

    EXPECT_TRUE(transport->blocked_write_started());
    EXPECT_TRUE(request_completed);
    EXPECT_FALSE(watchdog_fired);
    ASSERT_TRUE(error_code.has_value());
    EXPECT_EQ(*error_code, mcp::g_REQUEST_TIMEOUT);
    EXPECT_EQ(client.pending_request_count(), 0);
}

TEST_F(ClientCoreTest, TimedOutQueuedRequestIsNotTransmittedLater) {
    auto transport = std::make_shared<QueuedWriteTransport>(io_ctx_.get_executor());
    mcp::Client client(transport, io_ctx_.get_executor());

    std::optional<int> error_code;
    std::exception_ptr notification_error;
    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            co_await client.connect("test-client", "0.1");
            boost::asio::co_spawn(io_ctx_,
                                  client.send_notification("notifications/block", std::nullopt),
                                  [&notification_error](std::exception_ptr error) {
                                      notification_error = std::move(error);
                                  });
            co_await transport->wait_until_blocked();

            try {
                mcp::RequestOptions options;
                options.timeout = std::chrono::milliseconds(10);
                co_await client.send_request("tools/list", std::nullopt, options);
            } catch (const mcp::McpError& error) {
                error_code = error.code();
            }

            transport->release_blocked_write();
            boost::asio::steady_timer settle(io_ctx_, std::chrono::milliseconds(10));
            co_await settle.async_wait(boost::asio::use_awaitable);
            client.close();
        },
        boost::asio::detached);

    io_ctx_.run();

    ASSERT_TRUE(error_code.has_value());
    EXPECT_EQ(*error_code, mcp::g_REQUEST_TIMEOUT);
    EXPECT_EQ(notification_error, nullptr);
    EXPECT_EQ(std::count(transport->methods().begin(), transport->methods().end(), "tools/list"), 0);
    EXPECT_EQ(client.pending_request_count(), 0);
}

TEST_F(ClientCoreTest, RemoteErrorPreservesJsonRpcData) {
    auto transport = std::make_shared<ScriptedTransport>(io_ctx_.get_executor());
    auto* raw_transport = transport.get();

    raw_transport->set_on_write([raw_transport](std::string_view msg) {
        auto json_msg = nlohmann::json::parse(msg);
        if (!json_msg.contains("id")) {
            return;
        }
        auto id = json_msg["id"].get<std::string>();
        if (json_msg.value("method", "") == "initialize") {
            raw_transport->enqueue_message(make_result_response(id, make_initialize_result()).dump());
            return;
        }
        auto response = make_error_response(id, mcp::g_INVALID_PARAMS, "invalid arguments");
        response["error"]["data"] = {{"field", "limit"}};
        raw_transport->enqueue_message(response.dump());
    });

    mcp::Client client(transport, io_ctx_.get_executor());
    std::optional<mcp::Error> captured_error;

    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            co_await client.connect("test-client", "0.1");
            try {
                co_await client.send_request("tools/call", nlohmann::json::object());
            } catch (const mcp::McpError& error) {
                captured_error = error.error();
            }
            client.close();
        },
        boost::asio::detached);

    io_ctx_.run();

    ASSERT_TRUE(captured_error.has_value());
    EXPECT_EQ(captured_error->code, mcp::g_INVALID_PARAMS);
    ASSERT_TRUE(captured_error->data.has_value());
    EXPECT_EQ(captured_error->data->at("field"), "limit");
}

TEST_F(ClientCoreTest, ImmediateResponseCannotBeLostBeforeWaitStarts) {
    auto transport = std::make_shared<ImmediateResponseTransport>(io_ctx_.get_executor());
    mcp::ClientOptions client_options;
    client_options.request_timeout = std::chrono::seconds(2);
    mcp::Client client(transport, io_ctx_.get_executor(), client_options);

    boost::asio::steady_timer watchdog(io_ctx_);
    watchdog.expires_after(std::chrono::milliseconds(250));
    bool watchdog_fired = false;
    bool request_completed = false;
    nlohmann::json result;

    watchdog.async_wait([&](const boost::system::error_code& error) {
        if (!error) {
            watchdog_fired = true;
            client.close();
        }
    });

    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            co_await client.connect("immediate-response-client", "1.0");
            result = co_await client.send_request("test/immediate", std::nullopt);
            request_completed = true;
            watchdog.cancel();
            client.close();
        },
        boost::asio::detached);

    io_ctx_.run();

    EXPECT_FALSE(watchdog_fired);
    EXPECT_TRUE(request_completed);
    EXPECT_EQ(result, nlohmann::json({{"ok", true}}));
}

TEST_F(ClientCoreTest, DestructionSafelyStopsActiveReadLoop) {
    auto transport = std::make_shared<ScriptedTransport>(io_ctx_.get_executor());
    auto* raw_transport = transport.get();
    raw_transport->set_on_write([raw_transport](std::string_view message) {
        const auto request = nlohmann::json::parse(message);
        if (request.contains("id") && request.value("method", "") == "initialize") {
            const auto id = request.at("id").get<std::string>();
            raw_transport->enqueue_message(make_result_response(id, make_initialize_result()).dump());
        }
    });

    auto client = std::make_unique<mcp::Client>(transport, io_ctx_.get_executor());
    bool initialized = false;
    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            co_await client->connect("destruction-client", "1.0");
            initialized = true;
            client.reset();
        },
        boost::asio::detached);

    io_ctx_.run();

    EXPECT_TRUE(initialized);
    EXPECT_EQ(client, nullptr);
    EXPECT_TRUE(raw_transport->is_closed());
}

TEST_F(ClientCoreTest, ConcurrentRequestsRemainCorrelatedOnMultiThreadedExecutor) {
    using namespace std::chrono_literals;

    constexpr std::size_t request_count = 256;
    auto [client_transport, server_transport] =
        mcp::create_memory_transport_pair(io_ctx_.get_executor());
    mcp::ClientOptions options;
    options.request_timeout = 2s;
    mcp::Client client(client_transport, io_ctx_.get_executor(), options);

    std::atomic_size_t completed{0};
    std::atomic_size_t correct{0};
    std::atomic_size_t errors{0};
    std::atomic_bool stop_poll{false};

    boost::asio::co_spawn(
        io_ctx_,
        [server_transport]() -> mcp::Task<void> {
            std::size_t responses = 0;
            while (responses <= request_count) {
                const auto request = nlohmann::json::parse(co_await server_transport->read_message());
                if (!request.contains("id")) {
                    continue;
                }

                nlohmann::json result;
                if (request.value("method", "") == "initialize") {
                    result = make_initialize_result();
                } else {
                    result = {{"sequence", request.at("params").at("sequence")}};
                }
                co_await server_transport->write_message(
                    make_result_response(request.at("id").get<std::string>(), std::move(result))
                        .dump());
                ++responses;
            }
        },
        [&errors](std::exception_ptr error) {
            if (error) {
                errors.fetch_add(1, std::memory_order_relaxed);
            }
        });

    boost::asio::co_spawn(
        io_ctx_,
        [&client, &completed, &correct, &errors]() -> mcp::Task<void> {
            try {
                co_await client.connect("threaded-client", "1.0");
                auto executor = co_await boost::asio::this_coro::executor;
                for (std::size_t sequence = 0; sequence < request_count; ++sequence) {
                    boost::asio::co_spawn(
                        executor,
                        client.send_request("test/repeat", nlohmann::json{{"sequence", sequence}}),
                        [sequence, &completed, &correct, &errors](std::exception_ptr error,
                                                                  nlohmann::json result) {
                            if (error) {
                                errors.fetch_add(1, std::memory_order_relaxed);
                            } else if (result.at("sequence").get<std::size_t>() == sequence) {
                                correct.fetch_add(1, std::memory_order_relaxed);
                            }
                            completed.fetch_add(1, std::memory_order_release);
                        });
                }
            } catch (...) {
                errors.fetch_add(1, std::memory_order_relaxed);
                client.close();
            }
        },
        boost::asio::detached);

    boost::asio::steady_timer watchdog(io_ctx_);
    watchdog.expires_after(5s);
    watchdog.async_wait([&client, &completed, &stop_poll](const boost::system::error_code& error) {
        if (!error && completed.load(std::memory_order_acquire) != request_count) {
            stop_poll.store(true, std::memory_order_release);
            client.close();
        }
    });

    boost::asio::steady_timer completion_poll(io_ctx_);
    std::function<void()> poll;
    poll = [&]() {
        if (stop_poll.load(std::memory_order_acquire)) {
            return;
        }
        if (completed.load(std::memory_order_acquire) == request_count) {
            stop_poll.store(true, std::memory_order_release);
            watchdog.cancel();
            client.close();
            return;
        }
        completion_poll.expires_after(1ms);
        completion_poll.async_wait([&poll](const boost::system::error_code& error) {
            if (!error) {
                poll();
            }
        });
    };
    poll();

    run_on_thread_pool(io_ctx_);

    EXPECT_EQ(completed.load(), request_count);
    EXPECT_EQ(correct.load(), request_count);
    EXPECT_EQ(errors.load(), 0);
    EXPECT_EQ(client.pending_request_count(), 0);
}

namespace {

// Drives a connect() whose peer answers `initialize` with the given protocolVersion, and returns
// the McpError message the client raised. The version string is entirely the server's choice, so
// this is the untrusted-peer text that reaches the diagnostic.
struct RejectedVersionOutcome {
    bool threw{false};
    int code{0};
    std::string message;
};

RejectedVersionOutcome connect_against_protocol_version(boost::asio::io_context& io_ctx,
                                                        const std::string& protocol_version) {
    auto transport = std::make_shared<ScriptedTransport>(io_ctx.get_executor());
    auto* raw_transport = transport.get();

    raw_transport->set_on_write([raw_transport, protocol_version](std::string_view msg) {
        const auto json_msg = nlohmann::json::parse(msg);
        if (!json_msg.contains("id")) {
            return;
        }
        raw_transport->enqueue_message(
            make_result_response(
                json_msg.at("id").get<std::string>(),
                nlohmann::json{{"protocolVersion", protocol_version},
                               {"capabilities", nlohmann::json::object()},
                               {"serverInfo", {{"name", "test-server"}, {"version", "1.0"}}}})
                .dump());
    });

    mcp::Client client(transport, io_ctx.get_executor());

    RejectedVersionOutcome outcome;
    boost::asio::co_spawn(
        io_ctx,
        [&]() -> mcp::Task<void> {
            mcp::Implementation info;
            info.name = "test-client";
            info.version = "0.1";
            try {
                static_cast<void>(co_await client.connect(std::move(info), mcp::ClientCapabilities{}));
            } catch (const mcp::McpError& error) {
                outcome.threw = true;
                outcome.code = error.code();
                outcome.message = error.message();
            }
            raw_transport->close();
        },
        boost::asio::detached);

    io_ctx.run();
    return outcome;
}

}  // namespace

// The protocol version in an `initialize` result is chosen by the remote server. When the client
// refuses it, the rejected value is interpolated into the McpError the application logs, so a
// malicious or broken server that answers with CR/LF forges a line in the application's log and a
// bidi override reorders the rest of the message.
TEST_F(ClientCoreTest, UnsupportedProtocolVersionDiagnosticFlattensServerChosenVersion) {
    // "zqtripwire" is a token no other code path produces; see the tripwire assertions below.
    // \xe2\x80\xae is U+202E RIGHT-TO-LEFT OVERRIDE, written escaped so this source file does not
    // itself contain a bidi override.
    const std::string forged_version =
        "zqtripwire\r\n2026-09-20 ERROR forged line from the server\xe2\x80\xae reordered tail";

    const auto outcome = connect_against_protocol_version(io_ctx_, forged_version);

    // Tripwire. The rejection must be the strict-protocol-validation site, not a JSON parse
    // failure, a transport error, or any other throw that never interpolates the peer's bytes --
    // each of those would make the assertions below pass for a reason unrelated to the site under
    // test. Dump the message on failure so a vacuous pass cannot hide.
    ASSERT_TRUE(outcome.threw) << "connect() did not reject the version at all";
    ASSERT_EQ(outcome.code, mcp::g_INVALID_REQUEST) << "actual message: " << outcome.message;
    ASSERT_EQ(outcome.message.rfind("Server selected unsupported protocol version: ", 0), 0U)
        << "actual message: " << outcome.message;
    ASSERT_NE(outcome.message.find("zqtripwire"), std::string::npos)
        << "actual message: " << outcome.message;

    EXPECT_EQ(outcome.message.find('\r'), std::string::npos) << "actual message: " << outcome.message;
    EXPECT_EQ(outcome.message.find('\n'), std::string::npos) << "actual message: " << outcome.message;
    EXPECT_EQ(outcome.message.find("\xe2\x80\xae"), std::string::npos)
        << "actual message: " << outcome.message;
}

// The same site must also bound the value, so a server cannot flood the application's log through
// a megabyte-long protocol version.
TEST_F(ClientCoreTest, UnsupportedProtocolVersionDiagnosticBoundsServerChosenVersion) {
    const std::string forged_version = "zqtripwire" + std::string(64 * 1024, 'A');

    const auto outcome = connect_against_protocol_version(io_ctx_, forged_version);

    ASSERT_TRUE(outcome.threw) << "connect() did not reject the version at all";
    ASSERT_EQ(outcome.code, mcp::g_INVALID_REQUEST)
        << "actual message prefix: " << outcome.message.substr(0, 80);
    ASSERT_EQ(outcome.message.rfind("Server selected unsupported protocol version: ", 0), 0U)
        << "actual message prefix: " << outcome.message.substr(0, 80);
    ASSERT_NE(outcome.message.find("zqtripwire"), std::string::npos)
        << "actual message prefix: " << outcome.message.substr(0, 80);

    EXPECT_LT(outcome.message.size(), forged_version.size())
        << "message size: " << outcome.message.size();
    EXPECT_LE(outcome.message.size(), std::size_t{512}) << "message size: " << outcome.message.size();
}

namespace {

// Drives a request the peer answers with a JSON-RPC error object, and reports what the client
// raised.
//
// This is the shortest path there is from a peer's bytes to an application's log. `error.message`
// is deserialized verbatim off the wire (`json_message.at("error").get<Error>()`), carried into
// McpError, and interpolated into what(). No OAuth, no discovery, no metadata document: an
// ordinary error response to an ordinary request.
struct PeerErrorOutcome {
    bool threw{false};
    int code{0};
    std::string what;         ///< McpError::what() -- the human-readable diagnostic.
    std::string raw_message;  ///< McpError::message() -- the structured field, which stays raw.
};

PeerErrorOutcome send_request_against_peer_error(boost::asio::io_context& io_ctx,
                                                 const std::string& peer_message) {
    auto transport = std::make_shared<ScriptedTransport>(io_ctx.get_executor());
    auto* raw_transport = transport.get();

    raw_transport->set_on_write([raw_transport, peer_message](std::string_view msg) {
        const auto json_msg = nlohmann::json::parse(msg);
        if (!json_msg.contains("id")) {
            return;
        }
        const auto id = json_msg.at("id").get<std::string>();
        if (json_msg.value("method", "") == "initialize") {
            raw_transport->enqueue_message(make_result_response(id, make_initialize_result()).dump());
            return;
        }
        // The message travels as JSON, so CR/LF written here arrive at the client as real control
        // bytes rather than as the two-character escapes: the encoder escapes them, the decoder
        // turns them back. That decode is what makes a parsed field sharper than raw wire bytes.
        raw_transport->enqueue_message(
            make_error_response(id, mcp::g_INTERNAL_ERROR, peer_message).dump());
    });

    mcp::Client client(transport, io_ctx.get_executor());

    PeerErrorOutcome outcome;
    boost::asio::co_spawn(
        io_ctx,
        [&]() -> mcp::Task<void> {
            mcp::Implementation info;
            info.name = "test-client";
            info.version = "0.1";
            static_cast<void>(co_await client.connect(std::move(info), mcp::ClientCapabilities{}));
            try {
                static_cast<void>(co_await client.send_request("tools/list", std::nullopt));
            } catch (const mcp::McpError& error) {
                outcome.threw = true;
                outcome.code = error.code();
                outcome.what = error.what();
                outcome.raw_message = error.message();
            }
            raw_transport->close();
        },
        boost::asio::detached);

    io_ctx.run();
    return outcome;
}

}  // namespace

// The message in a peer's error response is the peer's text, and it reaches what() -- which is
// what an application logs. A server that answers with CR/LF forges a line in that log, and a bidi
// override reorders the tail of the diagnostic so a refusal can be made to read as its opposite.
TEST_F(ClientCoreTest, PeerErrorDiagnosticFlattensThePeerChosenMessage) {
    // "zqtripwire" is a token no other code path produces; see the tripwire assertions below.
    // \xe2\x80\xae is U+202E RIGHT-TO-LEFT OVERRIDE, written escaped so this source file does not
    // itself contain a bidi override.
    const std::string forged =
        "denied\r\n2026-09-20 INFO zqtripwire operator approved\xe2\x80\xae reordered tail";

    const auto outcome = send_request_against_peer_error(io_ctx_, forged);

    // Tripwire, ordered before the flattening assertions. The throw has to be the McpError built
    // from the peer's own error object -- not a request timeout, not an envelope-validation
    // refusal, not a transport failure. Each of those also raises an McpError, with a message this
    // SDK authored, and would satisfy everything below without the peer's bytes ever reaching a
    // diagnostic. Dump what() on failure so a vacuous pass cannot hide.
    ASSERT_TRUE(outcome.threw) << "send_request() raised nothing at all";
    ASSERT_EQ(outcome.code, mcp::g_INTERNAL_ERROR) << "actual what(): " << outcome.what;
    ASSERT_EQ(outcome.what.rfind("JSON-RPC error ", 0), 0U) << "actual what(): " << outcome.what;
    ASSERT_NE(outcome.what.find("zqtripwire"), std::string::npos) << "actual what(): " << outcome.what;

    EXPECT_EQ(outcome.what.find('\r'), std::string::npos) << "actual what(): " << outcome.what;
    EXPECT_EQ(outcome.what.find('\n'), std::string::npos) << "actual what(): " << outcome.what;
    EXPECT_EQ(outcome.what.find("\xe2\x80\xae"), std::string::npos)
        << "actual what(): " << outcome.what;

    // The split, and it is the point: what() is a diagnostic and is sanitized; error() and the
    // message() it exposes are structured protocol data a caller may compare or re-encode, so they
    // must come back exactly as the peer sent them. Sanitizing those instead would silently change
    // what an application matches on.
    EXPECT_EQ(outcome.raw_message, forged);
}

// The same site must bound the value too, so a peer cannot flood the application's log through a
// megabyte-long error message.
TEST_F(ClientCoreTest, PeerErrorDiagnosticBoundsThePeerChosenMessage) {
    const std::string forged = "zqtripwire" + std::string(64 * 1024, 'A');

    const auto outcome = send_request_against_peer_error(io_ctx_, forged);

    ASSERT_TRUE(outcome.threw) << "send_request() raised nothing at all";
    ASSERT_EQ(outcome.code, mcp::g_INTERNAL_ERROR)
        << "actual what() prefix: " << outcome.what.substr(0, 80);
    ASSERT_EQ(outcome.what.rfind("JSON-RPC error ", 0), 0U)
        << "actual what() prefix: " << outcome.what.substr(0, 80);
    ASSERT_NE(outcome.what.find("zqtripwire"), std::string::npos)
        << "actual what() prefix: " << outcome.what.substr(0, 80);

    EXPECT_LT(outcome.what.size(), forged.size()) << "what() size: " << outcome.what.size();
    EXPECT_LE(outcome.what.size(), std::size_t{512}) << "what() size: " << outcome.what.size();
    EXPECT_EQ(outcome.raw_message.size(), forged.size())
        << "the structured message was truncated; only the diagnostic may be";
}

namespace {

// Drives two requests over one session. The peer answers the first with a caller-supplied burst of
// messages and the second normally.
//
// The second request is the whole point: it separates a failure confined to one message from a
// failure that took the session down with it. A client that survives a message it cannot use fails
// at most the first request; a client that does not fails the second too, and every request after
// it, for the life of the connection.
struct SecondRequestOutcome {
    bool first_threw{false};
    int first_code{0};
    std::string first_message;
    bool second_succeeded{false};
    std::string second_failure;
    std::vector<mcp::Error> reported;  ///< What ClientOptions::on_protocol_error saw.
};

SecondRequestOutcome send_two_requests(
    boost::asio::io_context& io_ctx,
    const std::function<std::vector<nlohmann::json>(const std::string& id)>& answer_first) {
    auto transport = std::make_shared<ScriptedTransport>(io_ctx.get_executor());
    auto* raw_transport = transport.get();

    SecondRequestOutcome outcome;

    int answered = 0;
    raw_transport->set_on_write([&](std::string_view msg) {
        const auto json_msg = nlohmann::json::parse(msg);
        if (!json_msg.contains("id")) {
            return;
        }
        const auto id = json_msg.at("id").get<std::string>();
        if (json_msg.value("method", "") == "initialize") {
            raw_transport->enqueue_message(make_result_response(id, make_initialize_result()).dump());
            return;
        }
        ++answered;
        if (answered == 1) {
            for (const auto& message : answer_first(id)) {
                raw_transport->enqueue_message(message.dump());
            }
        } else {
            raw_transport->enqueue_message(make_result_response(id, nlohmann::json::object()).dump());
        }
    });

    mcp::ClientOptions options;
    // Short enough that a response the client never delivers surfaces as a test failure instead of
    // a stalled run.
    options.request_timeout = std::chrono::seconds(5);
    options.on_protocol_error = [&](const mcp::Error& error) { outcome.reported.push_back(error); };
    mcp::Client client(transport, io_ctx.get_executor(), options);

    boost::asio::co_spawn(
        io_ctx,
        [&]() -> mcp::Task<void> {
            static_cast<void>(co_await client.connect("test-client", "0.1"));

            try {
                co_await client.ping();
            } catch (const mcp::McpError& error) {
                outcome.first_threw = true;
                outcome.first_code = error.code();
                outcome.first_message = error.what();
            }

            try {
                co_await client.ping();
                outcome.second_succeeded = true;
            } catch (const mcp::McpError& error) {
                outcome.second_failure = error.what();
            }

            raw_transport->close();
        },
        boost::asio::detached);

    io_ctx.run();
    return outcome;
}

}  // namespace

// JSON-RPC 2.0 requires `error.message`, but omitting it is an ordinary server-side slip, not an
// attack. It must cost the peer that one response, not the session.
TEST_F(ClientCoreTest, ErrorResponseWithoutMessageDoesNotStopReadLoop) {
    const auto outcome = send_two_requests(io_ctx_, [](const std::string& id) {
        return std::vector<nlohmann::json>{
            {{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", mcp::g_METHOD_NOT_FOUND}}}}};
    });

    EXPECT_TRUE(outcome.second_succeeded)
        << "the session died on one malformed response: " << outcome.second_failure;
    ASSERT_TRUE(outcome.first_threw);
    EXPECT_EQ(outcome.first_code, mcp::g_METHOD_NOT_FOUND)
        << "the error code did not survive the missing message: " << outcome.first_message;
}

// A peer that serializes absent optionals as explicit null -- the default for Go's encoding/json
// without omitempty -- sends `message: null` rather than omitting the key.
TEST_F(ClientCoreTest, ErrorResponseWithNullMessageDoesNotStopReadLoop) {
    const auto outcome = send_two_requests(io_ctx_, [](const std::string& id) {
        return std::vector<nlohmann::json>{
            {{"jsonrpc", "2.0"},
             {"id", id},
             {"error", {{"code", mcp::g_METHOD_NOT_FOUND}, {"message", nullptr}, {"data", nullptr}}}}};
    });

    EXPECT_TRUE(outcome.second_succeeded)
        << "the session died on one malformed response: " << outcome.second_failure;
    ASSERT_TRUE(outcome.first_threw);
    EXPECT_EQ(outcome.first_code, mcp::g_METHOD_NOT_FOUND);
}

// An `error` member that is not an object yields no code, so the request it answers fails -- but
// it still answers that request rather than leaving the caller to wait out its deadline, and the
// session continues.
TEST_F(ClientCoreTest, ErrorMemberThatIsNotAnObjectFailsOnlyItsOwnRequest) {
    const auto outcome = send_two_requests(io_ctx_, [](const std::string& id) {
        return std::vector<nlohmann::json>{{{"jsonrpc", "2.0"}, {"id", id}, {"error", "oops"}}};
    });

    EXPECT_TRUE(outcome.second_succeeded)
        << "the session died on one malformed response: " << outcome.second_failure;
    ASSERT_TRUE(outcome.first_threw);
    EXPECT_EQ(outcome.first_code, mcp::g_INVALID_REQUEST) << "actual: " << outcome.first_message;
}

// A message the client cannot classify at all is dropped. Dropping it silently would leave an
// application unable to tell a misbehaving peer from a quiet one, so it is reported first.
TEST_F(ClientCoreTest, UndecodableMessageIsReportedAndDropped) {
    const auto outcome = send_two_requests(io_ctx_, [](const std::string& id) {
        return std::vector<nlohmann::json>{
            nlohmann::json::array({"not", "an", "object"}),
            {{"jsonrpc", "2.0"}, {"id", id}, {"result", nlohmann::json::object()}}};
    });

    EXPECT_TRUE(outcome.second_succeeded)
        << "the session died on one malformed message: " << outcome.second_failure;
    EXPECT_FALSE(outcome.first_threw) << "actual: " << outcome.first_message;
    ASSERT_EQ(outcome.reported.size(), 1U) << "the drop was silent";
    EXPECT_EQ(outcome.reported.front().code, mcp::g_PARSE_ERROR);
}

// A request from the peer whose id is neither string nor integer can never be answered. Dispatching
// it would bury the failure in a detached coroutine; it is rejected where the drop is reported.
TEST_F(ClientCoreTest, PeerRequestWithUnusableIdDoesNotStopReadLoop) {
    const auto outcome = send_two_requests(io_ctx_, [](const std::string& id) {
        return std::vector<nlohmann::json>{
            {{"jsonrpc", "2.0"}, {"id", nlohmann::json::object()}, {"method", "ping"}},
            {{"jsonrpc", "2.0"}, {"id", id}, {"result", nlohmann::json::object()}}};
    });

    EXPECT_TRUE(outcome.second_succeeded)
        << "the session died on one malformed request: " << outcome.second_failure;
    EXPECT_FALSE(outcome.first_threw) << "actual: " << outcome.first_message;
    ASSERT_EQ(outcome.reported.size(), 1U) << "the drop was silent";
    EXPECT_EQ(outcome.reported.front().code, mcp::g_PARSE_ERROR);
}

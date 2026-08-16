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

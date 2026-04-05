#include "mcp/client.hpp"

#include <gtest/gtest.h>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <functional>
#include <memory>
#include <queue>
#include <string>
#include <string_view>
#include <vector>

namespace {

// Mock transport that captures outgoing messages and feeds scripted responses.
// write_message stores messages for inspection; read_message returns queued
// responses, suspending via a timer until one is available.
class ScriptedTransport final : public mcp::ITransport {
   public:
    explicit ScriptedTransport(const boost::asio::any_io_executor& executor)
        : strand_(boost::asio::make_strand(executor)), timer_(strand_) {
        timer_.expires_at(std::chrono::steady_clock::time_point::max());
    }

    mcp::Task<std::string> read_message() override {
        for (;;) {
            if (closed_) {
                throw std::runtime_error("transport closed");
            }
            if (!incoming_.empty()) {
                auto msg = std::move(incoming_.front());
                incoming_.pop();
                co_return msg;
            }
            timer_.expires_at(std::chrono::steady_clock::time_point::max());
            try {
                co_await timer_.async_wait(boost::asio::use_awaitable);
            } catch (const boost::system::system_error& err) {
                if (err.code() != boost::asio::error::operation_aborted) {
                    throw;
                }
            }
        }
    }

    mcp::Task<void> write_message(std::string_view message) override {
        co_await boost::asio::post(strand_, boost::asio::use_awaitable);
        written_.emplace_back(message);
        if (on_write_) {
            on_write_(written_.back());
        }
    }

    void close() override {
        closed_ = true;
        timer_.cancel();
    }

    void enqueue_response(std::string msg) {
        boost::asio::post(strand_, [this, m = std::move(msg)]() mutable {
            incoming_.push(std::move(m));
            timer_.cancel();
        });
    }

    void set_on_write(std::function<void(std::string_view)> callback) {
        on_write_ = std::move(callback);
    }

    [[nodiscard]] const std::vector<std::string>& written() const { return written_; }
    [[nodiscard]] bool is_closed() const { return closed_; }

   private:
    boost::asio::strand<boost::asio::any_io_executor> strand_;
    boost::asio::steady_timer timer_;
    std::queue<std::string> incoming_;
    std::vector<std::string> written_;
    std::function<void(std::string_view)> on_write_;
    bool closed_ = false;
};

nlohmann::json make_result_response(std::string_view id, nlohmann::json result) {
    return {{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}};
}

nlohmann::json make_error_response(std::string_view id, int code, std::string_view message) {
    return {{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}};
}

nlohmann::json make_initialize_result() {
    return {{"protocolVersion", mcp::LATEST_PROTOCOL_VERSION},
            {"capabilities", nlohmann::json::object()},
            {"serverInfo", {{"name", "test-server"}, {"version", "1.0"}}}};
}

}  // namespace

class ClientCoreTest : public ::testing::Test {
   protected:
    boost::asio::io_context io_ctx_;
};

TEST_F(ClientCoreTest, SendRequestAddsToMapAndReturnsResult) {
    auto* raw_transport = new ScriptedTransport(io_ctx_.get_executor());
    auto transport = std::unique_ptr<mcp::ITransport>(raw_transport);

    raw_transport->set_on_write([raw_transport](std::string_view msg) {
        auto json_msg = nlohmann::json::parse(msg);
        if (json_msg.contains("id")) {
            auto id = json_msg["id"].get<std::string>();
            auto method = json_msg.value("method", "");
            if (method == "initialize") {
                raw_transport->enqueue_response(
                    make_result_response(id, make_initialize_result()).dump());
            } else {
                raw_transport->enqueue_response(make_result_response(id, {{"answer", 42}}).dump());
            }
        }
    });

    mcp::Client client(std::move(transport), io_ctx_.get_executor());

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

TEST_F(ClientCoreTest, ConnectHandshakeFollowsMcpOrder) {
    auto* raw_transport = new ScriptedTransport(io_ctx_.get_executor());
    auto transport = std::unique_ptr<mcp::ITransport>(raw_transport);

    raw_transport->set_on_write([raw_transport](std::string_view msg) {
        auto json_msg = nlohmann::json::parse(msg);
        if (json_msg.contains("id")) {
            auto id = json_msg["id"].get<std::string>();
            raw_transport->enqueue_response(make_result_response(id, make_initialize_result()).dump());
        }
    });

    mcp::Client client(std::move(transport), io_ctx_.get_executor());

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
    EXPECT_EQ(first["params"]["protocolVersion"], mcp::LATEST_PROTOCOL_VERSION);
    EXPECT_EQ(first["params"]["clientInfo"]["name"], "test-client");
    EXPECT_EQ(first["params"]["clientInfo"]["version"], "0.1");

    auto second = nlohmann::json::parse(raw_transport->written()[1]);
    EXPECT_EQ(second["method"], "notifications/initialized");
    EXPECT_FALSE(second.contains("id"));
    EXPECT_EQ(second["jsonrpc"], "2.0");

    EXPECT_EQ(init_result.protocolVersion, mcp::LATEST_PROTOCOL_VERSION);
    EXPECT_EQ(init_result.serverInfo.name, "test-server");
    EXPECT_EQ(init_result.serverInfo.version, "1.0");
}

TEST_F(ClientCoreTest, SendRequestErrorResponseThrows) {
    auto* raw_transport = new ScriptedTransport(io_ctx_.get_executor());
    auto transport = std::unique_ptr<mcp::ITransport>(raw_transport);

    raw_transport->set_on_write([raw_transport](std::string_view msg) {
        auto json_msg = nlohmann::json::parse(msg);
        if (json_msg.contains("id") && json_msg["method"] == "initialize") {
            auto id = json_msg["id"].get<std::string>();
            raw_transport->enqueue_response(make_result_response(id, make_initialize_result()).dump());
        } else if (json_msg.contains("id")) {
            auto id = json_msg["id"].get<std::string>();
            raw_transport->enqueue_response(
                make_error_response(id, mcp::METHOD_NOT_FOUND, "method not found").dump());
        }
    });

    mcp::Client client(std::move(transport), io_ctx_.get_executor());

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
        EXPECT_TRUE(msg.find("-32601") != std::string::npos);
        EXPECT_TRUE(msg.find("method not found") != std::string::npos);
    }
}

TEST_F(ClientCoreTest, SendNotificationDoesNotExpectResponse) {
    auto* raw_transport = new ScriptedTransport(io_ctx_.get_executor());
    auto transport = std::unique_ptr<mcp::ITransport>(raw_transport);

    raw_transport->set_on_write([raw_transport](std::string_view msg) {
        auto json_msg = nlohmann::json::parse(msg);
        if (json_msg.contains("id")) {
            auto id = json_msg["id"].get<std::string>();
            raw_transport->enqueue_response(make_result_response(id, make_initialize_result()).dump());
        }
    });

    mcp::Client client(std::move(transport), io_ctx_.get_executor());

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
    auto* raw_transport = new ScriptedTransport(io_ctx_.get_executor());
    auto transport = std::unique_ptr<mcp::ITransport>(raw_transport);

    raw_transport->set_on_write([raw_transport](std::string_view msg) {
        auto json_msg = nlohmann::json::parse(msg);
        if (json_msg.contains("id")) {
            auto id = json_msg["id"].get<std::string>();
            auto method = json_msg.value("method", "");
            if (method == "initialize") {
                raw_transport->enqueue_response(
                    make_result_response(id, make_initialize_result()).dump());
            } else {
                raw_transport->enqueue_response(
                    make_result_response(id, {{"method_echo", method}}).dump());
            }
        }
    });

    mcp::Client client(std::move(transport), io_ctx_.get_executor());

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
    auto* raw_transport = new ScriptedTransport(io_ctx_.get_executor());
    auto transport = std::unique_ptr<mcp::ITransport>(raw_transport);

    raw_transport->set_on_write([raw_transport](std::string_view msg) {
        auto json_msg = nlohmann::json::parse(msg);
        if (json_msg.contains("id")) {
            auto id = json_msg["id"].get<std::string>();
            auto method = json_msg.value("method", "");
            if (method == "initialize") {
                raw_transport->enqueue_response(
                    make_result_response(id, make_initialize_result()).dump());
            } else {
                nlohmann::json server_notif = {{"jsonrpc", "2.0"},
                                               {"method", "notifications/tools/list_changed"}};
                raw_transport->enqueue_response(server_notif.dump());
                raw_transport->enqueue_response(make_result_response(id, {{"status", "ok"}}).dump());
            }
        }
    });

    mcp::Client client(std::move(transport), io_ctx_.get_executor());

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
    auto* raw_transport = new ScriptedTransport(io_ctx_.get_executor());
    auto transport = std::unique_ptr<mcp::ITransport>(raw_transport);

    std::size_t count_during_flight = 0;

    mcp::Client client(std::move(transport), io_ctx_.get_executor());

    raw_transport->set_on_write([raw_transport, &count_during_flight, &client](std::string_view msg) {
        auto json_msg = nlohmann::json::parse(msg);
        if (json_msg.contains("id")) {
            auto id = json_msg["id"].get<std::string>();
            auto method = json_msg.value("method", "");
            if (method == "initialize") {
                raw_transport->enqueue_response(
                    make_result_response(id, make_initialize_result()).dump());
            } else {
                count_during_flight = client.pending_request_count();
                raw_transport->enqueue_response(
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
    auto* raw_transport = new ScriptedTransport(io_ctx_.get_executor());
    auto transport = std::unique_ptr<mcp::ITransport>(raw_transport);

    auto init_result_json = make_initialize_result();
    init_result_json["capabilities"]["tools"] = {{"listChanged", true}};
    init_result_json["instructions"] = "Welcome to the test server";

    raw_transport->set_on_write(
        [raw_transport, init_result_json = std::move(init_result_json)](std::string_view msg) {
            auto json_msg = nlohmann::json::parse(msg);
            if (json_msg.contains("id")) {
                auto id = json_msg["id"].get<std::string>();
                raw_transport->enqueue_response(make_result_response(id, init_result_json).dump());
            }
        });

    mcp::Client client(std::move(transport), io_ctx_.get_executor());

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

    EXPECT_EQ(init_result.protocolVersion, mcp::LATEST_PROTOCOL_VERSION);
    EXPECT_EQ(init_result.serverInfo.name, "test-server");
    ASSERT_TRUE(init_result.instructions.has_value());
    EXPECT_EQ(*init_result.instructions, "Welcome to the test server");
    ASSERT_TRUE(init_result.capabilities.tools.has_value());
    ASSERT_TRUE(init_result.capabilities.tools->listChanged.has_value());
    EXPECT_TRUE(*init_result.capabilities.tools->listChanged);
}

TEST_F(ClientCoreTest, PingSendsRequestAndReceivesResponse) {
    auto* raw_transport = new ScriptedTransport(io_ctx_.get_executor());
    auto transport = std::unique_ptr<mcp::ITransport>(raw_transport);

    raw_transport->set_on_write([raw_transport](std::string_view msg) {
        auto json_msg = nlohmann::json::parse(msg);
        if (json_msg.contains("id")) {
            auto id = json_msg["id"].get<std::string>();
            auto method = json_msg.value("method", "");
            if (method == "initialize") {
                raw_transport->enqueue_response(
                    make_result_response(id, make_initialize_result()).dump());
            } else if (method == "ping") {
                raw_transport->enqueue_response(
                    make_result_response(id, nlohmann::json::object()).dump());
            }
        }
    });

    mcp::Client client(std::move(transport), io_ctx_.get_executor());

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

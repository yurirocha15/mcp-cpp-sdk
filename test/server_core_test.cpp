#include "mcp/context.hpp"
#include "mcp/server.hpp"

#include <gtest/gtest.h>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <functional>
#include <memory>
#include <queue>
#include <string>
#include <string_view>
#include <vector>

namespace {

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

    void enqueue_message(std::string msg) {
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

nlohmann::json make_initialize_request(std::string_view id) {
    return {{"jsonrpc", "2.0"},
            {"id", id},
            {"method", "initialize"},
            {"params",
             {{"protocolVersion", mcp::LATEST_PROTOCOL_VERSION},
              {"clientInfo", {{"name", "test-client"}, {"version", "0.1"}}},
              {"capabilities", nlohmann::json::object()}}}};
}

nlohmann::json make_shutdown_request(std::string_view id) {
    return {{"jsonrpc", "2.0"}, {"id", id}, {"method", "shutdown"}};
}

}  // namespace

class ServerCoreTest : public ::testing::Test {
   protected:
    boost::asio::io_context io_ctx_;
};

TEST_F(ServerCoreTest, InitializeReturnsServerCapabilities) {
    auto* raw_transport = new ScriptedTransport(io_ctx_.get_executor());
    auto transport = std::unique_ptr<mcp::ITransport>(raw_transport);

    mcp::Implementation server_info;
    server_info.name = "test-server";
    server_info.version = "1.0";

    mcp::ServerCapabilities caps;
    mcp::ServerCapabilities::ToolsCapability tools_cap;
    tools_cap.listChanged = true;
    caps.tools = std::move(tools_cap);

    mcp::Server server(std::move(server_info), std::move(caps));

    nlohmann::json response;
    raw_transport->set_on_write([&response, raw_transport](std::string_view msg) {
        response = nlohmann::json::parse(msg);
        raw_transport->close();
    });

    nlohmann::json init_req = make_initialize_request("1");
    raw_transport->enqueue_message(init_req.dump());

    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> { co_await server.run(std::move(transport), io_ctx_.get_executor()); },
        boost::asio::detached);

    io_ctx_.run();

    ASSERT_TRUE(response.contains("result"));
    ASSERT_TRUE(response.contains("id"));
    EXPECT_EQ(response["id"], "1");
    EXPECT_EQ(response["jsonrpc"], "2.0");

    auto result = response["result"];
    EXPECT_EQ(result["protocolVersion"], std::string(mcp::LATEST_PROTOCOL_VERSION));
    EXPECT_EQ(result["serverInfo"]["name"], "test-server");
    EXPECT_EQ(result["serverInfo"]["version"], "1.0");
    ASSERT_TRUE(result["capabilities"].contains("tools"));
    EXPECT_TRUE(result["capabilities"]["tools"]["listChanged"]);

    EXPECT_TRUE(server.is_initialized());
}

TEST_F(ServerCoreTest, ShutdownSetsFlag) {
    auto* raw_transport = new ScriptedTransport(io_ctx_.get_executor());
    auto transport = std::unique_ptr<mcp::ITransport>(raw_transport);

    mcp::Implementation server_info;
    server_info.name = "test-server";
    server_info.version = "1.0";

    mcp::Server server(std::move(server_info), mcp::ServerCapabilities{});

    std::vector<nlohmann::json> responses;
    raw_transport->set_on_write([&responses, raw_transport](std::string_view msg) {
        responses.push_back(nlohmann::json::parse(msg));
        if (responses.size() == 2) {
            raw_transport->close();
        }
    });

    nlohmann::json init_req = make_initialize_request("1");
    nlohmann::json shutdown_req = make_shutdown_request("2");
    raw_transport->enqueue_message(init_req.dump());
    raw_transport->enqueue_message(shutdown_req.dump());

    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> { co_await server.run(std::move(transport), io_ctx_.get_executor()); },
        boost::asio::detached);

    io_ctx_.run();

    ASSERT_EQ(responses.size(), 2);

    EXPECT_EQ(responses[0]["id"], "1");
    ASSERT_TRUE(responses[0].contains("result"));

    EXPECT_EQ(responses[1]["id"], "2");
    ASSERT_TRUE(responses[1].contains("result"));
    EXPECT_EQ(responses[1]["result"], nlohmann::json::object());

    EXPECT_TRUE(server.is_initialized());
    EXPECT_TRUE(server.is_shutdown_requested());
}

TEST_F(ServerCoreTest, UnknownMethodReturnsError) {
    auto* raw_transport = new ScriptedTransport(io_ctx_.get_executor());
    auto transport = std::unique_ptr<mcp::ITransport>(raw_transport);

    mcp::Implementation server_info;
    server_info.name = "test-server";
    server_info.version = "1.0";

    mcp::Server server(std::move(server_info), mcp::ServerCapabilities{});

    nlohmann::json response;
    raw_transport->set_on_write([&response, raw_transport](std::string_view msg) {
        response = nlohmann::json::parse(msg);
        raw_transport->close();
    });

    nlohmann::json unknown_req = {{"jsonrpc", "2.0"}, {"id", "42"}, {"method", "nonexistent/method"}};
    raw_transport->enqueue_message(unknown_req.dump());

    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> { co_await server.run(std::move(transport), io_ctx_.get_executor()); },
        boost::asio::detached);

    io_ctx_.run();

    ASSERT_TRUE(response.contains("error"));
    EXPECT_EQ(response["id"], "42");
    EXPECT_EQ(response["error"]["code"], mcp::METHOD_NOT_FOUND);
    EXPECT_TRUE(response["error"]["message"].get<std::string>().find("nonexistent/method") !=
                std::string::npos);
}

TEST_F(ServerCoreTest, NotificationsAreSilentlyIgnored) {
    auto* raw_transport = new ScriptedTransport(io_ctx_.get_executor());
    auto transport = std::unique_ptr<mcp::ITransport>(raw_transport);

    mcp::Implementation server_info;
    server_info.name = "test-server";
    server_info.version = "1.0";

    mcp::Server server(std::move(server_info), mcp::ServerCapabilities{});

    nlohmann::json response;
    bool got_response = false;
    raw_transport->set_on_write([&response, &got_response, raw_transport](std::string_view msg) {
        response = nlohmann::json::parse(msg);
        got_response = true;
        raw_transport->close();
    });

    nlohmann::json notification = {{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}};
    nlohmann::json init_req = make_initialize_request("1");
    raw_transport->enqueue_message(notification.dump());
    raw_transport->enqueue_message(init_req.dump());

    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> { co_await server.run(std::move(transport), io_ctx_.get_executor()); },
        boost::asio::detached);

    io_ctx_.run();

    EXPECT_TRUE(got_response);
    EXPECT_EQ(response["id"], "1");
    ASSERT_TRUE(response.contains("result"));
}

TEST_F(ServerCoreTest, ContextLogInfoSendsNotification) {
    auto* raw_transport = new ScriptedTransport(io_ctx_.get_executor());

    nlohmann::json notification;
    raw_transport->set_on_write(
        [&notification](std::string_view msg) { notification = nlohmann::json::parse(msg); });

    mcp::Context ctx(*raw_transport);

    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            co_await ctx.log_info("hello world");
            raw_transport->close();
        },
        boost::asio::detached);

    io_ctx_.run();

    EXPECT_EQ(notification["jsonrpc"], "2.0");
    EXPECT_EQ(notification["method"], "notifications/message");
    ASSERT_TRUE(notification.contains("params"));
    EXPECT_EQ(notification["params"]["level"], "info");
    EXPECT_EQ(notification["params"]["data"], "hello world");
    EXPECT_FALSE(notification.contains("id"));
}

TEST_F(ServerCoreTest, ContextLogInfoMultipleMessages) {
    auto* raw_transport = new ScriptedTransport(io_ctx_.get_executor());

    std::vector<nlohmann::json> notifications;
    raw_transport->set_on_write([&notifications](std::string_view msg) {
        notifications.push_back(nlohmann::json::parse(msg));
    });

    mcp::Context ctx(*raw_transport);

    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            co_await ctx.log_info("first");
            co_await ctx.log_info("second");
            raw_transport->close();
        },
        boost::asio::detached);

    io_ctx_.run();

    ASSERT_EQ(notifications.size(), 2);
    EXPECT_EQ(notifications[0]["params"]["data"], "first");
    EXPECT_EQ(notifications[1]["params"]["data"], "second");
}

TEST_F(ServerCoreTest, DispatchDirectlyWithoutRun) {
    mcp::Implementation server_info;
    server_info.name = "test-server";
    server_info.version = "1.0";

    auto* raw_transport = new ScriptedTransport(io_ctx_.get_executor());
    auto transport = std::unique_ptr<mcp::ITransport>(raw_transport);

    mcp::Server server(std::move(server_info), mcp::ServerCapabilities{});

    nlohmann::json response;
    raw_transport->set_on_write(
        [&response](std::string_view msg) { response = nlohmann::json::parse(msg); });

    raw_transport->set_on_write([&response, raw_transport](std::string_view msg) {
        response = nlohmann::json::parse(msg);
        raw_transport->close();
    });

    nlohmann::json init_req = make_initialize_request("10");
    raw_transport->enqueue_message(init_req.dump());

    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> { co_await server.run(std::move(transport), io_ctx_.get_executor()); },
        boost::asio::detached);

    io_ctx_.run();

    EXPECT_EQ(response["id"], "10");
    ASSERT_TRUE(response.contains("result"));
    EXPECT_FALSE(server.is_shutdown_requested());
}

TEST_F(ServerCoreTest, PingHandlerReturnsEmptyResult) {
    auto* raw_transport = new ScriptedTransport(io_ctx_.get_executor());
    auto transport = std::unique_ptr<mcp::ITransport>(raw_transport);

    mcp::Implementation server_info;
    server_info.name = "test-server";
    server_info.version = "1.0";

    mcp::Server server(std::move(server_info), mcp::ServerCapabilities{});

    nlohmann::json response;
    raw_transport->set_on_write([&response, raw_transport](std::string_view msg) {
        response = nlohmann::json::parse(msg);
        raw_transport->close();
    });

    nlohmann::json ping_req = {{"jsonrpc", "2.0"}, {"id", "99"}, {"method", "ping"}};
    raw_transport->enqueue_message(ping_req.dump());

    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> { co_await server.run(std::move(transport), io_ctx_.get_executor()); },
        boost::asio::detached);

    io_ctx_.run();

    ASSERT_TRUE(response.contains("result"));
    EXPECT_EQ(response["id"], "99");
    EXPECT_EQ(response["result"], nlohmann::json::object());
}

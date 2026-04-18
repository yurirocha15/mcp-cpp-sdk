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

   private:
    boost::asio::strand<boost::asio::any_io_executor> strand_;
    boost::asio::steady_timer timer_;
    std::queue<std::string> incoming_;
    std::vector<std::string> written_;
    std::function<void(std::string_view)> on_write_;
    bool closed_ = false;
};

struct NoopParams {
    int dummy = 0;
};

inline void to_json(nlohmann::json& j, const NoopParams& p) { j = nlohmann::json{{"dummy", p.dummy}}; }
inline void from_json(const nlohmann::json& j, NoopParams& p) { j.at("dummy").get_to(p.dummy); }

struct NoopResult {
    bool done = true;
};

inline void to_json(nlohmann::json& j, const NoopResult& r) { j = nlohmann::json{{"done", r.done}}; }
inline void from_json(const nlohmann::json& j, NoopResult& r) { j.at("done").get_to(r.done); }

nlohmann::json make_initialize_request(std::string_view req_id) {
    return {{"jsonrpc", "2.0"},
            {"id", req_id},
            {"method", "initialize"},
            {"params",
             {{"protocolVersion", mcp::LATEST_PROTOCOL_VERSION},
              {"clientInfo", {{"name", "test-client"}, {"version", "0.1"}}},
              {"capabilities", nlohmann::json::object()}}}};
}

}  // namespace

class ServerLoggingTest : public ::testing::Test {
   protected:
    boost::asio::io_context io_ctx_;
};

TEST_F(ServerLoggingTest, SetLevelChangesServerLogLevel) {
    auto transport = std::make_shared<ScriptedTransport>(io_ctx_.get_executor());
    auto* raw_transport = transport.get();

    mcp::Implementation info;
    info.name = "test-server";
    info.version = "1.0";

    mcp::ServerCapabilities caps;
    caps.logging = nlohmann::json::object();

    mcp::Server server(std::move(info), std::move(caps));

    // Default level is Debug
    EXPECT_EQ(server.get_log_level(), mcp::LoggingLevel::Debug);

    std::vector<nlohmann::json> responses;
    raw_transport->set_on_write([&responses, raw_transport](std::string_view msg) {
        responses.push_back(nlohmann::json::parse(msg));
        if (responses.size() == 2) {
            raw_transport->close();
        }
    });

    raw_transport->enqueue_message(make_initialize_request("1").dump());

    nlohmann::json set_level_req;
    set_level_req["jsonrpc"] = "2.0";
    set_level_req["id"] = "2";
    set_level_req["method"] = "logging/setLevel";
    set_level_req["params"] = nlohmann::json{{"level", "warning"}};
    raw_transport->enqueue_message(set_level_req.dump());

    boost::asio::co_spawn(
        io_ctx_, [&]() -> mcp::Task<void> { co_await server.run(transport, io_ctx_.get_executor()); },
        boost::asio::detached);

    io_ctx_.run();

    ASSERT_EQ(responses.size(), 2);
    EXPECT_EQ(responses[1]["id"], "2");
    EXPECT_EQ(responses[1]["result"], nlohmann::json::object());

    EXPECT_EQ(server.get_log_level(), mcp::LoggingLevel::Warning);
}

TEST_F(ServerLoggingTest, LogFilteredByLevel) {
    auto transport = std::make_shared<ScriptedTransport>(io_ctx_.get_executor());
    auto* raw_transport = transport.get();

    mcp::Implementation info;
    info.name = "test-server";
    info.version = "1.0";

    mcp::ServerCapabilities caps;
    mcp::ServerCapabilities::ToolsCapability tools_cap;
    caps.tools = std::move(tools_cap);
    caps.logging = nlohmann::json::object();

    mcp::Server server(std::move(info), std::move(caps));

    server.add_tool<NoopParams, NoopResult>(
        "log_tool", "Logs at multiple levels", nlohmann::json{{"type", "object"}},
        [](mcp::Context& ctx, NoopParams /*params*/) -> mcp::Task<NoopResult> {
            co_await ctx.log(mcp::LoggingLevel::Error, "error message");
            co_await ctx.log(mcp::LoggingLevel::Debug, "debug message");
            co_return NoopResult{true};
        });

    std::vector<nlohmann::json> responses;
    raw_transport->set_on_write([&responses, raw_transport](std::string_view msg) {
        responses.push_back(nlohmann::json::parse(msg));
        // init + setLevel result + error log + tool result = 4
        // (debug log filtered out because level set to Warning)
        if (responses.size() == 4) {
            raw_transport->close();
        }
    });

    raw_transport->enqueue_message(make_initialize_request("1").dump());

    // Set level to Warning (filters out Info and Debug)
    nlohmann::json set_level_req;
    set_level_req["jsonrpc"] = "2.0";
    set_level_req["id"] = "2";
    set_level_req["method"] = "logging/setLevel";
    set_level_req["params"] = nlohmann::json{{"level", "warning"}};
    raw_transport->enqueue_message(set_level_req.dump());

    nlohmann::json call_req;
    call_req["jsonrpc"] = "2.0";
    call_req["id"] = "3";
    call_req["method"] = "tools/call";
    call_req["params"] = nlohmann::json{{"name", "log_tool"}, {"arguments", {{"dummy", 0}}}};
    raw_transport->enqueue_message(call_req.dump());

    boost::asio::co_spawn(
        io_ctx_, [&]() -> mcp::Task<void> { co_await server.run(transport, io_ctx_.get_executor()); },
        boost::asio::detached);

    io_ctx_.run();

    ASSERT_EQ(responses.size(), 4);

    // setLevel result
    EXPECT_EQ(responses[1]["id"], "2");
    EXPECT_EQ(responses[1]["result"], nlohmann::json::object());

    // Only error log came through (debug was filtered)
    auto& log_notif = responses[2];
    EXPECT_EQ(log_notif["method"], "notifications/message");
    EXPECT_EQ(log_notif["params"]["level"], "error");
    EXPECT_EQ(log_notif["params"]["data"], "error message");

    // Tool result
    EXPECT_EQ(responses[3]["id"], "3");
    ASSERT_TRUE(responses[3].contains("result"));
}

TEST_F(ServerLoggingTest, LogWithLoggerField) {
    auto transport = std::make_shared<ScriptedTransport>(io_ctx_.get_executor());
    auto* raw_transport = transport.get();

    mcp::Implementation info;
    info.name = "test-server";
    info.version = "1.0";

    mcp::ServerCapabilities caps;
    mcp::ServerCapabilities::ToolsCapability tools_cap;
    caps.tools = std::move(tools_cap);

    mcp::Server server(std::move(info), std::move(caps));

    server.add_tool<NoopParams, NoopResult>(
        "logger_tool", "Logs with logger name", nlohmann::json{{"type", "object"}},
        [](mcp::Context& ctx, NoopParams /*params*/) -> mcp::Task<NoopResult> {
            co_await ctx.log(mcp::LoggingLevel::Info, "tagged message", "my-component");
            co_return NoopResult{true};
        });

    std::vector<nlohmann::json> responses;
    raw_transport->set_on_write([&responses, raw_transport](std::string_view msg) {
        responses.push_back(nlohmann::json::parse(msg));
        // init + log + result = 3
        if (responses.size() == 3) {
            raw_transport->close();
        }
    });

    raw_transport->enqueue_message(make_initialize_request("1").dump());

    nlohmann::json call_req;
    call_req["jsonrpc"] = "2.0";
    call_req["id"] = "2";
    call_req["method"] = "tools/call";
    call_req["params"] = nlohmann::json{{"name", "logger_tool"}, {"arguments", {{"dummy", 0}}}};
    raw_transport->enqueue_message(call_req.dump());

    boost::asio::co_spawn(
        io_ctx_, [&]() -> mcp::Task<void> { co_await server.run(transport, io_ctx_.get_executor()); },
        boost::asio::detached);

    io_ctx_.run();

    ASSERT_EQ(responses.size(), 3);

    auto& log_notif = responses[1];
    EXPECT_EQ(log_notif["method"], "notifications/message");
    EXPECT_EQ(log_notif["params"]["level"], "info");
    EXPECT_EQ(log_notif["params"]["data"], "tagged message");
    EXPECT_EQ(log_notif["params"]["logger"], "my-component");
}

TEST_F(ServerLoggingTest, ListChangedNotificationsSent) {
    auto transport = std::make_shared<ScriptedTransport>(io_ctx_.get_executor());
    auto* raw_transport = transport.get();

    mcp::Implementation info;
    info.name = "test-server";
    info.version = "1.0";

    mcp::ServerCapabilities caps;
    mcp::ServerCapabilities::ToolsCapability tools_cap;
    tools_cap.listChanged = true;
    caps.tools = std::move(tools_cap);

    mcp::Server server(std::move(info), std::move(caps));

    server.add_tool<nlohmann::json, nlohmann::json>(
        "trigger_list_changed", "Triggers list changed", nlohmann::json{{"type", "object"}},
        [&server](mcp::Context& /*ctx*/, nlohmann::json /*params*/) -> mcp::Task<nlohmann::json> {
            co_await server.notify_tools_list_changed();
            co_return nlohmann::json{{"done", true}};
        });

    std::vector<nlohmann::json> responses;
    raw_transport->set_on_write([&responses, raw_transport](std::string_view msg) {
        responses.push_back(nlohmann::json::parse(msg));
        // init + list_changed notification + tool result = 3
        if (responses.size() == 3) {
            raw_transport->close();
        }
    });

    raw_transport->enqueue_message(make_initialize_request("1").dump());

    nlohmann::json call_req;
    call_req["jsonrpc"] = "2.0";
    call_req["id"] = "2";
    call_req["method"] = "tools/call";
    call_req["params"] =
        nlohmann::json{{"name", "trigger_list_changed"}, {"arguments", nlohmann::json::object()}};
    raw_transport->enqueue_message(call_req.dump());

    boost::asio::co_spawn(
        io_ctx_, [&]() -> mcp::Task<void> { co_await server.run(transport, io_ctx_.get_executor()); },
        boost::asio::detached);

    io_ctx_.run();

    ASSERT_EQ(responses.size(), 3);

    // List changed notification
    auto& notif = responses[1];
    EXPECT_EQ(notif["method"], "notifications/tools/list_changed");
    EXPECT_FALSE(notif.contains("id"));

    // Tool result
    EXPECT_EQ(responses[2]["id"], "2");
    ASSERT_TRUE(responses[2].contains("result"));
}

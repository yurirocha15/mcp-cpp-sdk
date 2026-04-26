#include "test_utils.hpp"

#include "mcp/server.hpp"

#include <gtest/gtest.h>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace {

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
    EXPECT_EQ(server.get_log_level(), mcp::LoggingLevel::eDebug);

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

    EXPECT_EQ(server.get_log_level(), mcp::LoggingLevel::eWarning);
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
            co_await ctx.log(mcp::LoggingLevel::eError, "error message");
            co_await ctx.log(mcp::LoggingLevel::eDebug, "debug message");
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
            co_await ctx.log(mcp::LoggingLevel::eInfo, "tagged message", "my-component");
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

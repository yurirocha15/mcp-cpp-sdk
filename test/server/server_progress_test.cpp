#include "../test_utils.hpp"

#include "mcp/server/server.hpp"

#include <gtest/gtest.h>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

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

class ServerProgressTest : public ::testing::Test {
   protected:
    boost::asio::io_context io_ctx_;
};

TEST_F(ServerProgressTest, ProgressNotificationSentWithToken) {
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
        "progress_tool", "Reports progress", nlohmann::json{{"type", "object"}},
        [](mcp::Context& ctx, NoopParams /*params*/) -> mcp::Task<NoopResult> {
            co_await ctx.report_progress(0.5, 1.0, "halfway there");
            co_return NoopResult{true};
        });

    std::vector<nlohmann::json> responses;
    raw_transport->set_on_write([&responses, raw_transport](std::string_view msg) {
        responses.push_back(nlohmann::json::parse(msg));
        // init + progress notification + tool result = 3
        if (responses.size() == 3) {
            raw_transport->close();
        }
    });

    raw_transport->enqueue_message(make_initialize_request("1").dump());
    raw_transport->enqueue_message(make_initialized_notification().dump());

    nlohmann::json call_req;
    call_req["jsonrpc"] = "2.0";
    call_req["id"] = "2";
    call_req["method"] = "tools/call";
    call_req["params"] = nlohmann::json{{"name", "progress_tool"},
                                        {"arguments", {{"dummy", 0}}},
                                        {"_meta", {{"progressToken", "tok-123"}}}};
    raw_transport->enqueue_message(call_req.dump());

    boost::asio::co_spawn(
        io_ctx_, [&]() -> mcp::Task<void> { co_await server.run(transport, io_ctx_.get_executor()); },
        boost::asio::detached);

    io_ctx_.run();

    ASSERT_EQ(responses.size(), 3);

    // Second message is the progress notification
    auto& progress = responses[1];
    EXPECT_EQ(progress["method"], "notifications/progress");
    EXPECT_FALSE(progress.contains("id"));
    ASSERT_TRUE(progress.contains("params"));
    EXPECT_EQ(progress["params"]["progressToken"], "tok-123");
    EXPECT_DOUBLE_EQ(progress["params"]["progress"].get<double>(), 0.5);
    EXPECT_DOUBLE_EQ(progress["params"]["total"].get<double>(), 1.0);
    EXPECT_EQ(progress["params"]["message"], "halfway there");

    // Third is the tool result
    EXPECT_EQ(responses[2]["id"], "2");
    ASSERT_TRUE(responses[2].contains("result"));
}

TEST_F(ServerProgressTest, ProgressSilentWithoutToken) {
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
        "silent_progress", "Reports progress but no token", nlohmann::json{{"type", "object"}},
        [](mcp::Context& ctx, NoopParams /*params*/) -> mcp::Task<NoopResult> {
            co_await ctx.report_progress(0.5, 1.0, "should not appear");
            co_return NoopResult{true};
        });

    std::vector<nlohmann::json> responses;
    raw_transport->set_on_write([&responses, raw_transport](std::string_view msg) {
        responses.push_back(nlohmann::json::parse(msg));
        // init + tool result = 2 (no progress notification because no token)
        if (responses.size() == 2) {
            raw_transport->close();
        }
    });

    raw_transport->enqueue_message(make_initialize_request("1").dump());
    raw_transport->enqueue_message(make_initialized_notification().dump());

    // No _meta.progressToken in this request
    nlohmann::json call_req;
    call_req["jsonrpc"] = "2.0";
    call_req["id"] = "2";
    call_req["method"] = "tools/call";
    call_req["params"] = nlohmann::json{{"name", "silent_progress"}, {"arguments", {{"dummy", 0}}}};
    raw_transport->enqueue_message(call_req.dump());

    boost::asio::co_spawn(
        io_ctx_, [&]() -> mcp::Task<void> { co_await server.run(transport, io_ctx_.get_executor()); },
        boost::asio::detached);

    io_ctx_.run();

    // Only 2 responses: init + tool result (no progress notification)
    ASSERT_EQ(responses.size(), 2);
    EXPECT_EQ(responses[0]["id"], "1");
    EXPECT_EQ(responses[1]["id"], "2");
    ASSERT_TRUE(responses[1].contains("result"));
}

TEST_F(ServerProgressTest, MultipleProgressNotifications) {
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
        "multi_progress", "Reports multiple progress updates", nlohmann::json{{"type", "object"}},
        [](mcp::Context& ctx, NoopParams /*params*/) -> mcp::Task<NoopResult> {
            co_await ctx.report_progress(1.0, 3.0);
            co_await ctx.report_progress(2.0, 3.0);
            co_await ctx.report_progress(3.0, 3.0);
            co_return NoopResult{true};
        });

    std::vector<nlohmann::json> responses;
    raw_transport->set_on_write([&responses, raw_transport](std::string_view msg) {
        responses.push_back(nlohmann::json::parse(msg));
        // init + 3 progress + tool result = 5
        if (responses.size() == 5) {
            raw_transport->close();
        }
    });

    raw_transport->enqueue_message(make_initialize_request("1").dump());
    raw_transport->enqueue_message(make_initialized_notification().dump());

    nlohmann::json call_req;
    call_req["jsonrpc"] = "2.0";
    call_req["id"] = "2";
    call_req["method"] = "tools/call";
    call_req["params"] = nlohmann::json{{"name", "multi_progress"},
                                        {"arguments", {{"dummy", 0}}},
                                        {"_meta", {{"progressToken", "multi-tok"}}}};
    raw_transport->enqueue_message(call_req.dump());

    boost::asio::co_spawn(
        io_ctx_, [&]() -> mcp::Task<void> { co_await server.run(transport, io_ctx_.get_executor()); },
        boost::asio::detached);

    io_ctx_.run();

    ASSERT_EQ(responses.size(), 5);

    // Verify 3 progress notifications
    for (int i = 1; i <= 3; ++i) {
        auto& p = responses[i];
        EXPECT_EQ(p["method"], "notifications/progress");
        EXPECT_EQ(p["params"]["progressToken"], "multi-tok");
        EXPECT_DOUBLE_EQ(p["params"]["progress"].get<double>(), static_cast<double>(i));
        EXPECT_DOUBLE_EQ(p["params"]["total"].get<double>(), 3.0);
    }

    // Final tool result
    EXPECT_EQ(responses[4]["id"], "2");
    ASSERT_TRUE(responses[4].contains("result"));
}

// Each request's log and progress notifications reach the client in the order its handler sent
// them, and before its result, also while another request's handler writes at the same time.
TEST_F(ServerProgressTest, NotificationsOfEachRequestPrecedeItsResultInOrder) {
    auto transport = std::make_shared<ScriptedTransport>(io_ctx_.get_executor());
    auto* raw_transport = transport.get();

    mcp::ServerCapabilities caps;
    caps.tools = mcp::ServerCapabilities::ToolsCapability{};
    mcp::Server server(mcp::Implementation{"test-server", "1.0"}, std::move(caps));

    static constexpr int steps = 5;
    server.add_tool<NoopParams, NoopResult>(
        "chatty", "Logs and reports progress", nlohmann::json{{"type", "object"}},
        [](mcp::Context& ctx, NoopParams /*params*/) -> mcp::Task<NoopResult> {
            for (int step = 0; step < steps; ++step) {
                co_await ctx.log_info("step " + std::to_string(step));
                co_await ctx.report_progress(step, steps);
            }
            co_return NoopResult{true};
        });

    // initialize + two calls, each with 2 * steps notifications and a result.
    constexpr std::size_t expected_messages = 1 + 2 * (2 * steps + 1);
    std::vector<nlohmann::json> written;
    raw_transport->set_on_write([&written, raw_transport](std::string_view message) {
        written.push_back(nlohmann::json::parse(message));
        if (written.size() == expected_messages) {
            raw_transport->close();
        }
    });

    raw_transport->enqueue_message(make_initialize_request("1").dump());
    raw_transport->enqueue_message(make_initialized_notification().dump());
    for (const auto* id : {"a", "b"}) {
        nlohmann::json call;
        call["jsonrpc"] = "2.0";
        call["id"] = id;
        call["method"] = "tools/call";
        call["params"] = nlohmann::json{
            {"name", "chatty"}, {"arguments", {{"dummy", 0}}}, {"_meta", {{"progressToken", id}}}};
        raw_transport->enqueue_message(call.dump());
    }

    boost::asio::co_spawn(
        io_ctx_, [&]() -> mcp::Task<void> { co_await server.run(transport, io_ctx_.get_executor()); },
        boost::asio::detached);
    io_ctx_.run();

    ASSERT_EQ(written.size(), expected_messages);
    // Log messages carry no request id, so each request's progress and result are checked, and
    // the log messages overall.
    for (const std::string id : {"a", "b"}) {
        std::vector<double> progress;
        bool answered = false;
        for (const auto& message : written) {
            if (message.value("method", "") == "notifications/progress" &&
                message["params"]["progressToken"] == id) {
                EXPECT_FALSE(answered) << "progress after the result of " << id;
                progress.push_back(message["params"]["progress"].get<double>());
            } else if (message.contains("id") && message["id"] == id) {
                EXPECT_TRUE(message.contains("result")) << message;
                answered = true;
            }
        }
        EXPECT_TRUE(answered) << id;
        EXPECT_EQ(progress, (std::vector<double>{0, 1, 2, 3, 4})) << id;
    }
    std::size_t logs = 0;
    for (const auto& message : written) {
        logs += message.value("method", "") == "notifications/message" ? 1U : 0U;
    }
    EXPECT_EQ(logs, 2U * steps);
}

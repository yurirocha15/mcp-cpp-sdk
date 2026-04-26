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

struct SlowParams {
    int value = 0;
};

inline void to_json(nlohmann::json& j, const SlowParams& p) { j = nlohmann::json{{"value", p.value}}; }

inline void from_json(const nlohmann::json& j, SlowParams& p) { j.at("value").get_to(p.value); }

struct SlowResult {
    bool was_cancelled = false;
};

inline void to_json(nlohmann::json& j, const SlowResult& r) {
    j = nlohmann::json{{"was_cancelled", r.was_cancelled}};
}

inline void from_json(const nlohmann::json& j, SlowResult& r) {
    j.at("was_cancelled").get_to(r.was_cancelled);
}

}  // namespace

class ServerCancellationTest : public ::testing::Test {
   protected:
    boost::asio::io_context io_ctx_;
};

TEST_F(ServerCancellationTest, CancellationFlagSetOnNotification) {
    auto transport = std::make_shared<ScriptedTransport>(io_ctx_.get_executor());
    auto* raw_transport = transport.get();

    mcp::Implementation info;
    info.name = "test-server";
    info.version = "1.0";

    mcp::ServerCapabilities caps;
    mcp::ServerCapabilities::ToolsCapability tools_cap;
    caps.tools = std::move(tools_cap);

    mcp::Server server(std::move(info), std::move(caps));

    // Register a tool that checks cancellation after a yield point.
    server.add_tool<SlowParams, SlowResult>(
        "slow_tool", "A tool that checks cancellation", nlohmann::json{{"type", "object"}},
        [&](mcp::Context& ctx, SlowParams /*params*/) -> mcp::Task<SlowResult> {
            // Yield to allow the cancellation notification to be processed.
            auto timer =
                boost::asio::steady_timer(io_ctx_.get_executor(), std::chrono::milliseconds(50));
            co_await timer.async_wait(boost::asio::use_awaitable);

            co_return SlowResult{ctx.is_cancelled()};
        });

    std::vector<nlohmann::json> responses;
    raw_transport->set_on_write([&responses, raw_transport](std::string_view msg) {
        responses.push_back(nlohmann::json::parse(msg));
        // init response + tool result = 2
        if (responses.size() == 2) {
            raw_transport->close();
        }
    });

    raw_transport->enqueue_message(make_initialize_request("1").dump());

    nlohmann::json call_req;
    call_req["jsonrpc"] = "2.0";
    call_req["id"] = "2";
    call_req["method"] = "tools/call";
    call_req["params"] = nlohmann::json{{"name", "slow_tool"}, {"arguments", {{"value", 42}}}};
    raw_transport->enqueue_message(call_req.dump());

    // Send cancellation notification for request "2".
    nlohmann::json cancel_notif;
    cancel_notif["jsonrpc"] = "2.0";
    cancel_notif["method"] = "notifications/cancelled";
    cancel_notif["params"] = nlohmann::json{{"requestId", "2"}, {"reason", "user aborted"}};
    raw_transport->enqueue_message(cancel_notif.dump());

    boost::asio::co_spawn(
        io_ctx_, [&]() -> mcp::Task<void> { co_await server.run(transport, io_ctx_.get_executor()); },
        boost::asio::detached);

    io_ctx_.run();

    ASSERT_EQ(responses.size(), 2);
    // First response is init
    EXPECT_EQ(responses[0]["id"], "1");
    // Second is tool result — the handler should see cancellation
    EXPECT_EQ(responses[1]["id"], "2");
    ASSERT_TRUE(responses[1].contains("result"));
    EXPECT_TRUE(responses[1]["result"]["was_cancelled"].get<bool>());
}

TEST_F(ServerCancellationTest, NoCancellationWhenNotSent) {
    auto transport = std::make_shared<ScriptedTransport>(io_ctx_.get_executor());
    auto* raw_transport = transport.get();

    mcp::Implementation info;
    info.name = "test-server";
    info.version = "1.0";

    mcp::ServerCapabilities caps;
    mcp::ServerCapabilities::ToolsCapability tools_cap;
    caps.tools = std::move(tools_cap);

    mcp::Server server(std::move(info), std::move(caps));

    server.add_tool<SlowParams, SlowResult>("check_cancel", "Check cancellation without cancel",
                                            nlohmann::json{{"type", "object"}},
                                            [](mcp::Context& ctx, SlowParams /*params*/) -> SlowResult {
                                                return SlowResult{ctx.is_cancelled()};
                                            });

    std::vector<nlohmann::json> responses;
    raw_transport->set_on_write([&responses, raw_transport](std::string_view msg) {
        responses.push_back(nlohmann::json::parse(msg));
        if (responses.size() == 2) {
            raw_transport->close();
        }
    });

    raw_transport->enqueue_message(make_initialize_request("1").dump());

    nlohmann::json call_req;
    call_req["jsonrpc"] = "2.0";
    call_req["id"] = "2";
    call_req["method"] = "tools/call";
    call_req["params"] = nlohmann::json{{"name", "check_cancel"}, {"arguments", {{"value", 1}}}};
    raw_transport->enqueue_message(call_req.dump());

    boost::asio::co_spawn(
        io_ctx_, [&]() -> mcp::Task<void> { co_await server.run(transport, io_ctx_.get_executor()); },
        boost::asio::detached);

    io_ctx_.run();

    ASSERT_EQ(responses.size(), 2);
    EXPECT_EQ(responses[1]["id"], "2");
    ASSERT_TRUE(responses[1].contains("result"));
    EXPECT_FALSE(responses[1]["result"]["was_cancelled"].get<bool>());
}

TEST_F(ServerCancellationTest, CancellationForUnknownRequestIsIgnored) {
    auto transport = std::make_shared<ScriptedTransport>(io_ctx_.get_executor());
    auto* raw_transport = transport.get();

    mcp::Implementation info;
    info.name = "test-server";
    info.version = "1.0";

    mcp::Server server(std::move(info), mcp::ServerCapabilities{});

    nlohmann::json response;
    raw_transport->set_on_write([&response, raw_transport](std::string_view msg) {
        response = nlohmann::json::parse(msg);
        raw_transport->close();
    });

    // Send a cancellation for a request that doesn't exist — should be silently ignored.
    nlohmann::json cancel_notif;
    cancel_notif["jsonrpc"] = "2.0";
    cancel_notif["method"] = "notifications/cancelled";
    cancel_notif["params"] = nlohmann::json{{"requestId", "999"}};
    raw_transport->enqueue_message(cancel_notif.dump());

    raw_transport->enqueue_message(make_initialize_request("1").dump());

    boost::asio::co_spawn(
        io_ctx_, [&]() -> mcp::Task<void> { co_await server.run(transport, io_ctx_.get_executor()); },
        boost::asio::detached);

    io_ctx_.run();

    EXPECT_EQ(response["id"], "1");
    ASSERT_TRUE(response.contains("result"));
}

TEST_F(ServerCancellationTest, InFlightCleanedUpAfterToolCompletes) {
    auto transport = std::make_shared<ScriptedTransport>(io_ctx_.get_executor());
    auto* raw_transport = transport.get();

    mcp::Implementation info;
    info.name = "test-server";
    info.version = "1.0";

    mcp::ServerCapabilities caps;
    mcp::ServerCapabilities::ToolsCapability tools_cap;
    caps.tools = std::move(tools_cap);

    mcp::Server server(std::move(info), std::move(caps));

    server.add_tool<SlowParams, SlowResult>(
        "quick_tool", "Quick tool", nlohmann::json{{"type", "object"}},
        [](SlowParams /*params*/) -> SlowResult { return SlowResult{false}; });

    std::vector<nlohmann::json> responses;
    raw_transport->set_on_write([&responses, raw_transport](std::string_view msg) {
        responses.push_back(nlohmann::json::parse(msg));
        if (responses.size() == 2) {
            // After tool completes, send a cancellation for it — should be a no-op.
            nlohmann::json cancel_notif;
            cancel_notif["jsonrpc"] = "2.0";
            cancel_notif["method"] = "notifications/cancelled";
            cancel_notif["params"] = nlohmann::json{{"requestId", "2"}};
            raw_transport->enqueue_message(cancel_notif.dump());

            // Then send a ping to verify server is still healthy.
            nlohmann::json ping_req = {{"jsonrpc", "2.0"}, {"id", "3"}, {"method", "ping"}};
            raw_transport->enqueue_message(ping_req.dump());
        }
        if (responses.size() == 3) {
            raw_transport->close();
        }
    });

    raw_transport->enqueue_message(make_initialize_request("1").dump());

    nlohmann::json call_req;
    call_req["jsonrpc"] = "2.0";
    call_req["id"] = "2";
    call_req["method"] = "tools/call";
    call_req["params"] = nlohmann::json{{"name", "quick_tool"}, {"arguments", {{"value", 0}}}};
    raw_transport->enqueue_message(call_req.dump());

    boost::asio::co_spawn(
        io_ctx_, [&]() -> mcp::Task<void> { co_await server.run(transport, io_ctx_.get_executor()); },
        boost::asio::detached);

    io_ctx_.run();

    ASSERT_EQ(responses.size(), 3);
    // Tool result
    EXPECT_EQ(responses[1]["id"], "2");
    ASSERT_TRUE(responses[1].contains("result"));
    // Ping after cancellation of completed request — server still works
    EXPECT_EQ(responses[2]["id"], "3");
    EXPECT_EQ(responses[2]["result"], nlohmann::json::object());
}

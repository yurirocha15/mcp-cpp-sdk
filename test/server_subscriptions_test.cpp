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

class ServerSubscriptionsTest : public ::testing::Test {
   protected:
    boost::asio::io_context io_ctx_;
};

TEST_F(ServerSubscriptionsTest, SubscribeReturnsEmptyResult) {
    auto transport = std::make_shared<ScriptedTransport>(io_ctx_.get_executor());
    auto* raw_transport = transport.get();

    mcp::Implementation info;
    info.name = "test-server";
    info.version = "1.0";

    mcp::ServerCapabilities caps;
    mcp::ServerCapabilities::ResourcesCapability res_cap;
    res_cap.subscribe = true;
    caps.resources = std::move(res_cap);

    mcp::Server server(std::move(info), std::move(caps));

    std::vector<nlohmann::json> responses;
    raw_transport->set_on_write([&responses, raw_transport](std::string_view msg) {
        responses.push_back(nlohmann::json::parse(msg));
        if (responses.size() == 2) {
            raw_transport->close();
        }
    });

    raw_transport->enqueue_message(make_initialize_request("1").dump());

    nlohmann::json sub_req;
    sub_req["jsonrpc"] = "2.0";
    sub_req["id"] = "2";
    sub_req["method"] = "resources/subscribe";
    sub_req["params"] = nlohmann::json{{"uri", "file:///test.txt"}};
    raw_transport->enqueue_message(sub_req.dump());

    boost::asio::co_spawn(
        io_ctx_, [&]() -> mcp::Task<void> { co_await server.run(transport, io_ctx_.get_executor()); },
        boost::asio::detached);

    io_ctx_.run();

    ASSERT_EQ(responses.size(), 2);
    EXPECT_EQ(responses[1]["id"], "2");
    ASSERT_TRUE(responses[1].contains("result"));
    EXPECT_EQ(responses[1]["result"], nlohmann::json::object());
}

TEST_F(ServerSubscriptionsTest, UnsubscribeReturnsEmptyResult) {
    auto transport = std::make_shared<ScriptedTransport>(io_ctx_.get_executor());
    auto* raw_transport = transport.get();

    mcp::Implementation info;
    info.name = "test-server";
    info.version = "1.0";

    mcp::ServerCapabilities caps;
    mcp::ServerCapabilities::ResourcesCapability res_cap;
    res_cap.subscribe = true;
    caps.resources = std::move(res_cap);

    mcp::Server server(std::move(info), std::move(caps));

    std::vector<nlohmann::json> responses;
    raw_transport->set_on_write([&responses, raw_transport](std::string_view msg) {
        responses.push_back(nlohmann::json::parse(msg));
        // init + subscribe + unsubscribe = 3
        if (responses.size() == 3) {
            raw_transport->close();
        }
    });

    raw_transport->enqueue_message(make_initialize_request("1").dump());

    nlohmann::json sub_req;
    sub_req["jsonrpc"] = "2.0";
    sub_req["id"] = "2";
    sub_req["method"] = "resources/subscribe";
    sub_req["params"] = nlohmann::json{{"uri", "file:///test.txt"}};
    raw_transport->enqueue_message(sub_req.dump());

    nlohmann::json unsub_req;
    unsub_req["jsonrpc"] = "2.0";
    unsub_req["id"] = "3";
    unsub_req["method"] = "resources/unsubscribe";
    unsub_req["params"] = nlohmann::json{{"uri", "file:///test.txt"}};
    raw_transport->enqueue_message(unsub_req.dump());

    boost::asio::co_spawn(
        io_ctx_, [&]() -> mcp::Task<void> { co_await server.run(transport, io_ctx_.get_executor()); },
        boost::asio::detached);

    io_ctx_.run();

    ASSERT_EQ(responses.size(), 3);
    EXPECT_EQ(responses[1]["id"], "2");
    EXPECT_EQ(responses[1]["result"], nlohmann::json::object());
    EXPECT_EQ(responses[2]["id"], "3");
    EXPECT_EQ(responses[2]["result"], nlohmann::json::object());
}

TEST_F(ServerSubscriptionsTest, NotifyResourceUpdatedSendsToSubscribers) {
    auto transport = std::make_shared<ScriptedTransport>(io_ctx_.get_executor());
    auto* raw_transport = transport.get();

    mcp::Implementation info;
    info.name = "test-server";
    info.version = "1.0";

    mcp::ServerCapabilities caps;
    mcp::ServerCapabilities::ResourcesCapability res_cap;
    res_cap.subscribe = true;
    caps.resources = std::move(res_cap);
    mcp::ServerCapabilities::ToolsCapability tools_cap;
    caps.tools = std::move(tools_cap);

    mcp::Server server(std::move(info), std::move(caps));

    // Register a tool that triggers resource update notification
    struct TriggerParams {
        std::string uri;
    };
    struct TriggerResult {
        bool triggered = true;
    };

    server.add_tool<nlohmann::json, nlohmann::json>(
        "trigger_update", "Triggers a resource update", nlohmann::json{{"type", "object"}},
        [&server](mcp::Context& /*ctx*/, nlohmann::json params) -> mcp::Task<nlohmann::json> {
            auto uri = params.at("uri").get<std::string>();
            co_await server.notify_resource_updated(uri);
            co_return nlohmann::json{{"triggered", true}};
        });

    std::vector<nlohmann::json> responses;
    raw_transport->set_on_write([&responses, raw_transport](std::string_view msg) {
        responses.push_back(nlohmann::json::parse(msg));
        // init + subscribe result + resource updated notification + tool result = 4
        if (responses.size() == 4) {
            raw_transport->close();
        }
    });

    raw_transport->enqueue_message(make_initialize_request("1").dump());

    // Subscribe to a resource
    nlohmann::json sub_req;
    sub_req["jsonrpc"] = "2.0";
    sub_req["id"] = "2";
    sub_req["method"] = "resources/subscribe";
    sub_req["params"] = nlohmann::json{{"uri", "file:///watched.txt"}};
    raw_transport->enqueue_message(sub_req.dump());

    // Call tool that triggers notification
    nlohmann::json call_req;
    call_req["jsonrpc"] = "2.0";
    call_req["id"] = "3";
    call_req["method"] = "tools/call";
    call_req["params"] =
        nlohmann::json{{"name", "trigger_update"}, {"arguments", {{"uri", "file:///watched.txt"}}}};
    raw_transport->enqueue_message(call_req.dump());

    boost::asio::co_spawn(
        io_ctx_, [&]() -> mcp::Task<void> { co_await server.run(transport, io_ctx_.get_executor()); },
        boost::asio::detached);

    io_ctx_.run();

    ASSERT_EQ(responses.size(), 4);

    // Subscribe result
    EXPECT_EQ(responses[1]["id"], "2");
    EXPECT_EQ(responses[1]["result"], nlohmann::json::object());

    // Resource updated notification
    auto& notif = responses[2];
    EXPECT_EQ(notif["method"], "notifications/resources/updated");
    EXPECT_FALSE(notif.contains("id"));
    EXPECT_EQ(notif["params"]["uri"], "file:///watched.txt");

    // Tool result
    EXPECT_EQ(responses[3]["id"], "3");
    ASSERT_TRUE(responses[3].contains("result"));
}

TEST_F(ServerSubscriptionsTest, NotifyResourceUpdatedNotSentForUnsubscribedUri) {
    auto transport = std::make_shared<ScriptedTransport>(io_ctx_.get_executor());
    auto* raw_transport = transport.get();

    mcp::Implementation info;
    info.name = "test-server";
    info.version = "1.0";

    mcp::ServerCapabilities caps;
    mcp::ServerCapabilities::ToolsCapability tools_cap;
    caps.tools = std::move(tools_cap);

    mcp::Server server(std::move(info), std::move(caps));

    server.add_tool<nlohmann::json, nlohmann::json>(
        "trigger_update", "Triggers update for unsubscribed", nlohmann::json{{"type", "object"}},
        [&server](mcp::Context& /*ctx*/, nlohmann::json params) -> mcp::Task<nlohmann::json> {
            auto uri = params.at("uri").get<std::string>();
            co_await server.notify_resource_updated(uri);
            co_return nlohmann::json{{"triggered", true}};
        });

    std::vector<nlohmann::json> responses;
    raw_transport->set_on_write([&responses, raw_transport](std::string_view msg) {
        responses.push_back(nlohmann::json::parse(msg));
        // init + tool result = 2 (no notification because not subscribed)
        if (responses.size() == 2) {
            raw_transport->close();
        }
    });

    raw_transport->enqueue_message(make_initialize_request("1").dump());

    nlohmann::json call_req;
    call_req["jsonrpc"] = "2.0";
    call_req["id"] = "2";
    call_req["method"] = "tools/call";
    call_req["params"] = nlohmann::json{{"name", "trigger_update"},
                                        {"arguments", {{"uri", "file:///not-subscribed.txt"}}}};
    raw_transport->enqueue_message(call_req.dump());

    boost::asio::co_spawn(
        io_ctx_, [&]() -> mcp::Task<void> { co_await server.run(transport, io_ctx_.get_executor()); },
        boost::asio::detached);

    io_ctx_.run();

    ASSERT_EQ(responses.size(), 2);
    EXPECT_EQ(responses[0]["id"], "1");
    EXPECT_EQ(responses[1]["id"], "2");
    // No notification was sent
}

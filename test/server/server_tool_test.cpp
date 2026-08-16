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

class ServerToolTest : public ::testing::Test {
   protected:
    boost::asio::io_context io_ctx_;

    struct ServerSetup {
        std::shared_ptr<mcp::ITransport> transport;
        ScriptedTransport* raw_transport;
        mcp::Server server;

        ServerSetup(boost::asio::io_context& io_ctx, mcp::ServerCapabilities caps)
            : transport(std::make_shared<ScriptedTransport>(io_ctx.get_executor())),
              raw_transport(static_cast<ScriptedTransport*>(transport.get())),
              server(
                  [] {
                      mcp::Implementation info;
                      info.name = "test-server";
                      info.version = "1.0";
                      return info;
                  }(),
                  std::move(caps)) {}
    };

    std::vector<nlohmann::json> call_tool(ServerSetup& setup, std::string name) {
        std::vector<nlohmann::json> responses;
        setup.raw_transport->set_on_write([&responses, &setup](std::string_view message) {
            responses.push_back(nlohmann::json::parse(message));
            if (responses.size() == 2) {
                setup.raw_transport->close();
            }
        });
        setup.raw_transport->enqueue_message(make_initialize_request("1").dump());
        setup.raw_transport->enqueue_message(make_initialized_notification().dump());
        setup.raw_transport->enqueue_message(nlohmann::json{
            {"jsonrpc", "2.0"},
            {"id", "2"},
            {"method", "tools/call"},
            {"params", {{"name", std::move(name)}, {"arguments", nlohmann::json::object()}}}}
                                                 .dump());

        boost::asio::co_spawn(
            io_ctx_,
            [&]() -> mcp::Task<void> {
                co_await setup.server.run(setup.transport, io_ctx_.get_executor());
            },
            boost::asio::detached);
        io_ctx_.run();
        return responses;
    }
};

TEST_F(ServerToolTest, NonTemplateSyncHandlerReturnsResult) {
    mcp::ServerCapabilities caps;
    caps.tools = mcp::ServerCapabilities::ToolsCapability{};
    ServerSetup setup(io_ctx_, std::move(caps));

    setup.server.add_tool(
        "greet", "Greets a user",
        nlohmann::json{{"type", "object"}, {"properties", {{"name", {{"type", "string"}}}}}},
        [](const nlohmann::json& params) -> nlohmann::json {
            return {{"message", "Hello, " + params.at("name").get<std::string>() + "!"}};
        });

    std::vector<nlohmann::json> responses;
    setup.raw_transport->set_on_write([&responses, &setup](std::string_view msg) {
        responses.push_back(nlohmann::json::parse(msg));
        if (responses.size() == 2) {
            setup.raw_transport->close();
        }
    });

    setup.raw_transport->enqueue_message(make_initialize_request("1").dump());
    setup.raw_transport->enqueue_message(make_initialized_notification().dump());

    nlohmann::json call_req;
    call_req["jsonrpc"] = "2.0";
    call_req["id"] = "2";
    call_req["method"] = "tools/call";
    call_req["params"] = {{"name", "greet"}, {"arguments", {{"name", "Alice"}}}};
    setup.raw_transport->enqueue_message(call_req.dump());

    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            co_await setup.server.run(setup.transport, io_ctx_.get_executor());
        },
        boost::asio::detached);

    io_ctx_.run();

    ASSERT_EQ(responses.size(), 2);
    EXPECT_EQ(responses[1]["id"], "2");
    ASSERT_TRUE(responses[1].contains("result"));
    EXPECT_EQ(responses[1]["result"]["structuredContent"]["message"], "Hello, Alice!");
    EXPECT_EQ(responses[1]["result"]["content"][0]["type"], "text");
}

TEST_F(ServerToolTest, NonTemplateHandlerExceptionBecomesToolError) {
    mcp::ServerCapabilities caps;
    caps.tools = mcp::ServerCapabilities::ToolsCapability{};
    ServerSetup setup(io_ctx_, std::move(caps));

    setup.server.add_tool(
        "fail", "Always fails", nlohmann::json{{"type", "object"}},
        [](const nlohmann::json&) -> nlohmann::json { throw std::runtime_error("something broke"); });

    std::vector<nlohmann::json> responses;
    setup.raw_transport->set_on_write([&responses, &setup](std::string_view msg) {
        responses.push_back(nlohmann::json::parse(msg));
        if (responses.size() == 2) {
            setup.raw_transport->close();
        }
    });

    setup.raw_transport->enqueue_message(make_initialize_request("1").dump());
    setup.raw_transport->enqueue_message(make_initialized_notification().dump());

    nlohmann::json call_req;
    call_req["jsonrpc"] = "2.0";
    call_req["id"] = "2";
    call_req["method"] = "tools/call";
    call_req["params"] = {{"name", "fail"}, {"arguments", nlohmann::json::object()}};
    setup.raw_transport->enqueue_message(call_req.dump());

    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            co_await setup.server.run(setup.transport, io_ctx_.get_executor());
        },
        boost::asio::detached);

    io_ctx_.run();

    ASSERT_EQ(responses.size(), 2);
    EXPECT_EQ(responses[1]["id"], "2");
    ASSERT_TRUE(responses[1].contains("result"));
    EXPECT_TRUE(responses[1]["result"]["isError"].get<bool>());
    auto content_arr = responses[1]["result"]["content"];
    ASSERT_FALSE(content_arr.empty());
    auto err_text = content_arr[0]["text"].get<std::string>();
    EXPECT_NE(err_text.find("something broke"), std::string::npos);
}

TEST_F(ServerToolTest, NonTemplateToolAppearsInToolsList) {
    mcp::ServerCapabilities caps;
    caps.tools = mcp::ServerCapabilities::ToolsCapability{};
    ServerSetup setup(io_ctx_, std::move(caps));

    nlohmann::json schema = {{"type", "object"}, {"properties", {{"x", {{"type", "integer"}}}}}};
    setup.server.add_tool("echo", "Echoes input", schema,
                          [](const nlohmann::json& params) -> nlohmann::json { return params; });

    std::vector<nlohmann::json> responses;
    setup.raw_transport->set_on_write([&responses, &setup](std::string_view msg) {
        responses.push_back(nlohmann::json::parse(msg));
        if (responses.size() == 2) {
            setup.raw_transport->close();
        }
    });

    setup.raw_transport->enqueue_message(make_initialize_request("1").dump());
    setup.raw_transport->enqueue_message(make_initialized_notification().dump());

    nlohmann::json list_req;
    list_req["jsonrpc"] = "2.0";
    list_req["id"] = "2";
    list_req["method"] = "tools/list";
    list_req["params"] = nlohmann::json::object();
    setup.raw_transport->enqueue_message(list_req.dump());

    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            co_await setup.server.run(setup.transport, io_ctx_.get_executor());
        },
        boost::asio::detached);

    io_ctx_.run();

    ASSERT_EQ(responses.size(), 2);
    auto tools_arr = responses[1]["result"]["tools"];
    ASSERT_EQ(tools_arr.size(), 1);
    EXPECT_EQ(tools_arr[0]["name"], "echo");
    EXPECT_EQ(tools_arr[0]["description"], "Echoes input");
    EXPECT_EQ(tools_arr[0]["inputSchema"], schema);
}

TEST_F(ServerToolTest, ResultHelpersProduceProtocolValidShapes) {
    nlohmann::json text = mcp::make_tool_text_result("done");
    ASSERT_EQ(text["content"].size(), 1);
    EXPECT_EQ(text["content"][0], nlohmann::json({{"type", "text"}, {"text", "done"}}));

    nlohmann::json error = mcp::make_tool_error_result("failed");
    EXPECT_TRUE(error["isError"].get<bool>());
    EXPECT_EQ(error["content"][0]["text"], "failed");

    nlohmann::json structured = mcp::make_tool_structured_result({{"answer", 42}}, "forty-two");
    EXPECT_EQ(structured["structuredContent"]["answer"], 42);
    EXPECT_EQ(structured["content"][0]["text"], "forty-two");
    EXPECT_THROW(static_cast<void>(mcp::make_tool_structured_result(nlohmann::json::array())),
                 std::invalid_argument);
}

TEST_F(ServerToolTest, RawToolAcceptsCompleteCallToolResult) {
    mcp::ServerCapabilities caps;
    caps.tools = mcp::ServerCapabilities::ToolsCapability{};
    ServerSetup setup(io_ctx_, std::move(caps));

    mcp::Tool tool;
    tool.name = "raw";
    tool.inputSchema = {{"type", "object"}};
    setup.server.add_raw_tool(tool, [](const nlohmann::json&) -> nlohmann::json {
        return mcp::make_tool_text_result("raw result");
    });

    auto responses = call_tool(setup, "raw");

    ASSERT_EQ(responses.size(), 2);
    EXPECT_EQ(responses[1]["result"]["content"][0]["text"], "raw result");
    EXPECT_FALSE(responses[1]["result"].contains("structuredContent"));
}

TEST_F(ServerToolTest, RawToolRejectsIncompleteCallToolResult) {
    mcp::ServerCapabilities caps;
    caps.tools = mcp::ServerCapabilities::ToolsCapability{};
    ServerSetup setup(io_ctx_, std::move(caps));

    mcp::Tool tool;
    tool.name = "invalid-raw";
    tool.inputSchema = {{"type", "object"}};
    setup.server.add_raw_tool(tool, [](const nlohmann::json&) -> nlohmann::json {
        return {{"message", "not a CallToolResult"}};
    });

    auto responses = call_tool(setup, "invalid-raw");

    ASSERT_EQ(responses.size(), 2);
    EXPECT_EQ(responses[1]["error"]["code"], mcp::g_INTERNAL_ERROR);
    EXPECT_NE(responses[1]["error"]["message"].get<std::string>().find("CallToolResult"),
              std::string::npos);
}

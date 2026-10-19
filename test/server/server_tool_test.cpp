#include "../test_utils.hpp"

#include "mcp/protocol/schema.hpp"
#include "mcp/server/server.hpp"

#include <gtest/gtest.h>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <chrono>
#include <functional>
#include <limits>
#include <memory>
#include <stdexcept>
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

    std::vector<nlohmann::json> call_tool(ServerSetup& setup, std::string name,
                                          nlohmann::json arguments = nlohmann::json::object()) {
        return call_tool(setup, std::move(name), io_ctx_, std::move(arguments));
    }

    static std::vector<nlohmann::json> call_tool(ServerSetup& setup, std::string name,
                                                 boost::asio::io_context& io_ctx,
                                                 nlohmann::json arguments = nlohmann::json::object()) {
        std::vector<nlohmann::json> responses;
        setup.raw_transport->set_on_write([&responses, &setup](std::string_view message) {
            responses.push_back(nlohmann::json::parse(message));
            if (responses.size() == 2) {
                setup.raw_transport->close();
            }
        });
        setup.raw_transport->enqueue_message(make_initialize_request("1").dump());
        setup.raw_transport->enqueue_message(make_initialized_notification().dump());
        setup.raw_transport->enqueue_message(
            nlohmann::json{{"jsonrpc", "2.0"},
                           {"id", "2"},
                           {"method", "tools/call"},
                           {"params", {{"name", std::move(name)}, {"arguments", std::move(arguments)}}}}
                .dump());

        boost::asio::co_spawn(
            io_ctx,
            [&]() -> mcp::Task<void> {
                co_await setup.server.run(setup.transport, io_ctx.get_executor());
            },
            boost::asio::detached);
        io_ctx.run();
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

TEST_F(ServerToolTest, NonStdExceptionFromHandlerBecomesToolError) {
    mcp::ServerCapabilities caps;
    caps.tools = mcp::ServerCapabilities::ToolsCapability{};
    ServerSetup setup(io_ctx_, std::move(caps));

    setup.server.add_tool("fail", "Throws something that is not a std::exception",
                          nlohmann::json{{"type", "object"}},
                          [](const nlohmann::json&) -> nlohmann::json { throw 42; });

    std::vector<nlohmann::json> responses;
    boost::asio::steady_timer watchdog(io_ctx_);

    setup.raw_transport->set_on_write([&responses, &setup, &watchdog](std::string_view message) {
        responses.push_back(nlohmann::json::parse(message));
        if (responses.size() == 2) {
            watchdog.cancel();
            setup.raw_transport->close();
        }
    });

    setup.raw_transport->enqueue_message(make_initialize_request("1").dump());
    setup.raw_transport->enqueue_message(make_initialized_notification().dump());
    setup.raw_transport->enqueue_message(
        nlohmann::json{{"jsonrpc", "2.0"},
                       {"id", "2"},
                       {"method", "tools/call"},
                       {"params", {{"name", "fail"}, {"arguments", nlohmann::json::object()}}}}
            .dump());

    // A throw the tool guard does not catch escapes the dispatcher without writing anything, so
    // the request is never answered and nothing closes the transport. Bound the wait, or that
    // regression hangs the suite instead of failing it.
    watchdog.expires_after(std::chrono::seconds(5));
    watchdog.async_wait([&setup](const boost::system::error_code& error) {
        if (!error) {
            setup.raw_transport->close();
        }
    });

    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            co_await setup.server.run(setup.transport, io_ctx_.get_executor());
        },
        boost::asio::detached);

    io_ctx_.run();

    ASSERT_EQ(responses.size(), 2) << "the tools/call request went unanswered";
    EXPECT_EQ(responses[1]["id"], "2");
    ASSERT_TRUE(responses[1].contains("result")) << responses[1].dump();
    EXPECT_TRUE(responses[1]["result"]["isError"].get<bool>());
}

TEST_F(ServerToolTest, NonTemplateSyncHandlerReturningCallToolResultIsNotRewrapped) {
    mcp::ServerCapabilities caps;
    caps.tools = mcp::ServerCapabilities::ToolsCapability{};
    ServerSetup setup(io_ctx_, std::move(caps));

    setup.server.add_tool("greet", "Greets a user", nlohmann::json{{"type", "object"}},
                          [](const nlohmann::json&) -> nlohmann::json {
                              return nlohmann::json(mcp::make_tool_text_result("Hello, Alice!"));
                          });

    auto responses = call_tool(setup, "greet");

    ASSERT_EQ(responses.size(), 2);
    ASSERT_TRUE(responses[1].contains("result"));
    EXPECT_EQ(responses[1]["result"]["content"][0]["text"], "Hello, Alice!");
    EXPECT_FALSE(responses[1]["result"].contains("structuredContent"));
}

TEST_F(ServerToolTest, NonTemplateSyncHandlerReturningErrorResultKeepsItsErrorFlag) {
    mcp::ServerCapabilities caps;
    caps.tools = mcp::ServerCapabilities::ToolsCapability{};
    ServerSetup setup(io_ctx_, std::move(caps));

    setup.server.add_tool("deny", "Denies every call", nlohmann::json{{"type", "object"}},
                          [](const nlohmann::json&) -> nlohmann::json {
                              return nlohmann::json(mcp::make_tool_error_result("not permitted"));
                          });

    auto responses = call_tool(setup, "deny");

    ASSERT_EQ(responses.size(), 2);
    ASSERT_TRUE(responses[1].contains("result"));
    EXPECT_TRUE(responses[1]["result"]["isError"].get<bool>());
    EXPECT_EQ(responses[1]["result"]["content"][0]["text"], "not permitted");
}

TEST_F(ServerToolTest, NonTemplateSyncHandlerReturningLiteralResultJsonIsNotRewrapped) {
    mcp::ServerCapabilities caps;
    caps.tools = mcp::ServerCapabilities::ToolsCapability{};
    ServerSetup setup(io_ctx_, std::move(caps));

    setup.server.add_tool(
        "echo", "Echoes a message", nlohmann::json{{"type", "object"}},
        [](const nlohmann::json&) -> nlohmann::json {
            return nlohmann::json{{"content", nlohmann::json::array({nlohmann::json{
                                                  {"type", "text"}, {"text", "hello"}}})}};
        });

    auto responses = call_tool(setup, "echo");

    ASSERT_EQ(responses.size(), 2);
    ASSERT_TRUE(responses[1].contains("result"));
    ASSERT_EQ(responses[1]["result"]["content"].size(), 1);
    EXPECT_EQ(responses[1]["result"]["content"][0]["text"], "hello");
    EXPECT_FALSE(responses[1]["result"].contains("structuredContent"));
}

// Only an object made entirely of CallToolResult members is passed through. Domain data is wrapped
// whatever its "content" holds, so the shape of the response does not depend on the data.
TEST_F(ServerToolTest, DomainObjectNamedContentIsStillWrapped) {
    const std::vector<nlohmann::json> domain_values = {
        {{"content", nlohmann::json::array({"first", "second"})}},
        {{"content", nlohmann::json::array()}},
        {{"content", nlohmann::json::array()}, {"total", 0}, {"page", 1}},
        {{"content", nlohmann::json::array({nlohmann::json{{"type", "text"}, {"text", "row"}}})},
         {"total", 1}},
    };

    for (const auto& value : domain_values) {
        SCOPED_TRACE(value.dump());
        boost::asio::io_context io_ctx;
        mcp::ServerCapabilities caps;
        caps.tools = mcp::ServerCapabilities::ToolsCapability{};
        ServerSetup setup(io_ctx, std::move(caps));

        setup.server.add_tool("fetch", "Returns domain data", nlohmann::json{{"type", "object"}},
                              [value](const nlohmann::json&) -> nlohmann::json { return value; });

        auto responses = call_tool(setup, "fetch", io_ctx);

        ASSERT_EQ(responses.size(), 2);
        ASSERT_TRUE(responses[1].contains("result")) << responses[1].dump();
        EXPECT_EQ(responses[1]["result"]["structuredContent"], value);
        EXPECT_EQ(responses[1]["result"]["content"][0]["type"], "text");
    }
}

TEST_F(ServerToolTest, SyncHandlerExceptionTextIsSanitizedBeforeItReachesThePeer) {
    mcp::ServerCapabilities caps;
    caps.tools = mcp::ServerCapabilities::ToolsCapability{};
    ServerSetup setup(io_ctx_, std::move(caps));

    setup.server.add_tool("fail", "Always fails", nlohmann::json{{"type", "object"}},
                          [](const nlohmann::json&) -> nlohmann::json {
                              throw std::runtime_error("broke\nat /opt/internal/secret.cpp:12");
                          });

    auto responses = call_tool(setup, "fail");

    ASSERT_EQ(responses.size(), 2);
    ASSERT_TRUE(responses[1].contains("result"));
    EXPECT_TRUE(responses[1]["result"]["isError"].get<bool>());
    auto text = responses[1]["result"]["content"][0]["text"].get<std::string>();
    EXPECT_EQ(text.find('\n'), std::string::npos);
    EXPECT_NE(text.find("broke"), std::string::npos);
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

namespace {

mcp::Server make_tool_server() {
    mcp::Implementation info;
    info.name = "schema-server";
    info.version = "1.0";
    mcp::ServerCapabilities caps;
    caps.tools = mcp::ServerCapabilities::ToolsCapability{};
    return {info, caps};
}

mcp::Tool tool_with_input(const nlohmann::json& input_schema) {
    mcp::Tool tool;
    tool.name = "t";
    tool.inputSchema = input_schema;
    return tool;
}

nlohmann::json raw_result(const nlohmann::json& /*arguments*/) {
    return mcp::make_tool_text_result("ok");
}

/// The category of the SchemaError registering @p tool throws, after checking that the message
/// names the tool and @p field.
mcp::SchemaError::Category registration_error(const mcp::Tool& tool, std::string_view field) {
    auto server = make_tool_server();
    try {
        server.add_raw_tool(tool, raw_result);
    } catch (const mcp::SchemaError& error) {
        const std::string message = error.what();
        EXPECT_NE(message.find("Tool '" + tool.name + "'"), std::string::npos) << message;
        EXPECT_NE(message.find(field), std::string::npos) << message;
        return error.category();
    }
    ADD_FAILURE() << "tool registered: " << nlohmann::json(tool).dump();
    return mcp::SchemaError::Category::eInvalidSchema;
}

}  // namespace

TEST(ServerToolSchema, InvalidInputSchemaIsRefused) {
    EXPECT_EQ(registration_error(tool_with_input({{"type", "object"}, {"properties", nullptr}}),
                                 "inputSchema"),
              mcp::SchemaError::Category::eInvalidSchema);
    EXPECT_EQ(
        registration_error(tool_with_input({{"type", "object"}, {"required", "a"}}), "inputSchema"),
        mcp::SchemaError::Category::eInvalidSchema);
}

TEST(ServerToolSchema, InputSchemaMustBeAnObject) {
    for (const auto& schema :
         {nlohmann::json(), nlohmann::json(true), nlohmann::json::array(), nlohmann::json("object")}) {
        SCOPED_TRACE(schema.dump());
        EXPECT_EQ(registration_error(tool_with_input(schema), "inputSchema"),
                  mcp::SchemaError::Category::eInvalidSchema);
    }
}

TEST(ServerToolSchema, InputSchemaMustDeclareObjectType) {
    for (const auto& schema : {nlohmann::json::object(), nlohmann::json{{"type", "string"}},
                               nlohmann::json{{"type", {"object", "null"}}},
                               nlohmann::json{{"properties", {{"a", {{"type", "string"}}}}}}}) {
        SCOPED_TRACE(schema.dump());
        EXPECT_EQ(registration_error(tool_with_input(schema), "inputSchema"),
                  mcp::SchemaError::Category::eInvalidSchema);
    }
}

TEST(ServerToolSchema, UnsupportedDialectIsRefused) {
    EXPECT_EQ(registration_error(
                  tool_with_input({{"$schema", "https://example.com/my-dialect"}, {"type", "object"}}),
                  "inputSchema"),
              mcp::SchemaError::Category::eUnsupportedDialect);
}

TEST(ServerToolSchema, RemoteReferenceIsRefused) {
    EXPECT_EQ(registration_error(
                  tool_with_input(
                      {{"type", "object"},
                       {"properties", {{"a", {{"$ref", "https://example.com/schemas/a.json"}}}}}}),
                  "inputSchema"),
              mcp::SchemaError::Category::eUnresolvedRef);
}

TEST(ServerToolSchema, InvalidOutputSchemaIsRefused) {
    auto tool = tool_with_input({{"type", "object"}});
    tool.outputSchema = {{"type", "object"}, {"properties", {{"n", {{"minimum", "zero"}}}}}};
    EXPECT_EQ(registration_error(tool, "outputSchema"), mcp::SchemaError::Category::eInvalidSchema);
    tool.outputSchema = true;
    EXPECT_EQ(registration_error(tool, "outputSchema"), mcp::SchemaError::Category::eInvalidSchema);

    auto server = make_tool_server();
    EXPECT_THROW((server.add_tool<nlohmann::json, nlohmann::json>(
                     "typed", "typed tool", {{"type", "object"}}, {{"type", "objekt"}},
                     [](const nlohmann::json& args) { return args; })),
                 mcp::SchemaError);
}

TEST(ServerToolSchema, AcceptsAnyKeywordOf2020_12) {
    const auto schema = nlohmann::json::parse(R"({
        "type": "object",
        "$defs": {"id": {"type": "string", "pattern": "^[a-z]+-[0-9]+$"}},
        "properties": {
            "id": {"$ref": "#/$defs/id"},
            "mode": {"enum": ["a", "b"], "x-mcp-header": "Mode"},
            "pair": {"type": "array", "prefixItems": [{"type": "string"}, {"type": "integer"}]},
            "choice": {"oneOf": [{"type": "string"}, {"type": "integer", "minimum": 0}]}
        },
        "required": ["id"],
        "if": {"properties": {"mode": {"const": "a"}}},
        "then": {"required": ["pair"]},
        "dependentSchemas": {"pair": {"not": {"required": ["choice"]}}},
        "unevaluatedProperties": false
    })");
    auto tool = tool_with_input(schema);
    tool.outputSchema = nlohmann::json::parse(R"({"anyOf": [{"type": "object"}, {"type": "array"}]})");
    auto server = make_tool_server();
    EXPECT_NO_THROW(server.add_raw_tool(tool, raw_result));
}

TEST(ServerToolSchema, RefusedToolIsNotRegistered) {
    auto server = make_tool_server();
    EXPECT_THROW(server.add_raw_tool(tool_with_input(nlohmann::json()), raw_result), mcp::SchemaError);
    EXPECT_NO_THROW(server.add_raw_tool(tool_with_input({{"type", "object"}}), raw_result));
    EXPECT_THROW(server.add_raw_tool(tool_with_input({{"type", "object"}}), raw_result),
                 std::invalid_argument);
}

namespace {

const nlohmann::json& order_schema() {
    static const nlohmann::json schema = nlohmann::json::parse(R"({
        "type": "object",
        "properties": {
            "item": {"type": "string", "minLength": 1},
            "quantity": {"type": "integer", "minimum": 1}
        },
        "required": ["item", "quantity"],
        "additionalProperties": false
    })");
    return schema;
}

mcp::ServerCapabilities tool_capabilities() {
    mcp::ServerCapabilities caps;
    caps.tools = mcp::ServerCapabilities::ToolsCapability{};
    return caps;
}

}  // namespace

class ServerToolArgsTest : public ServerToolTest {
   protected:
    /// Registers "order" with order_schema(); the handler counts its runs and echoes the arguments.
    void add_order_tool(ServerSetup& setup) {
        setup.server.add_tool("order", "Place an order", order_schema(),
                              [this](const nlohmann::json& arguments) {
                                  ++handler_runs_;
                                  return nlohmann::json{{"ordered", arguments}};
                              });
    }

    int handler_runs_ = 0;
};

TEST_F(ServerToolArgsTest, NonConformingArgumentsGiveAnErrorResultWithoutRunningTheHandler) {
    ServerSetup setup(io_ctx_, tool_capabilities());
    add_order_tool(setup);

    auto responses = call_tool(setup, "order", {{"item", ""}, {"quantity", 0}, {"extra", true}});

    ASSERT_EQ(responses.size(), 2);
    const auto& result = responses[1].at("result");
    EXPECT_TRUE(result.value("isError", false));
    const auto text = result.at("content").at(0).at("text").get<std::string>();
    EXPECT_EQ(text.rfind("Invalid arguments: ", 0), 0U) << text;
    EXPECT_NE(text.find("/item"), std::string::npos) << text;
    EXPECT_NE(text.find("/quantity"), std::string::npos) << text;
    EXPECT_EQ(handler_runs_, 0);
    EXPECT_EQ(setup.server.tool_validation_stats().argument_rejections, 1U);
}

TEST_F(ServerToolArgsTest, MissingRequiredArgumentIsReported) {
    ServerSetup setup(io_ctx_, tool_capabilities());
    add_order_tool(setup);

    auto responses = call_tool(setup, "order", {{"item", "tea"}});

    ASSERT_EQ(responses.size(), 2);
    const auto& result = responses[1].at("result");
    EXPECT_TRUE(result.value("isError", false));
    EXPECT_NE(result.at("content").at(0).at("text").get<std::string>().find("quantity"),
              std::string::npos);
    EXPECT_EQ(handler_runs_, 0);
}

// Arguments that are not an object are refused while decoding the request, before validation.
TEST_F(ServerToolArgsTest, ArgumentsThatAreNotAnObjectAreInvalidParams) {
    ServerSetup setup(io_ctx_, tool_capabilities());
    add_order_tool(setup);

    auto responses = call_tool(setup, "order", nlohmann::json::array({"tea", 2}));

    ASSERT_EQ(responses.size(), 2);
    EXPECT_EQ(responses[1].at("error").at("code"), mcp::g_INVALID_PARAMS);
    EXPECT_EQ(handler_runs_, 0);
    EXPECT_EQ(setup.server.tool_validation_stats().argument_rejections, 0U);
}

TEST_F(ServerToolArgsTest, ConformingArgumentsReachTheHandler) {
    ServerSetup setup(io_ctx_, tool_capabilities());
    add_order_tool(setup);

    const nlohmann::json arguments = {{"item", "tea"}, {"quantity", 2}};
    auto responses = call_tool(setup, "order", arguments);

    ASSERT_EQ(responses.size(), 2);
    const auto& result = responses[1].at("result");
    EXPECT_FALSE(result.value("isError", false));
    EXPECT_EQ(handler_runs_, 1);
    EXPECT_EQ(setup.server.tool_validation_stats().argument_rejections, 0U);
}

// Values a lenient implementation might mangle on the way to the validator: integral doubles,
// integers beyond int64, non-ASCII text, nesting within the depth limit, many elements.
TEST_F(ServerToolArgsTest, ValidCorpusIsNeverRejected) {
    ServerSetup setup(io_ctx_, tool_capabilities());
    const auto schema = nlohmann::json::parse(R"({
        "type": "object",
        "properties": {
            "count": {"type": "integer"},
            "big": {"type": "integer", "minimum": 0},
            "ratio": {"type": "number", "exclusiveMaximum": 1},
            "name": {"type": "string", "maxLength": 5, "pattern": "^[^0-9]+$"},
            "tree": {"$ref": "#/$defs/node"},
            "values": {"type": "array", "items": {"type": "number"}, "maxItems": 10000}
        },
        "$defs": {"node": {"type": "object", "properties": {"child": {"$ref": "#/$defs/node"}}}}
    })");
    setup.server.add_tool("corpus", "Accepts the corpus", schema, [this](const nlohmann::json&) {
        ++handler_runs_;
        return nlohmann::json{{"ok", true}};
    });
    nlohmann::json tree = nlohmann::json::object();
    for (int i = 0; i < 30; ++i) {
        tree = {{"child", std::move(tree)}};
    }
    nlohmann::json values = nlohmann::json::array();
    for (int i = 0; i < 10000; ++i) {
        values.push_back(i % 2 == 0 ? nlohmann::json(i) : nlohmann::json(i + 0.25));
    }
    const nlohmann::json arguments = {
        {"count", 3.0},
        {"big", std::numeric_limits<std::uint64_t>::max()},
        {"ratio", 0.999999},
        {"name", "\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9"},  // five code points, ten bytes
        {"tree", std::move(tree)},
        {"values", std::move(values)},
    };

    auto responses = call_tool(setup, "corpus", arguments);

    ASSERT_EQ(responses.size(), 2);
    const auto& result = responses[1].at("result");
    EXPECT_FALSE(result.value("isError", false)) << result.dump().substr(0, 400);
    EXPECT_EQ(handler_runs_, 1);
}

TEST_F(ServerToolArgsTest, ValidationCanBeTurnedOff) {
    ServerSetup setup(io_ctx_, tool_capabilities());
    setup.server.set_tool_validation({.validate_arguments = false});
    add_order_tool(setup);

    auto responses = call_tool(setup, "order", {{"quantity", "many"}});

    ASSERT_EQ(responses.size(), 2);
    EXPECT_FALSE(responses[1].at("result").value("isError", false));
    EXPECT_EQ(handler_runs_, 1);
    EXPECT_EQ(setup.server.tool_validation_stats().argument_rejections, 0U);
}

// Middleware runs first and sees the call; the check stands in for the handler.
TEST_F(ServerToolArgsTest, MiddlewareSeesARefusedCall) {
    ServerSetup setup(io_ctx_, tool_capabilities());
    add_order_tool(setup);
    int middleware_runs = 0;
    bool saw_error = false;
    setup.server.use([&](mcp::Context& ctx, const nlohmann::json& params,
                         mcp::TypeErasedHandler next) -> mcp::Task<nlohmann::json> {
        ++middleware_runs;
        auto result = co_await next(ctx, params);
        saw_error = result.value("isError", false);
        co_return result;
    });

    auto responses = call_tool(setup, "order", {{"item", 7}});

    ASSERT_EQ(responses.size(), 2);
    EXPECT_TRUE(responses[1].at("result").value("isError", false));
    EXPECT_EQ(middleware_runs, 1);
    EXPECT_TRUE(saw_error);
    EXPECT_EQ(handler_runs_, 0);
}

#include "../test_utils.hpp"

#include "mcp/client/client.hpp"

#include <gtest/gtest.h>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

// Returns just the result object (not a full JSON-RPC response) for use with make_result_response.
nlohmann::json make_initialize_result() {
    return {{"protocolVersion", mcp::g_LATEST_PROTOCOL_VERSION},
            {"capabilities", nlohmann::json::object()},
            {"serverInfo", {{"name", "test-server"}, {"version", "1.0"}}}};
}

}  // namespace

class ClientFeaturesTest : public ::testing::Test {
   protected:
    boost::asio::io_context io_ctx_;

    struct ConnectedClient {
        mcp::Client client;
        ScriptedTransport* raw;
    };

    ConnectedClient make_client_with_auto_init() {
        auto transport = std::make_shared<ScriptedTransport>(io_ctx_.get_executor());
        auto* raw = transport.get();

        raw->set_on_write([raw](std::string_view msg) {
            auto json_msg = nlohmann::json::parse(msg);
            if (json_msg.contains("id") && json_msg.value("method", "") == "initialize") {
                auto id = json_msg["id"].get<std::string>();
                raw->enqueue_message(make_result_response(id, make_initialize_result()).dump());
            }
        });

        return {mcp::Client(transport, io_ctx_.get_executor()), raw};
    }
};

TEST_F(ClientFeaturesTest, CallToolSendsCorrectWireFormat) {
    auto [client, raw] = make_client_with_auto_init();

    nlohmann::json captured_params;
    std::string captured_method;

    raw->set_on_write([raw, &captured_params, &captured_method](std::string_view msg) {
        auto json_msg = nlohmann::json::parse(msg);
        if (!json_msg.contains("id")) {
            return;
        }
        auto id = json_msg["id"].get<std::string>();
        auto method = json_msg.value("method", "");
        if (method == "initialize") {
            raw->enqueue_message(make_result_response(id, make_initialize_result()).dump());
        } else if (method == "tools/call") {
            captured_method = method;
            captured_params = json_msg["params"];
            nlohmann::json result = {{"content", {{{"type", "text"}, {"text", "hello world"}}}},
                                     {"isError", false}};
            raw->enqueue_message(make_result_response(id, std::move(result)).dump());
        }
    });

    mcp::CallToolResult result;
    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            mcp::Implementation info;
            info.name = "test-client";
            info.version = "0.1";
            co_await client.connect(std::move(info), mcp::ClientCapabilities{});

            nlohmann::json args = {{"input", "test-value"}};
            result = co_await client.call_tool("my_tool", std::move(args));
            raw->close();
        },
        boost::asio::detached);

    io_ctx_.run();

    EXPECT_EQ(captured_method, "tools/call");
    EXPECT_EQ(captured_params["name"], "my_tool");
    EXPECT_EQ(captured_params["arguments"]["input"], "test-value");
    ASSERT_EQ(result.content.size(), 1);
    auto& text_content = std::get<mcp::TextContent>(result.content[0]);
    EXPECT_EQ(text_content.text, "hello world");
    ASSERT_TRUE(result.isError.has_value());
    EXPECT_FALSE(*result.isError);
}

TEST_F(ClientFeaturesTest, ListToolsSendsCorrectWireFormat) {
    auto [client, raw] = make_client_with_auto_init();

    nlohmann::json captured_request;

    raw->set_on_write([raw, &captured_request](std::string_view msg) {
        auto json_msg = nlohmann::json::parse(msg);
        if (!json_msg.contains("id")) {
            return;
        }
        auto id = json_msg["id"].get<std::string>();
        auto method = json_msg.value("method", "");
        if (method == "initialize") {
            raw->enqueue_message(make_result_response(id, make_initialize_result()).dump());
        } else if (method == "tools/list") {
            captured_request = json_msg;
            nlohmann::json result = {{"tools",
                                      {{{"name", "echo"},
                                        {"description", "Echoes input"},
                                        {"inputSchema", {{"type", "object"}}}}}}};
            raw->enqueue_message(make_result_response(id, std::move(result)).dump());
        }
    });

    mcp::ListToolsResult result;
    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            mcp::Implementation info;
            info.name = "test-client";
            info.version = "0.1";
            co_await client.connect(std::move(info), mcp::ClientCapabilities{});

            result = co_await client.list_tools();
            raw->close();
        },
        boost::asio::detached);

    io_ctx_.run();

    EXPECT_EQ(captured_request["method"], "tools/list");
    EXPECT_FALSE(captured_request.contains("params"));
    ASSERT_EQ(result.tools.size(), 1);
    EXPECT_EQ(result.tools[0].name, "echo");
    ASSERT_TRUE(result.tools[0].description.has_value());
    EXPECT_EQ(*result.tools[0].description, "Echoes input");
}

TEST_F(ClientFeaturesTest, ListToolsWithCursorSendsParams) {
    auto [client, raw] = make_client_with_auto_init();

    nlohmann::json captured_params;

    raw->set_on_write([raw, &captured_params](std::string_view msg) {
        auto json_msg = nlohmann::json::parse(msg);
        if (!json_msg.contains("id")) {
            return;
        }
        auto id = json_msg["id"].get<std::string>();
        auto method = json_msg.value("method", "");
        if (method == "initialize") {
            raw->enqueue_message(make_result_response(id, make_initialize_result()).dump());
        } else if (method == "tools/list") {
            captured_params = json_msg.value("params", nlohmann::json::object());
            nlohmann::json result = {{"tools", nlohmann::json::array()}, {"nextCursor", "page3"}};
            raw->enqueue_message(make_result_response(id, std::move(result)).dump());
        }
    });

    mcp::ListToolsResult result;
    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            mcp::Implementation info;
            info.name = "test-client";
            info.version = "0.1";
            co_await client.connect(std::move(info), mcp::ClientCapabilities{});

            result = co_await client.list_tools("page2");
            raw->close();
        },
        boost::asio::detached);

    io_ctx_.run();

    EXPECT_EQ(captured_params["cursor"], "page2");
    ASSERT_TRUE(result.nextCursor.has_value());
    EXPECT_EQ(*result.nextCursor, "page3");
}

TEST_F(ClientFeaturesTest, ListResourcesSendsCorrectWireFormat) {
    auto [client, raw] = make_client_with_auto_init();

    std::string captured_method;

    raw->set_on_write([raw, &captured_method](std::string_view msg) {
        auto json_msg = nlohmann::json::parse(msg);
        if (!json_msg.contains("id")) {
            return;
        }
        auto id = json_msg["id"].get<std::string>();
        auto method = json_msg.value("method", "");
        if (method == "initialize") {
            raw->enqueue_message(make_result_response(id, make_initialize_result()).dump());
        } else if (method == "resources/list") {
            captured_method = method;
            nlohmann::json result = {{"resources", {{{"uri", "file:///a.txt"}, {"name", "a.txt"}}}}};
            raw->enqueue_message(make_result_response(id, std::move(result)).dump());
        }
    });

    mcp::ListResourcesResult result;
    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            mcp::Implementation info;
            info.name = "test-client";
            info.version = "0.1";
            co_await client.connect(std::move(info), mcp::ClientCapabilities{});

            result = co_await client.list_resources();
            raw->close();
        },
        boost::asio::detached);

    io_ctx_.run();

    EXPECT_EQ(captured_method, "resources/list");
    ASSERT_EQ(result.resources.size(), 1);
    EXPECT_EQ(result.resources[0].uri, "file:///a.txt");
    EXPECT_EQ(result.resources[0].name, "a.txt");
}

TEST_F(ClientFeaturesTest, ReadResourceSendsCorrectWireFormat) {
    auto [client, raw] = make_client_with_auto_init();

    nlohmann::json captured_params;

    raw->set_on_write([raw, &captured_params](std::string_view msg) {
        auto json_msg = nlohmann::json::parse(msg);
        if (!json_msg.contains("id")) {
            return;
        }
        auto id = json_msg["id"].get<std::string>();
        auto method = json_msg.value("method", "");
        if (method == "initialize") {
            raw->enqueue_message(make_result_response(id, make_initialize_result()).dump());
        } else if (method == "resources/read") {
            captured_params = json_msg["params"];
            nlohmann::json result = {
                {"contents", {{{"uri", "file:///b.txt"}, {"text", "file content"}}}}};
            raw->enqueue_message(make_result_response(id, std::move(result)).dump());
        }
    });

    mcp::ReadResourceResult result;
    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            mcp::Implementation info;
            info.name = "test-client";
            info.version = "0.1";
            co_await client.connect(std::move(info), mcp::ClientCapabilities{});

            result = co_await client.read_resource("file:///b.txt");
            raw->close();
        },
        boost::asio::detached);

    io_ctx_.run();

    EXPECT_EQ(captured_params["uri"], "file:///b.txt");
    ASSERT_EQ(result.contents.size(), 1);
    auto& text_contents = std::get<mcp::TextResourceContents>(result.contents[0]);
    EXPECT_EQ(text_contents.uri, "file:///b.txt");
    EXPECT_EQ(text_contents.text, "file content");
}

TEST_F(ClientFeaturesTest, ListResourceTemplatesSendsCorrectWireFormat) {
    auto [client, raw] = make_client_with_auto_init();

    std::string captured_method;

    raw->set_on_write([raw, &captured_method](std::string_view msg) {
        auto json_msg = nlohmann::json::parse(msg);
        if (!json_msg.contains("id")) {
            return;
        }
        auto id = json_msg["id"].get<std::string>();
        auto method = json_msg.value("method", "");
        if (method == "initialize") {
            raw->enqueue_message(make_result_response(id, make_initialize_result()).dump());
        } else if (method == "resources/templates/list") {
            captured_method = method;
            nlohmann::json result = {
                {"resourceTemplates", {{{"uriTemplate", "file:///{path}"}, {"name", "File"}}}}};
            raw->enqueue_message(make_result_response(id, std::move(result)).dump());
        }
    });

    mcp::ListResourceTemplatesResult result;
    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            mcp::Implementation info;
            info.name = "test-client";
            info.version = "0.1";
            co_await client.connect(std::move(info), mcp::ClientCapabilities{});

            result = co_await client.list_resource_templates();
            raw->close();
        },
        boost::asio::detached);

    io_ctx_.run();

    EXPECT_EQ(captured_method, "resources/templates/list");
    ASSERT_EQ(result.resourceTemplates.size(), 1);
    EXPECT_EQ(result.resourceTemplates[0].uriTemplate, "file:///{path}");
    EXPECT_EQ(result.resourceTemplates[0].name, "File");
}

TEST_F(ClientFeaturesTest, ListPromptsSendsCorrectWireFormat) {
    auto [client, raw] = make_client_with_auto_init();

    std::string captured_method;

    raw->set_on_write([raw, &captured_method](std::string_view msg) {
        auto json_msg = nlohmann::json::parse(msg);
        if (!json_msg.contains("id")) {
            return;
        }
        auto id = json_msg["id"].get<std::string>();
        auto method = json_msg.value("method", "");
        if (method == "initialize") {
            raw->enqueue_message(make_result_response(id, make_initialize_result()).dump());
        } else if (method == "prompts/list") {
            captured_method = method;
            nlohmann::json result = {{"prompts", {{{"name", "greeting"}}}}};
            raw->enqueue_message(make_result_response(id, std::move(result)).dump());
        }
    });

    mcp::ListPromptsResult result;
    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            mcp::Implementation info;
            info.name = "test-client";
            info.version = "0.1";
            co_await client.connect(std::move(info), mcp::ClientCapabilities{});

            result = co_await client.list_prompts();
            raw->close();
        },
        boost::asio::detached);

    io_ctx_.run();

    EXPECT_EQ(captured_method, "prompts/list");
    ASSERT_EQ(result.prompts.size(), 1);
    EXPECT_EQ(result.prompts[0].name, "greeting");
}

TEST_F(ClientFeaturesTest, GetPromptSendsCorrectWireFormat) {
    auto [client, raw] = make_client_with_auto_init();

    nlohmann::json captured_params;

    raw->set_on_write([raw, &captured_params](std::string_view msg) {
        auto json_msg = nlohmann::json::parse(msg);
        if (!json_msg.contains("id")) {
            return;
        }
        auto id = json_msg["id"].get<std::string>();
        auto method = json_msg.value("method", "");
        if (method == "initialize") {
            raw->enqueue_message(make_result_response(id, make_initialize_result()).dump());
        } else if (method == "prompts/get") {
            captured_params = json_msg["params"];
            nlohmann::json result = {
                {"messages",
                 {{{"role", "user"}, {"content", {{"type", "text"}, {"text", "Hello Alice"}}}}}}};
            raw->enqueue_message(make_result_response(id, std::move(result)).dump());
        }
    });

    mcp::GetPromptResult result;
    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            mcp::Implementation info;
            info.name = "test-client";
            info.version = "0.1";
            co_await client.connect(std::move(info), mcp::ClientCapabilities{});

            std::map<std::string, std::string> args = {{"name", "Alice"}};
            result = co_await client.get_prompt("greeting", std::move(args));
            raw->close();
        },
        boost::asio::detached);

    io_ctx_.run();

    EXPECT_EQ(captured_params["name"], "greeting");
    EXPECT_EQ(captured_params["arguments"]["name"], "Alice");
    ASSERT_EQ(result.messages.size(), 1);
    EXPECT_EQ(result.messages[0].role, mcp::Role::eUser);
    auto& text_content = std::get<mcp::TextContent>(result.messages[0].content);
    EXPECT_EQ(text_content.text, "Hello Alice");
}

TEST_F(ClientFeaturesTest, GetPromptWithoutArgumentsSendsCorrectWireFormat) {
    auto [client, raw] = make_client_with_auto_init();

    nlohmann::json captured_params;

    raw->set_on_write([raw, &captured_params](std::string_view msg) {
        auto json_msg = nlohmann::json::parse(msg);
        if (!json_msg.contains("id")) {
            return;
        }
        auto id = json_msg["id"].get<std::string>();
        auto method = json_msg.value("method", "");
        if (method == "initialize") {
            raw->enqueue_message(make_result_response(id, make_initialize_result()).dump());
        } else if (method == "prompts/get") {
            captured_params = json_msg["params"];
            nlohmann::json result = {
                {"messages",
                 {{{"role", "assistant"}, {"content", {{"type", "text"}, {"text", "Hi there"}}}}}}};
            raw->enqueue_message(make_result_response(id, std::move(result)).dump());
        }
    });

    mcp::GetPromptResult result;
    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            mcp::Implementation info;
            info.name = "test-client";
            info.version = "0.1";
            co_await client.connect(std::move(info), mcp::ClientCapabilities{});

            result = co_await client.get_prompt("simple_prompt");
            raw->close();
        },
        boost::asio::detached);

    io_ctx_.run();

    EXPECT_EQ(captured_params["name"], "simple_prompt");
    EXPECT_FALSE(captured_params.contains("arguments"));
    ASSERT_EQ(result.messages.size(), 1);
    EXPECT_EQ(result.messages[0].role, mcp::Role::eAssistant);
}

TEST_F(ClientFeaturesTest, CompleteSendsCorrectWireFormat) {
    auto [client, raw] = make_client_with_auto_init();

    nlohmann::json captured_params;

    raw->set_on_write([raw, &captured_params](std::string_view msg) {
        auto json_msg = nlohmann::json::parse(msg);
        if (!json_msg.contains("id")) {
            return;
        }
        auto id = json_msg["id"].get<std::string>();
        auto method = json_msg.value("method", "");
        if (method == "initialize") {
            raw->enqueue_message(make_result_response(id, make_initialize_result()).dump());
        } else if (method == "completion/complete") {
            captured_params = json_msg["params"];
            nlohmann::json result = {
                {"completion", {{"values", {"alpha", "beta"}}, {"hasMore", true}}}};
            raw->enqueue_message(make_result_response(id, std::move(result)).dump());
        }
    });

    mcp::CompleteResult result;
    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            mcp::Implementation info;
            info.name = "test-client";
            info.version = "0.1";
            co_await client.connect(std::move(info), mcp::ClientCapabilities{});

            mcp::CompleteParams complete_params;
            mcp::PromptReference ref;
            ref.name = "greeting";
            complete_params.ref = std::move(ref);
            complete_params.argument.name = "name";
            complete_params.argument.value = "Al";

            result = co_await client.complete(std::move(complete_params));
            raw->close();
        },
        boost::asio::detached);

    io_ctx_.run();

    EXPECT_EQ(captured_params["ref"]["type"], "ref/prompt");
    EXPECT_EQ(captured_params["ref"]["name"], "greeting");
    EXPECT_EQ(captured_params["argument"]["name"], "name");
    EXPECT_EQ(captured_params["argument"]["value"], "Al");
    ASSERT_EQ(result.completion.values.size(), 2);
    EXPECT_EQ(result.completion.values[0], "alpha");
    EXPECT_EQ(result.completion.values[1], "beta");
    ASSERT_TRUE(result.completion.hasMore.has_value());
    EXPECT_TRUE(*result.completion.hasMore);
}

class ClientToolSchemaTest : public ::testing::Test {
   protected:
    boost::asio::io_context io_ctx_;
    std::shared_ptr<ScriptedTransport> transport_ =
        std::make_shared<ScriptedTransport>(io_ctx_.get_executor());
    nlohmann::json tools_ = nlohmann::json::array();  ///< What tools/list answers.
    nlohmann::json call_result_;                      ///< What tools/call answers.

    void SetUp() override {
        auto* raw = transport_.get();
        raw->set_on_write([this, raw](std::string_view msg) {
            auto json_msg = nlohmann::json::parse(msg);
            if (!json_msg.contains("id")) {
                return;
            }
            const auto id = json_msg["id"].get<std::string>();
            const auto method = json_msg.value("method", "");
            if (method == "initialize") {
                raw->enqueue_message(make_result_response(id, make_initialize_result()).dump());
            } else if (method == "tools/list") {
                raw->enqueue_message(make_result_response(id, {{"tools", tools_}}).dump());
            } else if (method == "tools/call") {
                raw->enqueue_message(make_result_response(id, call_result_).dump());
            }
        });
    }

    /// Connects a client with @p options, runs @p body with it and closes the transport.
    void run(mcp::ClientOptions options, std::function<mcp::Task<void>(mcp::Client&)> body) {
        mcp::Client client(transport_, io_ctx_.get_executor(), std::move(options));
        boost::asio::co_spawn(
            io_ctx_,
            [&]() -> mcp::Task<void> {
                // An exception escaping here would leave the transport open and io_ctx_.run()
                // waiting, so it is recorded and the transport closed either way.
                try {
                    co_await client.connect("test-client", "0.1");
                    co_await body(client);
                } catch (const std::exception& error) {
                    unexpected_error_ = error.what();
                }
                transport_->close();
            },
            boost::asio::detached);
        io_ctx_.run();
        EXPECT_EQ(unexpected_error_, "");
    }

    std::string unexpected_error_;

    static nlohmann::json tool(std::string name, nlohmann::json input_schema,
                               std::optional<nlohmann::json> output_schema = std::nullopt) {
        nlohmann::json definition = {{"name", std::move(name)},
                                     {"inputSchema", std::move(input_schema)}};
        if (output_schema) {
            definition["outputSchema"] = std::move(*output_schema);
        }
        return definition;
    }

    static nlohmann::json size_schema() {
        return nlohmann::json::parse(R"({
            "type": "object",
            "properties": {"size": {"type": "integer", "minimum": 0}},
            "required": ["size"]
        })");
    }

    static nlohmann::json structured_result(nlohmann::json structured, bool error = false) {
        return {{"content", nlohmann::json::array()},
                {"structuredContent", std::move(structured)},
                {"isError", error}};
    }
};

TEST_F(ClientToolSchemaTest, ToolsWithUnusableSchemasAreLeftOutAndReported) {
    tools_ = nlohmann::json::array({
        tool("usable", {{"type", "object"}}, size_schema()),
        tool("invalid", {{"type", "object"}, {"properties", nullptr}}),
        tool("dialect", {{"$schema", "https://example.com/dialect"}, {"type", "object"}}),
        tool("remote",
             {{"type", "object"}, {"properties", {{"a", {{"$ref", "https://example.com/a.json"}}}}}}),
        tool("bad-output", {{"type", "object"}}, nlohmann::json{{"type", "objekt"}}),
        tool("no-object-type", nlohmann::json::object()),
    });
    std::vector<std::pair<std::string, mcp::SchemaError::Category>> reported;
    std::vector<std::string> messages;
    mcp::ClientOptions options;
    options.on_tool_schema_error = [&](const mcp::Tool& tool, const mcp::SchemaError& error) {
        reported.emplace_back(tool.name, error.category());
        messages.emplace_back(error.what());
    };
    std::vector<std::string> listed;
    run(std::move(options), [&](mcp::Client& client) -> mcp::Task<void> {
        auto result = co_await client.list_tools();
        for (const auto& listed_tool : result.tools) {
            listed.push_back(listed_tool.name);
        }
    });

    EXPECT_EQ(listed, (std::vector<std::string>{"usable", "no-object-type"}));
    const std::vector<std::pair<std::string, mcp::SchemaError::Category>> expected = {
        {"invalid", mcp::SchemaError::Category::eInvalidSchema},
        {"dialect", mcp::SchemaError::Category::eUnsupportedDialect},
        {"remote", mcp::SchemaError::Category::eUnresolvedRef},
        {"bad-output", mcp::SchemaError::Category::eInvalidSchema},
    };
    EXPECT_EQ(reported, expected);
    ASSERT_EQ(messages.size(), 4U);
    EXPECT_EQ(messages[0].rfind("inputSchema: ", 0), 0U) << messages[0];
    EXPECT_EQ(messages[3].rfind("outputSchema: ", 0), 0U) << messages[3];
}

TEST_F(ClientToolSchemaTest, NonConformingStructuredContentThrows) {
    tools_ = nlohmann::json::array({tool("measure", {{"type", "object"}}, size_schema())});
    call_result_ = structured_result({{"size", -1}});
    std::optional<mcp::StructuredContentError> thrown;
    run({}, [&](mcp::Client& client) -> mcp::Task<void> {
        co_await client.list_tools();
        try {
            co_await client.call_tool("measure", nlohmann::json::object());
        } catch (const mcp::StructuredContentError& error) {
            thrown = error;
        }
    });

    ASSERT_TRUE(thrown.has_value());
    EXPECT_EQ(thrown->code(), mcp::g_INTERNAL_ERROR);
    ASSERT_TRUE(thrown->data().has_value());
    const auto& violations = thrown->data()->at("violations");
    ASSERT_EQ(violations.size(), 1U);
    EXPECT_EQ(violations[0].at("instancePath"), "/size");
}

TEST_F(ClientToolSchemaTest, ConformingAndErrorResultsAreReturned) {
    tools_ = nlohmann::json::array({tool("measure", {{"type", "object"}}, size_schema())});
    std::optional<mcp::CallToolResult> conforming;
    std::optional<mcp::CallToolResult> error_result;
    run({}, [&](mcp::Client& client) -> mcp::Task<void> {
        co_await client.list_tools();
        call_result_ = structured_result({{"size", 4}});
        conforming = co_await client.call_tool("measure", nlohmann::json::object());
        call_result_ = structured_result({{"reason", "unavailable"}}, true);
        error_result = co_await client.call_tool("measure", nlohmann::json::object());
    });

    ASSERT_TRUE(conforming.has_value());
    EXPECT_EQ(conforming->structuredContent->at("size"), 4);
    ASSERT_TRUE(error_result.has_value());
    EXPECT_TRUE(error_result->isError.value_or(false));
}

TEST_F(ClientToolSchemaTest, UnlistedToolIsNotChecked) {
    call_result_ = structured_result({{"size", "anything"}});
    std::optional<mcp::CallToolResult> result;
    run({}, [&](mcp::Client& client) -> mcp::Task<void> {
        result = co_await client.call_tool("measure", nlohmann::json::object());
    });

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->structuredContent->at("size"), "anything");
}

TEST_F(ClientToolSchemaTest, ALaterListingReplacesTheOutputSchema) {
    tools_ = nlohmann::json::array({tool("measure", {{"type", "object"}}, size_schema())});
    call_result_ = structured_result({{"size", "large"}});
    std::optional<mcp::CallToolResult> result;
    run({}, [&](mcp::Client& client) -> mcp::Task<void> {
        co_await client.list_tools();
        tools_ = nlohmann::json::array({tool(
            "measure", {{"type", "object"}},
            nlohmann::json{{"type", "object"}, {"properties", {{"size", {{"type", "string"}}}}}})});
        co_await client.list_tools();
        result = co_await client.call_tool("measure", nlohmann::json::object());
    });

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->structuredContent->at("size"), "large");
}

TEST_F(ClientToolSchemaTest, ChecksCanBeTurnedOff) {
    tools_ = nlohmann::json::array({tool("measure", {{"type", "object"}}, size_schema()),
                                    tool("invalid", {{"type", "object"}, {"required", "a"}})});
    call_result_ = structured_result({{"size", -1}});
    mcp::ClientOptions options;
    options.validate_tool_schemas = false;
    std::size_t listed = 0;
    std::optional<mcp::CallToolResult> result;
    run(std::move(options), [&](mcp::Client& client) -> mcp::Task<void> {
        listed = (co_await client.list_tools()).tools.size();
        result = co_await client.call_tool("measure", nlohmann::json::object());
    });

    EXPECT_EQ(listed, 2U);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->structuredContent->at("size"), -1);
}

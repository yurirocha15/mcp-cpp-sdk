#include "test_utils.hpp"

#include "mcp/client.hpp"

#include <gtest/gtest.h>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

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

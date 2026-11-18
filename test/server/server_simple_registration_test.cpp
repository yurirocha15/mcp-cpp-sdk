#include "../support/extension_harness.hpp"
#include "../test_utils.hpp"

#include "mcp/core/async.hpp"
#include "mcp/server/server.hpp"

#include <gtest/gtest.h>

#include <mcp/core/constants.hpp>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <vector>

namespace server_simple_registration_test {

namespace jsonrpc = mcp::constants::jsonrpc;

namespace {

/// A legacy request with no session: @p params as given, without the per-request `_meta`.
nlohmann::json legacy_request(std::string_view method, nlohmann::json params) {
    return {{"jsonrpc", "2.0"}, {"id", 1}, {"method", method}, {"params", std::move(params)}};
}

/// The result of calling tool @p name with @p arguments as a 2026-07-28 request.
nlohmann::json call(mcp::Server& server, const std::string& name,
                    nlohmann::json arguments = nlohmann::json::object()) {
    auto response = mcp_test::dispatch_direct(
        server,
        make_modern_request(1, "tools/call", {{"name", name}, {"arguments", std::move(arguments)}}));
    EXPECT_TRUE(response.contains("result")) << response.dump();
    return response.value("result", nlohmann::json::object());
}

/// The text of the first content block of a tools/call result.
std::string first_text(const nlohmann::json& result) {
    return result.at("content").at(0).at("text").get<std::string>();
}

/// The tools/list, resources/list or prompts/list entry named @p name, listed under @p key.
nlohmann::json listed(mcp::Server& server, std::string_view method, const std::string& key,
                      const std::string& field, const std::string& value) {
    auto response = mcp_test::dispatch_direct(server, make_modern_request(1, method));
    for (const auto& entry : response.at("result").at(key)) {
        if (entry.at(field) == value) {
            return entry;
        }
    }
    ADD_FAILURE() << value << " is not listed: " << response.dump();
    return nlohmann::json::object();
}

}  // namespace

// tool_params() writes the object schema its arguments describe.
TEST(ToolParamsTest, WritesAnObjectSchemaWithTheRequiredArgumentsListed) {
    const auto schema = mcp::tool_params({{"city", "string", "The city"},
                                          {"days", "integer", "", false},
                                          {"metric", "boolean", "Use metric units"}});
    EXPECT_EQ(schema, nlohmann::json::parse(R"({
        "type": "object",
        "properties": {
            "city": {"type": "string", "description": "The city"},
            "days": {"type": "integer"},
            "metric": {"type": "boolean", "description": "Use metric units"}
        },
        "required": ["city", "metric"]
    })"));
}

TEST(ToolParamsTest, LeavesOutRequiredWhenNoArgumentIsRequired) {
    EXPECT_EQ(mcp::tool_params({}),
              (nlohmann::json{{"type", "object"}, {"properties", nlohmann::json::object()}}));
    const auto schema =
        mcp::tool_params({{"tags", "array", "", false}, {"extra", "object", "", false}});
    EXPECT_FALSE(schema.contains("required")) << schema.dump();
    EXPECT_EQ(schema["properties"]["tags"]["type"], "array");
    EXPECT_EQ(schema["properties"]["extra"]["type"], "object");
}

TEST(ToolParamsTest, RefusesAMissingNameARepeatedNameAndAnUnknownType) {
    EXPECT_THROW(static_cast<void>(mcp::tool_params({{"", "string", ""}})), std::invalid_argument);
    EXPECT_THROW(static_cast<void>(mcp::tool_params({{"a", "string", ""}, {"a", "number", ""}})),
                 std::invalid_argument);
    EXPECT_THROW(static_cast<void>(mcp::tool_params({{"a", "str", ""}})), std::invalid_argument);
    EXPECT_THROW(static_cast<void>(mcp::tool_params({{"a", "", ""}})), std::invalid_argument);
}

// A schema from tool_params() is accepted by add_tool() and checks the call's arguments.
TEST(ToolParamsTest, SchemaChecksTheArgumentsOfTheToolItDescribes) {
    mcp::Server server("simple", "1.0");
    server.add_tool("greet", "Greets someone", mcp::tool_params({{"name", "string", "Who to greet"}}),
                    [](const nlohmann::json& args) -> nlohmann::json {
                        return "Hello, " + args.at("name").get<std::string>();
                    });

    EXPECT_EQ(first_text(call(server, "greet", {{"name", "Ada"}})), "Hello, Ada");
    auto refused = call(server, "greet");
    EXPECT_EQ(refused.value("isError", false), true) << refused.dump();
}

TEST(SimpleToolTest, SyncHandlerWithContextGetsTheRequestsContext) {
    mcp::Server server("simple", "1.0");
    server.add_tool("state", "Reports the request's state", nlohmann::json{{"type", "object"}},
                    [](mcp::Context& ctx, const nlohmann::json& /*args*/) -> nlohmann::json {
                        return ctx.is_cancelled() ? "cancelled" : "running";
                    });

    EXPECT_EQ(first_text(call(server, "state")), "running");
}

TEST(SimpleToolTest, CoroutineHandlersRunWithAndWithoutTheContext) {
    mcp::Server server("simple", "1.0");
    const nlohmann::json schema{{"type", "object"}};
    server.add_tool("plain", "No context", schema,
                    [](const nlohmann::json& args) -> mcp::Task<nlohmann::json> {
                        co_return nlohmann::json("plain " + args.value("word", std::string("none")));
                    });
    server.add_tool("with_context", "With context", schema,
                    [](mcp::Context& ctx, const nlohmann::json& /*args*/) -> mcp::Task<nlohmann::json> {
                        co_return nlohmann::json(ctx.is_cancelled() ? "cancelled" : "running");
                    });

    EXPECT_EQ(first_text(call(server, "plain", {{"word", "hi"}})), "plain hi");
    EXPECT_EQ(first_text(call(server, "with_context")), "running");
}

// A Tool definition keeps everything it describes, whichever handler form registers it.
TEST(SimpleToolTest, ToolDefinitionFormListsItsTitleAndAnnotations) {
    mcp::Server server("simple", "1.0");
    mcp::Tool sync_tool;
    sync_tool.name = "sync_tool";
    sync_tool.title = "Sync tool";
    sync_tool.inputSchema = mcp::tool_params({{"text", "string", "Text to echo"}});
    sync_tool.annotations = mcp::ToolAnnotations{};
    sync_tool.annotations->readOnlyHint = true;
    server.add_tool(sync_tool,
                    [](const nlohmann::json& args) -> nlohmann::json { return args.at("text"); });

    auto context_tool = sync_tool;
    context_tool.name = "context_tool";
    server.add_tool(context_tool,
                    [](mcp::Context& /*ctx*/, const nlohmann::json& args) -> nlohmann::json {
                        return args.at("text");
                    });

    auto coroutine_tool = sync_tool;
    coroutine_tool.name = "coroutine_tool";
    server.add_tool(coroutine_tool, [](const nlohmann::json& args) -> mcp::Task<nlohmann::json> {
        co_return args.at("text");
    });

    for (const auto* name : {"sync_tool", "context_tool", "coroutine_tool"}) {
        auto entry = listed(server, "tools/list", "tools", "name", name);
        EXPECT_EQ(entry.value("title", ""), "Sync tool") << entry.dump();
        EXPECT_EQ(entry["annotations"]["readOnlyHint"], true) << entry.dump();
        EXPECT_EQ(entry["inputSchema"], sync_tool.inputSchema) << entry.dump();
        EXPECT_EQ(first_text(call(server, name, {{"text", name}})), name);
    }
    EXPECT_THROW(server.add_tool(sync_tool, [](const nlohmann::json&) -> nlohmann::json { return {}; }),
                 std::invalid_argument);
}

TEST(SimpleToolTest, ToolDefinitionWithAnOutputSchemaReturnsStructuredContent) {
    mcp::Server server("simple", "1.0");
    mcp::Tool tool;
    tool.name = "count";
    tool.inputSchema = mcp::tool_params({{"text", "string", "Text to count"}});
    tool.outputSchema = nlohmann::json::parse(
        R"({"type": "object", "properties": {"length": {"type": "integer"}}, "required": ["length"]})");
    server.add_tool(tool, [](const nlohmann::json& args) -> nlohmann::json {
        return {{"length", args.at("text").get<std::string>().size()}};
    });

    auto result = call(server, "count", {{"text", "four"}});
    EXPECT_EQ(result["structuredContent"], (nlohmann::json{{"length", 4}})) << result.dump();
    EXPECT_EQ(first_text(result), R"({"length":4})");
    EXPECT_EQ(listed(server, "tools/list", "tools", "name", "count")["outputSchema"],
              *tool.outputSchema);
}

TEST(SimpleToolTest, RawToolTakesACoroutineWithOrWithoutTheContext) {
    mcp::Server server("simple", "1.0");
    mcp::Tool tool;
    tool.name = "raw";
    tool.inputSchema = nlohmann::json{{"type", "object"}};
    server.add_raw_tool(tool, [](const nlohmann::json& /*args*/) -> mcp::Task<nlohmann::json> {
        auto result = mcp::make_tool_text_result("raw");
        result.structuredContent = nlohmann::json{{"kind", "raw"}};
        co_return nlohmann::json(result);
    });
    tool.name = "erased";
    mcp::TypeErasedHandler erased = [](mcp::Context& /*ctx*/,
                                       const nlohmann::json& /*args*/) -> mcp::Task<nlohmann::json> {
        co_return nlohmann::json(mcp::make_tool_error_result("refused"));
    };
    server.add_raw_tool(tool, erased);

    auto raw = call(server, "raw");
    EXPECT_EQ(first_text(raw), "raw");
    EXPECT_EQ(raw["structuredContent"]["kind"], "raw");
    auto refused = call(server, "erased");
    EXPECT_EQ(first_text(refused), "refused");
    EXPECT_EQ(refused.value("isError", false), true);
}

TEST(SimpleResourceTest, TextResourceIsReadAsTextOfItsMimeType) {
    mcp::Server server("simple", "1.0");
    int reads = 0;
    server.add_text_resource("config://motd", "Message of the day", [&reads] {
        ++reads;
        return std::string("hello ") + std::to_string(reads);
    });
    server.add_text_resource(
        "docs://readme", "Readme", [] { return std::string("# Readme"); }, "text/markdown");

    auto first = mcp_test::dispatch_direct(
        server, make_modern_request(1, "resources/read", {{"uri", "config://motd"}}));
    EXPECT_EQ(first["result"]["contents"],
              nlohmann::json::parse(
                  R"([{"uri": "config://motd", "mimeType": "text/plain", "text": "hello 1"}])"))
        << first.dump();
    auto second =
        mcp_test::dispatch_direct(server, legacy_request("resources/read", {{"uri", "config://motd"}}));
    EXPECT_EQ(second["result"]["contents"][0]["text"], "hello 2") << second.dump();

    auto readme = mcp_test::dispatch_direct(
        server, make_modern_request(1, "resources/read", {{"uri", "docs://readme"}}));
    EXPECT_EQ(readme["result"]["contents"][0]["mimeType"], "text/markdown") << readme.dump();
    EXPECT_EQ(readme["result"]["contents"][0]["text"], "# Readme");

    auto entry = listed(server, "resources/list", "resources", "uri", "docs://readme");
    EXPECT_EQ(entry.value("name", ""), "Readme");
    EXPECT_EQ(entry.value("mimeType", ""), "text/markdown");
}

TEST(SimpleResourceTest, ResourceFormKeepsItsDescriptionAndDefaultsToPlainText) {
    mcp::Server server("simple", "1.0");
    mcp::Resource resource;
    resource.uri = "notes://today";
    resource.name = "today";
    resource.title = "Today's notes";
    resource.description = "What happened today";
    server.add_text_resource(resource, [] { return std::string("nothing yet"); });

    auto coroutine_resource = resource;
    coroutine_resource.uri = "notes://tomorrow";
    coroutine_resource.mimeType = "text/markdown";
    server.add_text_resource(coroutine_resource,
                             []() -> mcp::Task<std::string> { co_return std::string("*planned*"); });

    auto today = listed(server, "resources/list", "resources", "uri", "notes://today");
    EXPECT_EQ(today.value("title", ""), "Today's notes");
    EXPECT_EQ(today.value("description", ""), "What happened today");
    EXPECT_EQ(today.value("mimeType", ""), "text/plain");

    auto tomorrow = mcp_test::dispatch_direct(
        server, make_modern_request(1, "resources/read", {{"uri", "notes://tomorrow"}}));
    EXPECT_EQ(tomorrow["result"]["contents"],
              nlohmann::json::parse(
                  R"([{"uri": "notes://tomorrow", "mimeType": "text/markdown", "text": "*planned*"}])"))
        << tomorrow.dump();
}

TEST(SimpleResourceTest, CoroutineFormByUriAndASecondRegistrationOfAUri) {
    mcp::Server server("simple", "1.0");
    server.add_text_resource("live://clock", "Clock",
                             []() -> mcp::Task<std::string> { co_return std::string("noon"); });

    auto read =
        mcp_test::dispatch_direct(server, legacy_request("resources/read", {{"uri", "live://clock"}}));
    EXPECT_EQ(read["result"]["contents"][0]["text"], "noon") << read.dump();
    EXPECT_EQ(read["result"]["contents"][0]["mimeType"], "text/plain");

    EXPECT_THROW(server.add_text_resource("live://clock", "Clock again", [] { return std::string(); }),
                 std::invalid_argument);
    EXPECT_THROW(server.add_text_resource("live://clock", "Clock again",
                                          []() -> mcp::Task<std::string> { co_return std::string(); }),
                 std::invalid_argument);
}

namespace {

std::vector<mcp::PromptArgument> review_arguments() {
    mcp::PromptArgument code;
    code.name = "code";
    code.description = "The code to review";
    code.required = true;
    mcp::PromptArgument focus;
    focus.name = "focus";
    return {code, focus};
}

}  // namespace

TEST(SimplePromptTest, RendersOneUserMessageFromTheArguments) {
    mcp::Server server("simple", "1.0");
    server.add_text_prompt(
        "review", "Review code", review_arguments(), [](const mcp::Server::PromptArguments& args) {
            const auto focus = args.find("focus");
            return "Review " + args.at("code") + (focus == args.end() ? "" : " for " + focus->second);
        });

    auto response = mcp_test::dispatch_direct(
        server, make_modern_request(
                    1, "prompts/get",
                    {{"name", "review"}, {"arguments", {{"code", "x = 1"}, {"focus", "style"}}}}));
    EXPECT_EQ(response["result"]["description"], "Review code") << response.dump();
    EXPECT_EQ(response["result"]["messages"], nlohmann::json::parse(R"([
        {"role": "user", "content": {"type": "text", "text": "Review x = 1 for style"}}
    ])"))
        << response.dump();

    auto entry = listed(server, "prompts/list", "prompts", "name", "review");
    EXPECT_EQ(entry["arguments"][0]["name"], "code") << entry.dump();
    EXPECT_EQ(entry["arguments"][0]["required"], true);
    EXPECT_EQ(entry["arguments"][1]["name"], "focus");
}

// A request without a required argument is refused before the prompt renders, in both eras.
TEST(SimplePromptTest, MissingRequiredArgumentIsInvalidParams) {
    mcp::Server server("simple", "1.0");
    int renders = 0;
    server.add_text_prompt("review", "Review code", review_arguments(),
                           [&renders](const mcp::Server::PromptArguments&) {
                               ++renders;
                               return std::string("rendered");
                           });

    const nlohmann::json params{{"name", "review"}, {"arguments", {{"focus", "style"}}}};
    for (const auto& request :
         {make_modern_request(1, "prompts/get", params), legacy_request("prompts/get", params)}) {
        auto response = mcp_test::dispatch_direct(server, request);
        ASSERT_TRUE(response.contains("error")) << response.dump();
        EXPECT_EQ(response["error"]["code"], jsonrpc::g_INVALID_PARAMS) << response.dump();
        EXPECT_NE(response["error"]["message"].get<std::string>().find("code"), std::string::npos)
            << response.dump();
    }
    EXPECT_EQ(renders, 0);
}

TEST(SimplePromptTest, AbsentArgumentsReachTheHandlerAsAnEmptyMap) {
    mcp::Server server("simple", "1.0");
    server.add_text_prompt("hello", "Says hello", {}, [](const mcp::Server::PromptArguments& args) {
        return "arguments: " + std::to_string(args.size());
    });

    auto response =
        mcp_test::dispatch_direct(server, legacy_request("prompts/get", {{"name", "hello"}}));
    EXPECT_EQ(response["result"]["messages"][0]["content"]["text"], "arguments: 0") << response.dump();
}

TEST(SimplePromptTest, CoroutineAndPromptFormsRenderAndKeepTheirTitle) {
    mcp::Server server("simple", "1.0");
    mcp::Prompt prompt;
    prompt.name = "summary";
    prompt.title = "Summary";
    prompt.arguments = review_arguments();
    server.add_text_prompt(prompt,
                           [](const mcp::Server::PromptArguments& args) -> mcp::Task<std::string> {
                               co_return "Summarize " + args.at("code");
                           });
    server.add_text_prompt("plain", "Plain coroutine", review_arguments(),
                           [](const mcp::Server::PromptArguments& args) -> mcp::Task<std::string> {
                               co_return "Plain " + args.at("code");
                           });

    auto summary = mcp_test::dispatch_direct(
        server,
        make_modern_request(1, "prompts/get", {{"name", "summary"}, {"arguments", {{"code", "f()"}}}}));
    EXPECT_EQ(summary["result"]["messages"][0]["content"]["text"], "Summarize f()") << summary.dump();
    EXPECT_FALSE(summary["result"].contains("description")) << summary.dump();
    auto plain = mcp_test::dispatch_direct(
        server, legacy_request("prompts/get", {{"name", "plain"}, {"arguments", {{"code", "g()"}}}}));
    EXPECT_EQ(plain["result"]["messages"][0]["content"]["text"], "Plain g()") << plain.dump();
    auto missing =
        mcp_test::dispatch_direct(server, legacy_request("prompts/get", {{"name", "plain"}}));
    EXPECT_EQ(missing["error"]["code"], jsonrpc::g_INVALID_PARAMS) << missing.dump();

    EXPECT_EQ(listed(server, "prompts/list", "prompts", "name", "summary").value("title", ""),
              "Summary");
    EXPECT_THROW(server.add_text_prompt(
                     prompt, [](const mcp::Server::PromptArguments&) { return std::string(); }),
                 std::invalid_argument);
}

TEST(ResultTextTest, JoinsTheTextBlocksAndSkipsTheRest) {
    mcp::CallToolResult result;
    EXPECT_EQ(mcp::result_text(result), "");
    result.content.emplace_back(mcp::TextContent{.text = "first"});
    mcp::ImageContent image;
    image.data = "aGk=";
    image.mimeType = "image/png";
    result.content.emplace_back(image);
    result.content.emplace_back(mcp::TextContent{.text = "second"});
    result.structuredContent = nlohmann::json{{"ignored", true}};
    EXPECT_EQ(mcp::result_text(result), "first\nsecond");
    EXPECT_EQ(mcp::result_text(mcp::make_tool_text_result("only")), "only");
}

}  // namespace server_simple_registration_test

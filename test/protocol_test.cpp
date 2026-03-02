#include "mcp/protocol.hpp"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

TEST(ProtocolTest, InitializeRequestSerialization) {
    mcp::InitializeRequest req;
    req.protocolVersion = std::string(mcp::LATEST_PROTOCOL_VERSION);
    req.clientInfo = {"test-client", "1.0.0"};
    req.capabilities = nlohmann::json::object();

    json json_obj = req;

    EXPECT_EQ(json_obj["protocolVersion"], mcp::LATEST_PROTOCOL_VERSION);
    EXPECT_EQ(json_obj["clientInfo"]["name"], "test-client");
    EXPECT_EQ(json_obj["clientInfo"]["version"], "1.0.0");
    EXPECT_TRUE(json_obj["capabilities"].is_object());
}

TEST(ProtocolTest, InitializeResultDeserialization) {
    json json_obj = {{"protocolVersion", mcp::LATEST_PROTOCOL_VERSION},
                     {"capabilities", {}},
                     {"serverInfo", {{"name", "test-server"}, {"version", "0.1.0"}}}};

    mcp::InitializeResult res = json_obj.get<mcp::InitializeResult>();

    EXPECT_EQ(res.protocolVersion, mcp::LATEST_PROTOCOL_VERSION);
    EXPECT_EQ(res.serverInfo.name, "test-server");
    EXPECT_FALSE(res.instructions.has_value());
}

TEST(ProtocolTest, SupportedProtocolVersions) {
    // Verify that we support the requested versions
    bool has_2024_11_05 = false;
    bool has_2025_03_26 = false;
    bool has_2025_06_18 = false;

    for (const auto& version : mcp::SUPPORTED_PROTOCOL_VERSIONS) {
        if (version == "2024-11-05") {
            has_2024_11_05 = true;
        }
        if (version == "2025-03-26") {
            has_2025_03_26 = true;
        }
        if (version == "2025-06-18") {
            has_2025_06_18 = true;
        }
    }

    EXPECT_TRUE(has_2024_11_05) << "Protocol version 2024-11-05 not found in supported versions";
    EXPECT_TRUE(has_2025_03_26) << "Protocol version 2025-03-26 not found in supported versions";
    EXPECT_TRUE(has_2025_06_18) << "Protocol version 2025-06-18 not found in supported versions";
}

TEST(ProtocolTest, ParseInitializeRequest_2024_11_05) {
    // Schema 2024-11-05: Implementation has name and version
    std::string json_str = R"({
        "protocolVersion": "2024-11-05",
        "capabilities": {},
        "clientInfo": {
            "name": "example-client",
            "version": "1.0.0"
        }
    })";

    auto json_obj = json::parse(json_str);
    mcp::InitializeRequest req = json_obj.get<mcp::InitializeRequest>();

    EXPECT_EQ(req.protocolVersion, "2024-11-05");
    EXPECT_EQ(req.clientInfo.name, "example-client");
    EXPECT_EQ(req.clientInfo.version, "1.0.0");
    EXPECT_FALSE(req.clientInfo.title.has_value());
}

TEST(ProtocolTest, ParseInitializeRequest_2025_03_26) {
    // Schema 2025-03-26: Implementation has only name and version
    std::string json_str = R"({
        "protocolVersion": "2025-03-26",
        "capabilities": {},
        "clientInfo": {
            "name": "example-client",
            "version": "1.0.0"
        }
    })";

    auto json_obj = json::parse(json_str);
    mcp::InitializeRequest req = json_obj.get<mcp::InitializeRequest>();

    EXPECT_EQ(req.protocolVersion, "2025-03-26");
    EXPECT_EQ(req.clientInfo.name, "example-client");
    EXPECT_EQ(req.clientInfo.version, "1.0.0");
    EXPECT_FALSE(req.clientInfo.title.has_value());
}

TEST(ProtocolTest, ParseInitializeRequest_2025_06_18) {
    // Schema 2025-06-18: Implementation adds title, websiteUrl, icons (we support title/websiteUrl)
    std::string json_str = R"({
        "protocolVersion": "2025-06-18",
        "capabilities": {},
        "clientInfo": {
            "name": "example-client",
            "version": "1.0.0",
            "title": "Example Client",
            "websiteUrl": "https://example.com"
        }
    })";

    auto json_obj = json::parse(json_str);
    mcp::InitializeRequest req = json_obj.get<mcp::InitializeRequest>();

    EXPECT_EQ(req.protocolVersion, "2025-06-18");
    EXPECT_EQ(req.clientInfo.name, "example-client");
    EXPECT_EQ(req.clientInfo.version, "1.0.0");
    ASSERT_TRUE(req.clientInfo.title.has_value());
    EXPECT_EQ(req.clientInfo.title.value_or(""), "Example Client");
    ASSERT_TRUE(req.clientInfo.websiteUrl.has_value());
    EXPECT_EQ(req.clientInfo.websiteUrl.value_or(""), "https://example.com");
}

TEST(ProtocolTest, ContentBlockSerialization) {
    mcp::TextContent text_content;
    text_content.text = "Hello, world!";

    json json_text = text_content;
    EXPECT_EQ(json_text["type"], "text");
    EXPECT_EQ(json_text["text"], "Hello, world!");

    mcp::ImageContent image_content;
    image_content.data = "base64data";
    image_content.mimeType = "image/png";

    json json_image = image_content;
    EXPECT_EQ(json_image["type"], "image");
    EXPECT_EQ(json_image["data"], "base64data");
    EXPECT_EQ(json_image["mimeType"], "image/png");
}

TEST(ProtocolTest, MixedContentBlockSerialization) {
    mcp::CallToolResult result;
    result.content.emplace_back(mcp::TextContent{.text = "Output text"});
    result.content.emplace_back(mcp::ImageContent{.data = "imgdata", .mimeType = "image/jpeg"});
    result.isError = false;

    json json_obj = result;
    EXPECT_EQ(json_obj["content"].size(), 2);
    EXPECT_EQ(json_obj["content"][0]["type"], "text");
    EXPECT_EQ(json_obj["content"][0]["text"], "Output text");
    EXPECT_EQ(json_obj["content"][1]["type"], "image");
    EXPECT_EQ(json_obj["content"][1]["data"], "imgdata");
    EXPECT_EQ(json_obj["content"][1]["mimeType"], "image/jpeg");
    EXPECT_EQ(json_obj["isError"], false);

    // Round-trip
    mcp::CallToolResult deserialized = json_obj.get<mcp::CallToolResult>();
    EXPECT_EQ(deserialized.content.size(), 2);
    EXPECT_TRUE(std::holds_alternative<mcp::TextContent>(deserialized.content[0]));
    EXPECT_TRUE(std::holds_alternative<mcp::ImageContent>(deserialized.content[1]));

    EXPECT_EQ(std::get<mcp::TextContent>(deserialized.content[0]).text, "Output text");
    EXPECT_EQ(std::get<mcp::ImageContent>(deserialized.content[1]).mimeType, "image/jpeg");
}

TEST(ProtocolTest, ToolSerialization) {
    mcp::Tool tool;
    tool.name = "test-tool";
    tool.description = "A test tool";
    tool.inputSchema = {{"type", "object"}, {"properties", {{"arg1", {{"type", "string"}}}}}};

    json json_obj = tool;
    EXPECT_EQ(json_obj["name"], "test-tool");
    EXPECT_EQ(json_obj["description"], "A test tool");
    EXPECT_EQ(json_obj["inputSchema"]["type"], "object");

    mcp::Tool deserialized = json_obj.get<mcp::Tool>();
    EXPECT_EQ(deserialized.name, "test-tool");
    ASSERT_TRUE(deserialized.description.has_value());
    EXPECT_EQ(deserialized.description.value_or(""), "A test tool");
}

TEST(ProtocolTest, ResourceSerialization) {
    mcp::Resource resource;
    resource.uri = "file:///test.txt";
    resource.name = "Test Resource";
    resource.mimeType = "text/plain";

    json json_obj = resource;
    EXPECT_EQ(json_obj["uri"], "file:///test.txt");
    EXPECT_EQ(json_obj["name"], "Test Resource");
    EXPECT_EQ(json_obj["mimeType"], "text/plain");
    EXPECT_FALSE(json_obj.contains("description"));

    mcp::Resource deserialized = json_obj.get<mcp::Resource>();
    EXPECT_EQ(deserialized.uri, "file:///test.txt");
    EXPECT_EQ(deserialized.name, "Test Resource");
    ASSERT_TRUE(deserialized.mimeType.has_value());
    EXPECT_EQ(deserialized.mimeType.value_or(""), "text/plain");
    EXPECT_FALSE(deserialized.description.has_value());
}

TEST(ProtocolTest, ResourceContentsSerialization) {
    mcp::TextResourceContents text_content{"file:///test.txt", "Hello World", "text/plain"};
    mcp::ResourceContents rc_text = text_content;
    nlohmann::json j_text = rc_text;
    EXPECT_EQ(j_text["uri"], "file:///test.txt");
    EXPECT_EQ(j_text["text"], "Hello World");
    EXPECT_EQ(j_text["mimeType"], "text/plain");

    auto deserialized_text = j_text.get<mcp::ResourceContents>();
    EXPECT_TRUE(std::holds_alternative<mcp::TextResourceContents>(deserialized_text));
    EXPECT_EQ(std::get<mcp::TextResourceContents>(deserialized_text).text, "Hello World");

    mcp::BlobResourceContents blob_content{"file:///test.bin", "base64blob",
                                           "application/octet-stream"};
    mcp::ResourceContents rc_blob = blob_content;
    nlohmann::json j_blob = rc_blob;
    EXPECT_EQ(j_blob["uri"], "file:///test.bin");
    EXPECT_EQ(j_blob["blob"], "base64blob");

    auto deserialized_blob = j_blob.get<mcp::ResourceContents>();
    EXPECT_TRUE(std::holds_alternative<mcp::BlobResourceContents>(deserialized_blob));
    EXPECT_EQ(std::get<mcp::BlobResourceContents>(deserialized_blob).blob, "base64blob");
}

TEST(ProtocolTest, ResourceTemplateSerialization) {
    mcp::ResourceTemplate tmpl{"file:///{path}", "test_template", "A test template", "text/plain"};
    nlohmann::json j = tmpl;
    EXPECT_EQ(j["uriTemplate"], "file:///{path}");
    EXPECT_EQ(j["name"], "test_template");
    EXPECT_EQ(j["description"], "A test template");

    auto deserialized = j.get<mcp::ResourceTemplate>();
    EXPECT_EQ(deserialized.uriTemplate, "file:///{path}");
    EXPECT_EQ(deserialized.name, "test_template");
    EXPECT_EQ(deserialized.description, "A test template");
}

TEST(ProtocolTest, OptionalFieldsSerialization) {
    // Test Icon
    mcp::Icon icon;
    icon.source = "https://example.com/icon.png";
    icon.mimeType = "image/png";
    icon.sizes = std::vector<std::string>{"32x32", "64x64"};
    icon.theme = "light";

    json json_icon = icon;
    std::cout << "Icon JSON: " << json_icon.dump() << "\n";
    EXPECT_EQ(json_icon["src"], "https://example.com/icon.png");
    EXPECT_EQ(json_icon["mimeType"], "image/png");
    EXPECT_EQ(json_icon["sizes"][0], "32x32");
    EXPECT_EQ(json_icon["theme"], "light");

    mcp::Icon deserialized_icon = json_icon.get<mcp::Icon>();
    EXPECT_EQ(deserialized_icon.source, "https://example.com/icon.png");
    EXPECT_EQ(deserialized_icon.mimeType.value_or(""), "image/png");
    EXPECT_EQ(deserialized_icon.sizes.value_or(std::vector<std::string>{})[0], "32x32");
    EXPECT_EQ(deserialized_icon.theme.value_or(""), "light");

    // Test Annotations
    mcp::Annotations annotations;
    annotations.audience = std::vector<mcp::Role>{mcp::Role::User, mcp::Role::Assistant};
    annotations.lastModified = "2023-10-27T10:00:00Z";
    constexpr double priority_val = 0.8;
    annotations.priority = priority_val;

    json json_annotations = annotations;
    EXPECT_EQ(json_annotations["audience"][0], "user");
    EXPECT_EQ(json_annotations["lastModified"], "2023-10-27T10:00:00Z");
    EXPECT_EQ(json_annotations["priority"], priority_val);

    mcp::Annotations deserialized_annotations = json_annotations.get<mcp::Annotations>();
    EXPECT_EQ(deserialized_annotations.audience.value_or(std::vector<mcp::Role>{})[0], mcp::Role::User);
    EXPECT_EQ(deserialized_annotations.lastModified.value_or(""), "2023-10-27T10:00:00Z");
    EXPECT_EQ(deserialized_annotations.priority.value_or(0.0), priority_val);

    // Test Tool with new fields
    mcp::Tool tool;
    tool.name = "complex-tool";
    tool.inputSchema = nlohmann::json::object();
    tool.meta = nlohmann::json::object({{"custom", "data"}});
    tool.annotations = mcp::ToolAnnotations{.destructiveHint = true, .title = "Complex Tool"};
    tool.outputSchema = nlohmann::json::object({{"type", "string"}});
    tool.title = "Display Title";
    tool.icons = {icon};

    json json_tool = tool;
    EXPECT_EQ(json_tool["_meta"]["custom"], "data");
    EXPECT_EQ(json_tool["annotations"]["destructiveHint"], true);
    EXPECT_EQ(json_tool["outputSchema"]["type"], "string");
    EXPECT_EQ(json_tool["title"], "Display Title");
    EXPECT_EQ(json_tool["icons"][0]["src"], "https://example.com/icon.png");

    mcp::Tool deserialized_tool = json_tool.get<mcp::Tool>();
    EXPECT_EQ(deserialized_tool.meta.value_or(nlohmann::json::object())["custom"], "data");
    EXPECT_EQ(
        deserialized_tool.annotations.value_or(mcp::ToolAnnotations{}).destructiveHint.value_or(false),
        true);
    EXPECT_EQ(deserialized_tool.outputSchema.value_or(nlohmann::json::object())["type"], "string");
    EXPECT_EQ(deserialized_tool.title.value_or(""), "Display Title");
    EXPECT_EQ(deserialized_tool.icons.value_or(std::vector<mcp::Icon>{})[0].source,
              "https://example.com/icon.png");

    // Test Resource with new fields
    mcp::Resource resource;
    resource.uri = "file:///test";
    resource.name = "test";
    constexpr int64_t size_val = 1024;
    resource.size = size_val;
    resource.annotations = annotations;

    json json_resource = resource;
    EXPECT_EQ(json_resource["size"], size_val);
    EXPECT_EQ(json_resource["annotations"]["priority"], priority_val);

    mcp::Resource deserialized_resource = json_resource.get<mcp::Resource>();
    EXPECT_EQ(deserialized_resource.size.value_or(0), size_val);
    EXPECT_EQ(deserialized_resource.annotations.value_or(mcp::Annotations{}).priority.value_or(0.0),
              priority_val);
}

TEST(ProtocolTest, CompletionSerialization) {
    // Test CompleteReference
    mcp::CompleteReference ref;
    ref.type = "ref/prompt";
    ref.name = "my_prompt";

    json json_ref = ref;
    EXPECT_EQ(json_ref["type"], "ref/prompt");
    EXPECT_EQ(json_ref["name"], "my_prompt");

    mcp::CompleteReference deserialized_ref = json_ref.get<mcp::CompleteReference>();
    EXPECT_EQ(deserialized_ref.type, "ref/prompt");
    EXPECT_EQ(deserialized_ref.name.value_or(""), "my_prompt");

    // Test CompleteParams
    mcp::CompleteParams params;
    params.ref = ref;
    params.argument = mcp::CompleteParamsArgument{.name = "arg", .value = "val"};
    params.context =
        mcp::CompleteContext{.arguments = std::map<std::string, std::string>{{"key", "value"}}};
    params.meta = nlohmann::json::object({{"custom", "data"}});

    json json_params = params;
    EXPECT_EQ(json_params["ref"]["type"], "ref/prompt");
    EXPECT_EQ(json_params["argument"]["name"], "arg");
    EXPECT_EQ(json_params["context"]["arguments"]["key"], "value");
    EXPECT_EQ(json_params["_meta"]["custom"], "data");

    mcp::CompleteParams deserialized_params = json_params.get<mcp::CompleteParams>();
    EXPECT_EQ(deserialized_params.ref.type, "ref/prompt");
    EXPECT_EQ(deserialized_params.argument.name, "arg");
    EXPECT_EQ(deserialized_params.context.value_or(mcp::CompleteContext{})
                  .arguments.value_or(std::map<std::string, std::string>{})["key"],
              "value");
    EXPECT_EQ(deserialized_params.meta.value_or(nlohmann::json::object())["custom"], "data");

    // Test CompleteResult
    mcp::CompleteResult result;
    result.completion.values = {"val1", "val2"};
    constexpr int64_t total_val = 10;
    result.completion.total = total_val;
    result.completion.hasMore = true;
    result.meta = nlohmann::json::object({{"custom", "result"}});

    json json_result = result;
    EXPECT_EQ(json_result["completion"]["values"][0], "val1");
    EXPECT_EQ(json_result["completion"]["total"], total_val);
    EXPECT_EQ(json_result["completion"]["hasMore"], true);
    EXPECT_EQ(json_result["_meta"]["custom"], "result");

    mcp::CompleteResult deserialized_result = json_result.get<mcp::CompleteResult>();
    EXPECT_EQ(deserialized_result.completion.values[0], "val1");
    EXPECT_EQ(deserialized_result.completion.total.value_or(0), total_val);
    EXPECT_EQ(deserialized_result.completion.hasMore.value_or(false), true);
    EXPECT_EQ(deserialized_result.meta.value_or(nlohmann::json::object())["custom"], "result");
}

TEST(ProtocolTest, PromptSerialization) {
    mcp::PromptArgument arg{"arg1", "An argument", true, "Argument 1"};
    mcp::Prompt prompt{"test_prompt", "A test prompt", std::vector<mcp::PromptArgument>{arg}};

    nlohmann::json j = prompt;
    EXPECT_EQ(j["name"], "test_prompt");
    EXPECT_EQ(j["description"], "A test prompt");
    EXPECT_EQ(j["arguments"][0]["name"], "arg1");
    EXPECT_EQ(j["arguments"][0]["required"], true);

    auto deserialized = j.get<mcp::Prompt>();
    EXPECT_EQ(deserialized.name, "test_prompt");
    EXPECT_EQ(deserialized.description, "A test prompt");
    EXPECT_EQ(deserialized.arguments->size(), 1);
    EXPECT_EQ(deserialized.arguments->at(0).name, "arg1");
}

TEST(ProtocolTest, GetPromptRequestSerialization) {
    mcp::GetPromptRequestParams params{"test_prompt",
                                       std::map<std::string, std::string>{{"arg1", "value1"}}};
    mcp::GetPromptRequest req{"prompts/get", params};

    nlohmann::json j = req;
    EXPECT_EQ(j["method"], "prompts/get");
    EXPECT_EQ(j["params"]["name"], "test_prompt");
    EXPECT_EQ(j["params"]["arguments"]["arg1"], "value1");

    auto deserialized = j.get<mcp::GetPromptRequest>();
    EXPECT_EQ(deserialized.method, "prompts/get");
    EXPECT_EQ(deserialized.params.name, "test_prompt");
    EXPECT_EQ(deserialized.params.arguments->at("arg1"), "value1");
}

TEST(ProtocolTest, GetPromptResultSerialization) {
    mcp::TextContent text{"text", "Hello"};
    mcp::PromptMessage msg{mcp::Role::Assistant, text};
    mcp::GetPromptResult res{std::vector<mcp::PromptMessage>{msg}, "A description"};

    nlohmann::json j = res;
    EXPECT_EQ(j["description"], "A description");
    EXPECT_EQ(j["messages"][0]["role"], "assistant");
    EXPECT_EQ(j["messages"][0]["content"]["type"], "text");
    EXPECT_EQ(j["messages"][0]["content"]["text"], "Hello");

    auto deserialized = j.get<mcp::GetPromptResult>();
    EXPECT_EQ(deserialized.description, "A description");
    EXPECT_EQ(deserialized.messages.size(), 1);
    EXPECT_EQ(deserialized.messages[0].role, mcp::Role::Assistant);
    EXPECT_TRUE(std::holds_alternative<mcp::TextContent>(deserialized.messages[0].content));
}

TEST(ProtocolTest, ListPromptsSerialization) {
    mcp::PaginatedRequestParams params{"cursor123"};
    mcp::ListPromptsRequest req{"prompts/list", params};

    nlohmann::json j_req = req;
    EXPECT_EQ(j_req["method"], "prompts/list");
    EXPECT_EQ(j_req["params"]["cursor"], "cursor123");

    auto deserialized_req = j_req.get<mcp::ListPromptsRequest>();
    EXPECT_EQ(deserialized_req.method, "prompts/list");
    EXPECT_EQ(deserialized_req.params->cursor, "cursor123");

    mcp::Prompt prompt{"test_prompt"};
    mcp::ListPromptsResult res{std::vector<mcp::Prompt>{prompt}, "nextCursor456"};

    nlohmann::json j_res = res;
    EXPECT_EQ(j_res["nextCursor"], "nextCursor456");
    EXPECT_EQ(j_res["prompts"][0]["name"], "test_prompt");

    auto deserialized_res = j_res.get<mcp::ListPromptsResult>();
    EXPECT_EQ(deserialized_res.nextCursor, "nextCursor456");
    EXPECT_EQ(deserialized_res.prompts.size(), 1);
    EXPECT_EQ(deserialized_res.prompts[0].name, "test_prompt");
}

TEST(ProtocolTest, ToolChoiceSerialization) {
    mcp::ToolChoice choice;
    choice.mode = "required";

    nlohmann::json j = choice;
    EXPECT_EQ(j["mode"], "required");

    auto deserialized = j.get<mcp::ToolChoice>();
    ASSERT_TRUE(deserialized.mode.has_value());
    EXPECT_EQ(*deserialized.mode, "required");

    // Empty ToolChoice
    mcp::ToolChoice empty;
    nlohmann::json j_empty = empty;
    EXPECT_TRUE(j_empty.is_object());
    EXPECT_FALSE(j_empty.contains("mode"));

    auto deserialized_empty = j_empty.get<mcp::ToolChoice>();
    EXPECT_FALSE(deserialized_empty.mode.has_value());
}

TEST(ProtocolTest, ToolUseContentSerialization) {
    mcp::ToolUseContent content;
    content.id = "call_123";
    content.name = "get_weather";
    content.input = {{"location", "San Francisco"}};

    nlohmann::json j = content;
    EXPECT_EQ(j["type"], "tool_use");
    EXPECT_EQ(j["id"], "call_123");
    EXPECT_EQ(j["name"], "get_weather");
    EXPECT_EQ(j["input"]["location"], "San Francisco");
    EXPECT_FALSE(j.contains("_meta"));

    auto deserialized = j.get<mcp::ToolUseContent>();
    EXPECT_EQ(deserialized.type, "tool_use");
    EXPECT_EQ(deserialized.id, "call_123");
    EXPECT_EQ(deserialized.name, "get_weather");
    EXPECT_EQ(deserialized.input["location"], "San Francisco");
    EXPECT_FALSE(deserialized.meta.has_value());

    // With meta
    content.meta = nlohmann::json{{"cacheId", "abc"}};
    nlohmann::json j_meta = content;
    EXPECT_EQ(j_meta["_meta"]["cacheId"], "abc");
}

TEST(ProtocolTest, ToolResultContentSerialization) {
    mcp::ToolResultContent content;
    content.toolUseId = "call_123";
    content.content.emplace_back(mcp::TextContent{.text = "72°F, sunny"});
    content.isError = false;

    nlohmann::json j = content;
    EXPECT_EQ(j["type"], "tool_result");
    EXPECT_EQ(j["toolUseId"], "call_123");
    EXPECT_EQ(j["content"][0]["type"], "text");
    EXPECT_EQ(j["content"][0]["text"], "72°F, sunny");
    EXPECT_EQ(j["isError"], false);

    auto deserialized = j.get<mcp::ToolResultContent>();
    EXPECT_EQ(deserialized.type, "tool_result");
    EXPECT_EQ(deserialized.toolUseId, "call_123");
    EXPECT_EQ(deserialized.content.size(), 1);
    EXPECT_TRUE(std::holds_alternative<mcp::TextContent>(deserialized.content[0]));
    EXPECT_EQ(std::get<mcp::TextContent>(deserialized.content[0]).text, "72°F, sunny");
    EXPECT_EQ(deserialized.isError, false);
}

TEST(ProtocolTest, SamplingMessageContentBlockSerialization) {
    // Single text block
    mcp::SamplingMessageContentBlock text_block = mcp::TextContent{.text = "Hello"};
    nlohmann::json j_text = text_block;
    EXPECT_EQ(j_text["type"], "text");

    auto deserialized_text = j_text.get<mcp::SamplingMessageContentBlock>();
    EXPECT_TRUE(std::holds_alternative<mcp::TextContent>(deserialized_text));

    // ToolUseContent block
    mcp::ToolUseContent tool_use;
    tool_use.id = "call_1";
    tool_use.name = "calc";
    tool_use.input = {{"x", 42}};

    mcp::SamplingMessageContentBlock tool_block = tool_use;
    nlohmann::json j_tool = tool_block;
    EXPECT_EQ(j_tool["type"], "tool_use");
    EXPECT_EQ(j_tool["id"], "call_1");

    auto deserialized_tool = j_tool.get<mcp::SamplingMessageContentBlock>();
    EXPECT_TRUE(std::holds_alternative<mcp::ToolUseContent>(deserialized_tool));
    EXPECT_EQ(std::get<mcp::ToolUseContent>(deserialized_tool).name, "calc");

    // ToolResultContent block
    mcp::ToolResultContent tool_result;
    tool_result.toolUseId = "call_1";
    tool_result.content.emplace_back(mcp::TextContent{.text = "42"});

    mcp::SamplingMessageContentBlock result_block = tool_result;
    nlohmann::json j_result = result_block;
    EXPECT_EQ(j_result["type"], "tool_result");

    auto deserialized_result = j_result.get<mcp::SamplingMessageContentBlock>();
    EXPECT_TRUE(std::holds_alternative<mcp::ToolResultContent>(deserialized_result));
    EXPECT_EQ(std::get<mcp::ToolResultContent>(deserialized_result).toolUseId, "call_1");
}

TEST(ProtocolTest, SamplingMessageSerialization) {
    // Single content block
    mcp::SamplingMessage msg;
    msg.role = mcp::Role::User;
    msg.content = mcp::SamplingMessageContentBlock{mcp::TextContent{.text = "What is 2+2?"}};

    nlohmann::json j = msg;
    EXPECT_EQ(j["role"], "user");
    EXPECT_EQ(j["content"]["type"], "text");
    EXPECT_EQ(j["content"]["text"], "What is 2+2?");

    auto deserialized = j.get<mcp::SamplingMessage>();
    EXPECT_EQ(deserialized.role, mcp::Role::User);
    EXPECT_TRUE(std::holds_alternative<mcp::SamplingMessageContentBlock>(deserialized.content));
    auto& block = std::get<mcp::SamplingMessageContentBlock>(deserialized.content);
    EXPECT_TRUE(std::holds_alternative<mcp::TextContent>(block));

    // Array content
    mcp::SamplingMessage msg_array;
    msg_array.role = mcp::Role::Assistant;
    std::vector<mcp::SamplingMessageContentBlock> blocks;
    blocks.emplace_back(mcp::TextContent{.text = "Let me calculate..."});
    blocks.emplace_back(
        mcp::ToolUseContent{.id = "call_1", .name = "calc", .input = {{"expr", "2+2"}}});
    msg_array.content = std::move(blocks);

    nlohmann::json j_array = msg_array;
    EXPECT_EQ(j_array["role"], "assistant");
    EXPECT_TRUE(j_array["content"].is_array());
    EXPECT_EQ(j_array["content"].size(), 2);
    EXPECT_EQ(j_array["content"][0]["type"], "text");
    EXPECT_EQ(j_array["content"][1]["type"], "tool_use");

    auto deserialized_array = j_array.get<mcp::SamplingMessage>();
    EXPECT_TRUE(std::holds_alternative<std::vector<mcp::SamplingMessageContentBlock>>(
        deserialized_array.content));
    auto& arr = std::get<std::vector<mcp::SamplingMessageContentBlock>>(deserialized_array.content);
    EXPECT_EQ(arr.size(), 2);
}

TEST(ProtocolTest, ModelHintSerialization) {
    mcp::ModelHint hint;
    hint.name = "claude-3-5-sonnet";

    nlohmann::json j = hint;
    EXPECT_EQ(j["name"], "claude-3-5-sonnet");

    auto deserialized = j.get<mcp::ModelHint>();
    ASSERT_TRUE(deserialized.name.has_value());
    EXPECT_EQ(*deserialized.name, "claude-3-5-sonnet");

    // Empty hint
    mcp::ModelHint empty;
    nlohmann::json j_empty = empty;
    EXPECT_FALSE(j_empty.contains("name"));
}

TEST(ProtocolTest, ModelPreferencesSerialization) {
    constexpr double cost = 0.3;
    constexpr double intelligence = 0.8;
    constexpr double speed = 0.5;

    mcp::ModelPreferences prefs;
    prefs.costPriority = cost;
    prefs.intelligencePriority = intelligence;
    prefs.speedPriority = speed;
    prefs.hints = std::vector<mcp::ModelHint>{{.name = "sonnet"}, {.name = "gpt-4"}};

    nlohmann::json j = prefs;
    EXPECT_EQ(j["costPriority"], cost);
    EXPECT_EQ(j["intelligencePriority"], intelligence);
    EXPECT_EQ(j["speedPriority"], speed);
    EXPECT_EQ(j["hints"].size(), 2);
    EXPECT_EQ(j["hints"][0]["name"], "sonnet");

    auto deserialized = j.get<mcp::ModelPreferences>();
    EXPECT_EQ(deserialized.costPriority, cost);
    EXPECT_EQ(deserialized.intelligencePriority, intelligence);
    EXPECT_EQ(deserialized.speedPriority, speed);
    ASSERT_TRUE(deserialized.hints.has_value());
    EXPECT_EQ(deserialized.hints->size(), 2);
    EXPECT_EQ(deserialized.hints->at(0).name, "sonnet");
}

TEST(ProtocolTest, CreateMessageRequestSerialization) {
    constexpr int max_tokens = 1024;
    constexpr double temperature = 0.7;

    mcp::SamplingMessage msg;
    msg.role = mcp::Role::User;
    msg.content = mcp::SamplingMessageContentBlock{mcp::TextContent{.text = "Hello"}};

    mcp::CreateMessageRequestParams params;
    params.messages = {msg};
    params.maxTokens = max_tokens;
    params.systemPrompt = "You are helpful.";
    params.temperature = temperature;
    params.stopSequences = std::vector<std::string>{"STOP"};
    params.includeContext = "none";

    mcp::CreateMessageRequest req;
    req.params = params;

    nlohmann::json j = req;
    EXPECT_EQ(j["method"], "sampling/createMessage");
    EXPECT_EQ(j["params"]["maxTokens"], max_tokens);
    EXPECT_EQ(j["params"]["systemPrompt"], "You are helpful.");
    EXPECT_EQ(j["params"]["temperature"], temperature);
    EXPECT_EQ(j["params"]["stopSequences"][0], "STOP");
    EXPECT_EQ(j["params"]["includeContext"], "none");
    EXPECT_EQ(j["params"]["messages"].size(), 1);

    auto deserialized = j.get<mcp::CreateMessageRequest>();
    EXPECT_EQ(deserialized.method, "sampling/createMessage");
    EXPECT_EQ(deserialized.params.maxTokens, max_tokens);
    EXPECT_EQ(deserialized.params.systemPrompt, "You are helpful.");
    EXPECT_EQ(deserialized.params.messages.size(), 1);
}

TEST(ProtocolTest, CreateMessageResultSerialization) {
    mcp::CreateMessageResult result;
    result.role = mcp::Role::Assistant;
    result.content = mcp::SamplingMessageContentBlock{mcp::TextContent{.text = "The answer is 4."}};
    result.model = "claude-3-5-sonnet-20241022";
    result.stopReason = "endTurn";

    nlohmann::json j = result;
    EXPECT_EQ(j["role"], "assistant");
    EXPECT_EQ(j["content"]["type"], "text");
    EXPECT_EQ(j["content"]["text"], "The answer is 4.");
    EXPECT_EQ(j["model"], "claude-3-5-sonnet-20241022");
    EXPECT_EQ(j["stopReason"], "endTurn");

    auto deserialized = j.get<mcp::CreateMessageResult>();
    EXPECT_EQ(deserialized.role, mcp::Role::Assistant);
    EXPECT_EQ(deserialized.model, "claude-3-5-sonnet-20241022");
    EXPECT_EQ(deserialized.stopReason, "endTurn");
    EXPECT_TRUE(std::holds_alternative<mcp::SamplingMessageContentBlock>(deserialized.content));
}

TEST(ProtocolTest, RootSerialization) {
    mcp::Root root;
    root.uri = "file:///home/user/project";
    root.name = "My Project";

    nlohmann::json j = root;
    EXPECT_EQ(j["uri"], "file:///home/user/project");
    EXPECT_EQ(j["name"], "My Project");
    EXPECT_FALSE(j.contains("_meta"));

    auto deserialized = j.get<mcp::Root>();
    EXPECT_EQ(deserialized.uri, "file:///home/user/project");
    ASSERT_TRUE(deserialized.name.has_value());
    EXPECT_EQ(*deserialized.name, "My Project");

    // Minimal root (uri only)
    mcp::Root minimal;
    minimal.uri = "file:///tmp";
    nlohmann::json j_min = minimal;
    EXPECT_EQ(j_min["uri"], "file:///tmp");
    EXPECT_FALSE(j_min.contains("name"));

    auto deserialized_min = j_min.get<mcp::Root>();
    EXPECT_EQ(deserialized_min.uri, "file:///tmp");
    EXPECT_FALSE(deserialized_min.name.has_value());
}

TEST(ProtocolTest, ListRootsRequestSerialization) {
    mcp::ListRootsRequest req;

    nlohmann::json j = req;
    EXPECT_EQ(j["method"], "roots/list");

    auto deserialized = j.get<mcp::ListRootsRequest>();
    EXPECT_EQ(deserialized.method, "roots/list");
}

TEST(ProtocolTest, ListRootsResultSerialization) {
    mcp::Root root1{.uri = "file:///home/user/project1", .name = "Project 1"};
    mcp::Root root2{.uri = "file:///home/user/project2"};

    mcp::ListRootsResult result;
    result.roots = {root1, root2};

    nlohmann::json j = result;
    EXPECT_EQ(j["roots"].size(), 2);
    EXPECT_EQ(j["roots"][0]["uri"], "file:///home/user/project1");
    EXPECT_EQ(j["roots"][0]["name"], "Project 1");
    EXPECT_EQ(j["roots"][1]["uri"], "file:///home/user/project2");
    EXPECT_FALSE(j["roots"][1].contains("name"));

    auto deserialized = j.get<mcp::ListRootsResult>();
    EXPECT_EQ(deserialized.roots.size(), 2);
    EXPECT_EQ(deserialized.roots[0].uri, "file:///home/user/project1");
    EXPECT_EQ(deserialized.roots[0].name, "Project 1");
    EXPECT_EQ(deserialized.roots[1].uri, "file:///home/user/project2");
    EXPECT_FALSE(deserialized.roots[1].name.has_value());
}

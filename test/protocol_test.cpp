#include "mcp/protocol.hpp"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

TEST(ProtocolTest, InitializeRequestSerialization) {
    mcp::InitializeRequest req;
    req.protocolVersion = std::string(mcp::LATEST_PROTOCOL_VERSION);
    req.clientInfo = {"test-client", "1.0.0"};
    req.capabilities = mcp::ClientCapabilities{};

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
    bool has_2024_11_05 = false;
    bool has_2025_03_26 = false;
    bool has_2025_06_18 = false;
    bool has_2025_11_25 = false;

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
        if (version == "2025-11-25") {
            has_2025_11_25 = true;
        }
    }

    EXPECT_TRUE(has_2024_11_05) << "Protocol version 2024-11-05 not found in supported versions";
    EXPECT_TRUE(has_2025_03_26) << "Protocol version 2025-03-26 not found in supported versions";
    EXPECT_TRUE(has_2025_06_18) << "Protocol version 2025-06-18 not found in supported versions";
    EXPECT_TRUE(has_2025_11_25) << "Protocol version 2025-11-25 not found in supported versions";
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
    mcp::PromptReference pref;
    pref.type = "ref/prompt";
    pref.name = "my_prompt";
    mcp::CompleteReference ref = pref;

    json json_ref = ref;
    EXPECT_EQ(json_ref["type"], "ref/prompt");
    EXPECT_EQ(json_ref["name"], "my_prompt");

    mcp::CompleteReference deserialized_ref = json_ref.get<mcp::CompleteReference>();
    EXPECT_EQ(std::get<mcp::PromptReference>(deserialized_ref).type, "ref/prompt");
    EXPECT_EQ(std::get<mcp::PromptReference>(deserialized_ref).name, "my_prompt");

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

    auto deserialized_params = json_params.get<mcp::CompleteParams>();
    EXPECT_EQ(std::get<mcp::PromptReference>(deserialized_params.ref).type, "ref/prompt");
    EXPECT_EQ(deserialized_params.argument.name, "arg");
    ASSERT_TRUE(deserialized_params.context.has_value());
    ASSERT_TRUE(deserialized_params.context->arguments.has_value());
    EXPECT_EQ((*deserialized_params.context->arguments)["key"], "value");

    // Test CompletionResultDetails
    mcp::CompletionResultDetails details;
    details.values = {"val1", "val2"};
    details.total = 10;
    details.hasMore = false;

    // Test CompleteResult
    mcp::CompleteResult res;
    res.completion = details;
    res.meta = json{{"requestId", "req123"}};

    json json_res = res;
    EXPECT_EQ(json_res["completion"]["values"][0], "val1");
    EXPECT_EQ(json_res["completion"]["total"], 10);
    EXPECT_EQ(json_res["_meta"]["requestId"], "req123");

    auto deserialized_res = json_res.get<mcp::CompleteResult>();
    EXPECT_EQ(deserialized_res.completion.values[1], "val2");
    EXPECT_EQ(deserialized_res.completion.total, 10);
    EXPECT_EQ(deserialized_res.meta.value()["requestId"], "req123");
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
    content.content.push_back(nlohmann::json(mcp::TextContent{.text = "72°F, sunny"}));
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
    EXPECT_EQ(deserialized.content[0]["type"], "text");
    EXPECT_EQ(deserialized.content[0]["text"], "72°F, sunny");
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
    tool_result.content.push_back(nlohmann::json(mcp::TextContent{.text = "42"}));

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

// ============================================================================
// C-09: Task & Elicitation Primitives
// ============================================================================

TEST(ProtocolTest, TaskStatusSerialization) {
    // Verify all enum values round-trip correctly
    nlohmann::json j;

    j = mcp::TaskStatus::Cancelled;
    EXPECT_EQ(j.get<std::string>(), "cancelled");
    EXPECT_EQ(j.get<mcp::TaskStatus>(), mcp::TaskStatus::Cancelled);

    j = mcp::TaskStatus::Completed;
    EXPECT_EQ(j.get<std::string>(), "completed");
    EXPECT_EQ(j.get<mcp::TaskStatus>(), mcp::TaskStatus::Completed);

    j = mcp::TaskStatus::Failed;
    EXPECT_EQ(j.get<std::string>(), "failed");
    EXPECT_EQ(j.get<mcp::TaskStatus>(), mcp::TaskStatus::Failed);

    j = mcp::TaskStatus::InputRequired;
    EXPECT_EQ(j.get<std::string>(), "input_required");
    EXPECT_EQ(j.get<mcp::TaskStatus>(), mcp::TaskStatus::InputRequired);

    j = mcp::TaskStatus::Working;
    EXPECT_EQ(j.get<std::string>(), "working");
    EXPECT_EQ(j.get<mcp::TaskStatus>(), mcp::TaskStatus::Working);
}

TEST(ProtocolTest, TaskMetadataSerialization) {
    // With ttl
    mcp::TaskMetadata meta;
    meta.ttl = 60000;

    nlohmann::json j = meta;
    EXPECT_EQ(j["ttl"], 60000);

    auto deserialized = j.get<mcp::TaskMetadata>();
    ASSERT_TRUE(deserialized.ttl.has_value());
    EXPECT_EQ(*deserialized.ttl, 60000);

    // Without ttl (empty object)
    mcp::TaskMetadata empty_meta;
    nlohmann::json j_empty = empty_meta;
    EXPECT_FALSE(j_empty.contains("ttl"));

    auto deserialized_empty = j_empty.get<mcp::TaskMetadata>();
    EXPECT_FALSE(deserialized_empty.ttl.has_value());
}

TEST(ProtocolTest, TaskDataSerialization) {
    // Full TaskData with all optional fields
    mcp::TaskData task;
    task.taskId = "task-123";
    task.status = mcp::TaskStatus::Working;
    task.createdAt = "2025-01-15T10:30:00Z";
    task.lastUpdatedAt = "2025-01-15T10:35:00Z";
    task.ttl = 300000;
    task.pollInterval = 5000;
    task.statusMessage = "Processing step 2 of 5";

    nlohmann::json j = task;
    EXPECT_EQ(j["taskId"], "task-123");
    EXPECT_EQ(j["status"], "working");
    EXPECT_EQ(j["createdAt"], "2025-01-15T10:30:00Z");
    EXPECT_EQ(j["lastUpdatedAt"], "2025-01-15T10:35:00Z");
    EXPECT_EQ(j["ttl"], 300000);
    EXPECT_EQ(j["pollInterval"], 5000);
    EXPECT_EQ(j["statusMessage"], "Processing step 2 of 5");

    auto deserialized = j.get<mcp::TaskData>();
    EXPECT_EQ(deserialized.taskId, "task-123");
    EXPECT_EQ(deserialized.status, mcp::TaskStatus::Working);
    EXPECT_EQ(deserialized.createdAt, "2025-01-15T10:30:00Z");
    EXPECT_EQ(deserialized.lastUpdatedAt, "2025-01-15T10:35:00Z");
    EXPECT_EQ(deserialized.ttl, 300000);
    ASSERT_TRUE(deserialized.pollInterval.has_value());
    EXPECT_EQ(*deserialized.pollInterval, 5000);
    ASSERT_TRUE(deserialized.statusMessage.has_value());
    EXPECT_EQ(*deserialized.statusMessage, "Processing step 2 of 5");

    // Minimal TaskData (required fields only)
    mcp::TaskData minimal;
    minimal.taskId = "task-min";
    minimal.status = mcp::TaskStatus::Completed;
    minimal.createdAt = "2025-01-15T10:30:00Z";
    minimal.lastUpdatedAt = "2025-01-15T10:30:00Z";
    minimal.ttl = 0;

    nlohmann::json j_min = minimal;
    EXPECT_EQ(j_min["taskId"], "task-min");
    EXPECT_EQ(j_min["status"], "completed");
    EXPECT_FALSE(j_min.contains("pollInterval"));
    EXPECT_FALSE(j_min.contains("statusMessage"));

    auto deserialized_min = j_min.get<mcp::TaskData>();
    EXPECT_EQ(deserialized_min.taskId, "task-min");
    EXPECT_FALSE(deserialized_min.pollInterval.has_value());
    EXPECT_FALSE(deserialized_min.statusMessage.has_value());
}

TEST(ProtocolTest, CreateTaskResultSerialization) {
    mcp::TaskData task;
    task.taskId = "task-456";
    task.status = mcp::TaskStatus::InputRequired;
    task.createdAt = "2025-01-15T10:30:00Z";
    task.lastUpdatedAt = "2025-01-15T10:31:00Z";
    task.ttl = 60000;

    mcp::CreateTaskResult result;
    result.task = std::move(task);
    result.meta = nlohmann::json{{"requestId", "req-1"}};

    nlohmann::json j = result;
    EXPECT_EQ(j["task"]["taskId"], "task-456");
    EXPECT_EQ(j["task"]["status"], "input_required");
    EXPECT_EQ(j["_meta"]["requestId"], "req-1");

    auto deserialized = j.get<mcp::CreateTaskResult>();
    EXPECT_EQ(deserialized.task.taskId, "task-456");
    EXPECT_EQ(deserialized.task.status, mcp::TaskStatus::InputRequired);
    ASSERT_TRUE(deserialized.meta.has_value());
    EXPECT_EQ((*deserialized.meta)["requestId"], "req-1");

    // Without _meta
    mcp::CreateTaskResult no_meta;
    no_meta.task.taskId = "task-789";
    no_meta.task.status = mcp::TaskStatus::Completed;
    no_meta.task.createdAt = "2025-01-15T10:30:00Z";
    no_meta.task.lastUpdatedAt = "2025-01-15T10:30:00Z";
    no_meta.task.ttl = 0;

    nlohmann::json j2 = no_meta;
    EXPECT_FALSE(j2.contains("_meta"));

    auto deserialized2 = j2.get<mcp::CreateTaskResult>();
    EXPECT_FALSE(deserialized2.meta.has_value());
}

TEST(ProtocolTest, GetTaskResultSerialization) {
    mcp::GetTaskResult result;
    result.taskId = "task-get-1";
    result.status = mcp::TaskStatus::Failed;
    result.createdAt = "2025-01-15T10:30:00Z";
    result.lastUpdatedAt = "2025-01-15T10:32:00Z";
    result.ttl = 120000;
    result.pollInterval = 10000;
    result.statusMessage = "Timeout exceeded";
    result.meta = nlohmann::json{{"trace", "abc"}};

    nlohmann::json j = result;
    EXPECT_EQ(j["taskId"], "task-get-1");
    EXPECT_EQ(j["status"], "failed");
    EXPECT_EQ(j["createdAt"], "2025-01-15T10:30:00Z");
    EXPECT_EQ(j["lastUpdatedAt"], "2025-01-15T10:32:00Z");
    EXPECT_EQ(j["ttl"], 120000);
    EXPECT_EQ(j["pollInterval"], 10000);
    EXPECT_EQ(j["statusMessage"], "Timeout exceeded");
    EXPECT_EQ(j["_meta"]["trace"], "abc");

    auto deserialized = j.get<mcp::GetTaskResult>();
    EXPECT_EQ(deserialized.taskId, "task-get-1");
    EXPECT_EQ(deserialized.status, mcp::TaskStatus::Failed);
    EXPECT_EQ(deserialized.ttl, 120000);
    ASSERT_TRUE(deserialized.pollInterval.has_value());
    EXPECT_EQ(*deserialized.pollInterval, 10000);
    ASSERT_TRUE(deserialized.statusMessage.has_value());
    EXPECT_EQ(*deserialized.statusMessage, "Timeout exceeded");
    ASSERT_TRUE(deserialized.meta.has_value());
    EXPECT_EQ((*deserialized.meta)["trace"], "abc");

    // Minimal GetTaskResult
    mcp::GetTaskResult minimal;
    minimal.taskId = "task-get-min";
    minimal.status = mcp::TaskStatus::Cancelled;
    minimal.createdAt = "2025-01-15T10:30:00Z";
    minimal.lastUpdatedAt = "2025-01-15T10:30:00Z";
    minimal.ttl = 0;

    nlohmann::json j_min = minimal;
    EXPECT_FALSE(j_min.contains("pollInterval"));
    EXPECT_FALSE(j_min.contains("statusMessage"));
    EXPECT_FALSE(j_min.contains("_meta"));

    auto deserialized_min = j_min.get<mcp::GetTaskResult>();
    EXPECT_FALSE(deserialized_min.pollInterval.has_value());
    EXPECT_FALSE(deserialized_min.statusMessage.has_value());
    EXPECT_FALSE(deserialized_min.meta.has_value());
}

TEST(ProtocolTest, ListTasksResultSerialization) {
    mcp::TaskData task1;
    task1.taskId = "t1";
    task1.status = mcp::TaskStatus::Completed;
    task1.createdAt = "2025-01-15T10:00:00Z";
    task1.lastUpdatedAt = "2025-01-15T10:01:00Z";
    task1.ttl = 60000;

    mcp::TaskData task2;
    task2.taskId = "t2";
    task2.status = mcp::TaskStatus::Working;
    task2.createdAt = "2025-01-15T10:02:00Z";
    task2.lastUpdatedAt = "2025-01-15T10:03:00Z";
    task2.ttl = 120000;

    mcp::ListTasksResult result;
    result.tasks = {task1, task2};
    result.nextCursor = "cursor-abc";
    result.meta = nlohmann::json{{"page", 1}};

    nlohmann::json j = result;
    EXPECT_EQ(j["tasks"].size(), 2);
    EXPECT_EQ(j["tasks"][0]["taskId"], "t1");
    EXPECT_EQ(j["tasks"][1]["taskId"], "t2");
    EXPECT_EQ(j["nextCursor"], "cursor-abc");
    EXPECT_EQ(j["_meta"]["page"], 1);

    auto deserialized = j.get<mcp::ListTasksResult>();
    EXPECT_EQ(deserialized.tasks.size(), 2);
    EXPECT_EQ(deserialized.tasks[0].taskId, "t1");
    EXPECT_EQ(deserialized.tasks[0].status, mcp::TaskStatus::Completed);
    EXPECT_EQ(deserialized.tasks[1].taskId, "t2");
    EXPECT_EQ(deserialized.tasks[1].status, mcp::TaskStatus::Working);
    ASSERT_TRUE(deserialized.nextCursor.has_value());
    EXPECT_EQ(*deserialized.nextCursor, "cursor-abc");
    ASSERT_TRUE(deserialized.meta.has_value());

    // Without pagination and meta
    mcp::ListTasksResult no_cursor;
    no_cursor.tasks = {task1};

    nlohmann::json j2 = no_cursor;
    EXPECT_FALSE(j2.contains("nextCursor"));
    EXPECT_FALSE(j2.contains("_meta"));

    auto deserialized2 = j2.get<mcp::ListTasksResult>();
    EXPECT_EQ(deserialized2.tasks.size(), 1);
    EXPECT_FALSE(deserialized2.nextCursor.has_value());
    EXPECT_FALSE(deserialized2.meta.has_value());
}

TEST(ProtocolTest, ElicitRequestURLParamsSerialization) {
    mcp::ElicitRequestURLParams params;
    params.message = "Please authenticate";
    params.url = "https://auth.example.com/login";
    params.elicitationId = "elicit-url-1";
    params.task = mcp::TaskMetadata{.ttl = 30000};
    params.meta = nlohmann::json{{"source", "server"}};

    nlohmann::json j = params;
    EXPECT_EQ(j["mode"], "url");
    EXPECT_EQ(j["message"], "Please authenticate");
    EXPECT_EQ(j["url"], "https://auth.example.com/login");
    EXPECT_EQ(j["elicitationId"], "elicit-url-1");
    EXPECT_EQ(j["task"]["ttl"], 30000);
    EXPECT_EQ(j["_meta"]["source"], "server");

    auto deserialized = j.get<mcp::ElicitRequestURLParams>();
    EXPECT_EQ(deserialized.mode, "url");
    EXPECT_EQ(deserialized.message, "Please authenticate");
    EXPECT_EQ(deserialized.url, "https://auth.example.com/login");
    EXPECT_EQ(deserialized.elicitationId, "elicit-url-1");
    ASSERT_TRUE(deserialized.task.has_value());
    ASSERT_TRUE(deserialized.task->ttl.has_value());
    EXPECT_EQ(*deserialized.task->ttl, 30000);
    ASSERT_TRUE(deserialized.meta.has_value());

    // Minimal (no task, no _meta)
    mcp::ElicitRequestURLParams minimal;
    minimal.message = "Go here";
    minimal.url = "https://example.com";
    minimal.elicitationId = "elicit-url-2";

    nlohmann::json j_min = minimal;
    EXPECT_EQ(j_min["mode"], "url");
    EXPECT_FALSE(j_min.contains("task"));
    EXPECT_FALSE(j_min.contains("_meta"));

    auto deserialized_min = j_min.get<mcp::ElicitRequestURLParams>();
    EXPECT_FALSE(deserialized_min.task.has_value());
    EXPECT_FALSE(deserialized_min.meta.has_value());
}

TEST(ProtocolTest, ElicitRequestFormParamsSerialization) {
    nlohmann::json schema = {
        {"type", "object"},
        {"properties", {{"name", {{"type", "string"}}}, {"age", {{"type", "integer"}}}}},
        {"required", {"name"}}};

    mcp::ElicitRequestFormParams params;
    params.message = "Please provide your details";
    params.requestedSchema = schema;
    params.task = mcp::TaskMetadata{.ttl = 60000};

    nlohmann::json j = params;
    EXPECT_EQ(j["mode"], "form");
    EXPECT_EQ(j["message"], "Please provide your details");
    EXPECT_EQ(j["requestedSchema"]["type"], "object");
    EXPECT_EQ(j["requestedSchema"]["required"][0], "name");
    EXPECT_EQ(j["task"]["ttl"], 60000);
    EXPECT_FALSE(j.contains("_meta"));

    auto deserialized = j.get<mcp::ElicitRequestFormParams>();
    EXPECT_EQ(deserialized.mode, "form");
    EXPECT_EQ(deserialized.message, "Please provide your details");
    EXPECT_EQ(deserialized.requestedSchema["type"], "object");
    ASSERT_TRUE(deserialized.task.has_value());
    ASSERT_TRUE(deserialized.task->ttl.has_value());
    EXPECT_EQ(*deserialized.task->ttl, 60000);
    EXPECT_FALSE(deserialized.meta.has_value());
}

TEST(ProtocolTest, ElicitRequestParamsVariantDispatch) {
    // URL mode round-trip via variant
    mcp::ElicitRequestURLParams url_params;
    url_params.message = "Click here";
    url_params.url = "https://example.com/auth";
    url_params.elicitationId = "elicit-v-1";

    mcp::ElicitRequestParams variant_url = url_params;
    nlohmann::json j_url = variant_url;
    EXPECT_EQ(j_url["mode"], "url");
    EXPECT_EQ(j_url["url"], "https://example.com/auth");

    auto deserialized_url = j_url.get<mcp::ElicitRequestParams>();
    ASSERT_TRUE(std::holds_alternative<mcp::ElicitRequestURLParams>(deserialized_url));
    auto& got_url = std::get<mcp::ElicitRequestURLParams>(deserialized_url);
    EXPECT_EQ(got_url.message, "Click here");
    EXPECT_EQ(got_url.url, "https://example.com/auth");
    EXPECT_EQ(got_url.elicitationId, "elicit-v-1");

    // Form mode round-trip via variant
    mcp::ElicitRequestFormParams form_params;
    form_params.message = "Fill this";
    form_params.requestedSchema = nlohmann::json{{"type", "object"}};

    mcp::ElicitRequestParams variant_form = form_params;
    nlohmann::json j_form = variant_form;
    EXPECT_EQ(j_form["mode"], "form");
    EXPECT_EQ(j_form["requestedSchema"]["type"], "object");

    auto deserialized_form = j_form.get<mcp::ElicitRequestParams>();
    ASSERT_TRUE(std::holds_alternative<mcp::ElicitRequestFormParams>(deserialized_form));
    auto& got_form = std::get<mcp::ElicitRequestFormParams>(deserialized_form);
    EXPECT_EQ(got_form.message, "Fill this");

    // Unknown mode throws
    nlohmann::json j_bad = {{"mode", "unknown"}, {"message", "x"}};
    EXPECT_THROW(j_bad.get<mcp::ElicitRequestParams>(), std::invalid_argument);
}

TEST(ProtocolTest, ElicitRequestSerialization) {
    mcp::ElicitRequestURLParams url_params;
    url_params.message = "Please visit";
    url_params.url = "https://example.com";
    url_params.elicitationId = "elicit-req-1";

    mcp::ElicitRequest req;
    req.params = url_params;

    nlohmann::json j = req;
    EXPECT_EQ(j["method"], "elicitation/create");
    EXPECT_EQ(j["params"]["mode"], "url");
    EXPECT_EQ(j["params"]["url"], "https://example.com");

    auto deserialized = j.get<mcp::ElicitRequest>();
    EXPECT_EQ(deserialized.method, "elicitation/create");
    ASSERT_TRUE(std::holds_alternative<mcp::ElicitRequestURLParams>(deserialized.params));
    auto& got = std::get<mcp::ElicitRequestURLParams>(deserialized.params);
    EXPECT_EQ(got.url, "https://example.com");
}

TEST(ProtocolTest, ElicitActionSerialization) {
    nlohmann::json j;

    j = mcp::ElicitAction::Accept;
    EXPECT_EQ(j.get<std::string>(), "accept");
    EXPECT_EQ(j.get<mcp::ElicitAction>(), mcp::ElicitAction::Accept);

    j = mcp::ElicitAction::Decline;
    EXPECT_EQ(j.get<std::string>(), "decline");
    EXPECT_EQ(j.get<mcp::ElicitAction>(), mcp::ElicitAction::Decline);

    j = mcp::ElicitAction::Cancel;
    EXPECT_EQ(j.get<std::string>(), "cancel");
    EXPECT_EQ(j.get<mcp::ElicitAction>(), mcp::ElicitAction::Cancel);
}

TEST(ProtocolTest, ElicitResultSerialization) {
    // Accept with content
    mcp::ElicitResult result;
    result.action = mcp::ElicitAction::Accept;
    result.content = nlohmann::json{{"name", "Alice"}, {"age", 30}};
    result.meta = nlohmann::json{{"requestId", "r-1"}};

    nlohmann::json j = result;
    EXPECT_EQ(j["action"], "accept");
    EXPECT_EQ(j["content"]["name"], "Alice");
    EXPECT_EQ(j["content"]["age"], 30);
    EXPECT_EQ(j["_meta"]["requestId"], "r-1");

    auto deserialized = j.get<mcp::ElicitResult>();
    EXPECT_EQ(deserialized.action, mcp::ElicitAction::Accept);
    ASSERT_TRUE(deserialized.content.has_value());
    EXPECT_EQ((*deserialized.content)["name"], "Alice");
    EXPECT_EQ((*deserialized.content)["age"], 30);
    ASSERT_TRUE(deserialized.meta.has_value());

    // Decline without content
    mcp::ElicitResult decline;
    decline.action = mcp::ElicitAction::Decline;

    nlohmann::json j2 = decline;
    EXPECT_EQ(j2["action"], "decline");
    EXPECT_FALSE(j2.contains("content"));
    EXPECT_FALSE(j2.contains("_meta"));

    auto deserialized2 = j2.get<mcp::ElicitResult>();
    EXPECT_EQ(deserialized2.action, mcp::ElicitAction::Decline);
    EXPECT_FALSE(deserialized2.content.has_value());
    EXPECT_FALSE(deserialized2.meta.has_value());

    // Cancel without content
    mcp::ElicitResult cancel;
    cancel.action = mcp::ElicitAction::Cancel;

    nlohmann::json j3 = cancel;
    EXPECT_EQ(j3["action"], "cancel");
    EXPECT_FALSE(j3.contains("content"));

    auto deserialized3 = j3.get<mcp::ElicitResult>();
    EXPECT_EQ(deserialized3.action, mcp::ElicitAction::Cancel);
    EXPECT_FALSE(deserialized3.content.has_value());
}

// ============================================================================
// C-10: JSON-RPC Base Types
// ============================================================================

TEST(ProtocolTest, RequestIdStringSerialization) {
    mcp::RequestId id = std::string("req-42");

    nlohmann::json j = id;
    EXPECT_TRUE(j.is_string());
    EXPECT_EQ(j.get<std::string>(), "req-42");

    auto deserialized = j.get<mcp::RequestId>();
    ASSERT_TRUE(std::holds_alternative<std::string>(deserialized));
    EXPECT_EQ(std::get<std::string>(deserialized), "req-42");
}

TEST(ProtocolTest, RequestIdIntegerSerialization) {
    mcp::RequestId id = int64_t{7};

    nlohmann::json j = id;
    EXPECT_TRUE(j.is_number_integer());
    EXPECT_EQ(j.get<int64_t>(), 7);

    auto deserialized = j.get<mcp::RequestId>();
    ASSERT_TRUE(std::holds_alternative<int64_t>(deserialized));
    EXPECT_EQ(std::get<int64_t>(deserialized), 7);
}

TEST(ProtocolTest, RequestIdInvalidTypeThrows) {
    nlohmann::json j = 3.14;
    EXPECT_THROW(j.get<mcp::RequestId>(), std::invalid_argument);

    nlohmann::json j2 = true;
    EXPECT_THROW(j2.get<mcp::RequestId>(), std::invalid_argument);
}

TEST(ProtocolTest, ProgressTokenIsRequestId) {
    static_assert(std::is_same_v<mcp::ProgressToken, mcp::RequestId>);

    mcp::ProgressToken token = std::string("progress-1");
    nlohmann::json j = token;
    EXPECT_EQ(j.get<std::string>(), "progress-1");

    mcp::ProgressToken int_token = int64_t{99};
    nlohmann::json j2 = int_token;
    EXPECT_EQ(j2.get<int64_t>(), 99);
}

TEST(ProtocolTest, ErrorSerialization) {
    mcp::Error error;
    error.code = -32600;
    error.message = "Invalid Request";
    error.data = nlohmann::json{{"detail", "missing method"}};

    nlohmann::json j = error;
    EXPECT_EQ(j["code"], -32600);
    EXPECT_EQ(j["message"], "Invalid Request");
    EXPECT_EQ(j["data"]["detail"], "missing method");

    auto deserialized = j.get<mcp::Error>();
    EXPECT_EQ(deserialized.code, -32600);
    EXPECT_EQ(deserialized.message, "Invalid Request");
    ASSERT_TRUE(deserialized.data.has_value());
    EXPECT_EQ((*deserialized.data)["detail"], "missing method");

    mcp::Error minimal;
    minimal.code = -32601;
    minimal.message = "Method not found";

    nlohmann::json j2 = minimal;
    EXPECT_FALSE(j2.contains("data"));

    auto deserialized2 = j2.get<mcp::Error>();
    EXPECT_EQ(deserialized2.code, -32601);
    EXPECT_FALSE(deserialized2.data.has_value());
}

TEST(ProtocolTest, JSONRPCRequestSerialization) {
    mcp::JSONRPCRequest req;
    req.id = std::string("req-1");
    req.method = "tools/list";
    req.params = nlohmann::json{{"cursor", "abc"}};

    nlohmann::json j = req;
    EXPECT_EQ(j["jsonrpc"], "2.0");
    EXPECT_EQ(j["id"], "req-1");
    EXPECT_EQ(j["method"], "tools/list");
    EXPECT_EQ(j["params"]["cursor"], "abc");

    auto deserialized = j.get<mcp::JSONRPCRequest>();
    EXPECT_EQ(deserialized.jsonrpc, "2.0");
    ASSERT_TRUE(std::holds_alternative<std::string>(deserialized.id));
    EXPECT_EQ(std::get<std::string>(deserialized.id), "req-1");
    EXPECT_EQ(deserialized.method, "tools/list");
    ASSERT_TRUE(deserialized.params.has_value());
    EXPECT_EQ((*deserialized.params)["cursor"], "abc");

    // Integer id, no params
    mcp::JSONRPCRequest int_req;
    int_req.id = int64_t{42};
    int_req.method = "ping";

    nlohmann::json j2 = int_req;
    EXPECT_EQ(j2["id"], 42);
    EXPECT_FALSE(j2.contains("params"));

    auto deserialized2 = j2.get<mcp::JSONRPCRequest>();
    ASSERT_TRUE(std::holds_alternative<int64_t>(deserialized2.id));
    EXPECT_EQ(std::get<int64_t>(deserialized2.id), 42);
    EXPECT_FALSE(deserialized2.params.has_value());
}

TEST(ProtocolTest, JSONRPCNotificationSerialization) {
    mcp::JSONRPCNotification notif;
    notif.method = "notifications/progress";
    notif.params = nlohmann::json{{"progressToken", "tok-1"}, {"progress", 50}};

    nlohmann::json j = notif;
    EXPECT_EQ(j["jsonrpc"], "2.0");
    EXPECT_EQ(j["method"], "notifications/progress");
    EXPECT_EQ(j["params"]["progress"], 50);

    auto deserialized = j.get<mcp::JSONRPCNotification>();
    EXPECT_EQ(deserialized.jsonrpc, "2.0");
    EXPECT_EQ(deserialized.method, "notifications/progress");
    ASSERT_TRUE(deserialized.params.has_value());

    // No params
    mcp::JSONRPCNotification minimal;
    minimal.method = "notifications/cancelled";

    nlohmann::json j2 = minimal;
    EXPECT_FALSE(j2.contains("params"));

    auto deserialized2 = j2.get<mcp::JSONRPCNotification>();
    EXPECT_EQ(deserialized2.method, "notifications/cancelled");
    EXPECT_FALSE(deserialized2.params.has_value());
}

TEST(ProtocolTest, JSONRPCResultResponseSerialization) {
    mcp::JSONRPCResultResponse resp;
    resp.id = std::string("resp-1");
    resp.result = nlohmann::json{{"tools", nlohmann::json::array()}};

    nlohmann::json j = resp;
    EXPECT_EQ(j["jsonrpc"], "2.0");
    EXPECT_EQ(j["id"], "resp-1");
    EXPECT_TRUE(j["result"]["tools"].is_array());

    auto deserialized = j.get<mcp::JSONRPCResultResponse>();
    ASSERT_TRUE(std::holds_alternative<std::string>(deserialized.id));
    EXPECT_EQ(std::get<std::string>(deserialized.id), "resp-1");
    EXPECT_TRUE(deserialized.result.contains("tools"));

    // Integer id
    mcp::JSONRPCResultResponse int_resp;
    int_resp.id = int64_t{10};
    int_resp.result = nlohmann::json::object();

    nlohmann::json j2 = int_resp;
    EXPECT_EQ(j2["id"], 10);

    auto deserialized2 = j2.get<mcp::JSONRPCResultResponse>();
    ASSERT_TRUE(std::holds_alternative<int64_t>(deserialized2.id));
    EXPECT_EQ(std::get<int64_t>(deserialized2.id), 10);
}

TEST(ProtocolTest, JSONRPCErrorResponseSerialization) {
    mcp::JSONRPCErrorResponse resp;
    resp.error = {.code = -32700, .message = "Parse error"};
    resp.id = mcp::RequestId{std::string("err-1")};

    nlohmann::json j = resp;
    EXPECT_EQ(j["jsonrpc"], "2.0");
    EXPECT_EQ(j["error"]["code"], -32700);
    EXPECT_EQ(j["error"]["message"], "Parse error");
    EXPECT_EQ(j["id"], "err-1");

    auto deserialized = j.get<mcp::JSONRPCErrorResponse>();
    EXPECT_EQ(deserialized.error.code, -32700);
    EXPECT_EQ(deserialized.error.message, "Parse error");
    ASSERT_TRUE(deserialized.id.has_value());
    ASSERT_TRUE(std::holds_alternative<std::string>(*deserialized.id));
    EXPECT_EQ(std::get<std::string>(*deserialized.id), "err-1");

    // No id (parse error before id was read)
    mcp::JSONRPCErrorResponse no_id;
    no_id.error = {.code = -32700, .message = "Parse error"};

    nlohmann::json j2 = no_id;
    EXPECT_FALSE(j2.contains("id"));

    auto deserialized2 = j2.get<mcp::JSONRPCErrorResponse>();
    EXPECT_FALSE(deserialized2.id.has_value());
}

TEST(ProtocolTest, JSONRPCResponseVariantDispatch) {
    // Success response
    nlohmann::json j_ok = {{"jsonrpc", "2.0"}, {"id", "r1"}, {"result", {{"status", "ok"}}}};
    auto resp_ok = j_ok.get<mcp::JSONRPCResponse>();
    ASSERT_TRUE(std::holds_alternative<mcp::JSONRPCResultResponse>(resp_ok));
    auto& ok = std::get<mcp::JSONRPCResultResponse>(resp_ok);
    EXPECT_EQ(ok.result["status"], "ok");

    // Error response
    nlohmann::json j_err = {
        {"jsonrpc", "2.0"}, {"id", 5}, {"error", {{"code", -32600}, {"message", "Invalid"}}}};
    auto resp_err = j_err.get<mcp::JSONRPCResponse>();
    ASSERT_TRUE(std::holds_alternative<mcp::JSONRPCErrorResponse>(resp_err));
    auto& err = std::get<mcp::JSONRPCErrorResponse>(resp_err);
    EXPECT_EQ(err.error.code, -32600);

    // Round-trip
    nlohmann::json j_rt = resp_ok;
    EXPECT_EQ(j_rt["result"]["status"], "ok");
}

TEST(ProtocolTest, JSONRPCMessageVariantDispatch) {
    // Request
    nlohmann::json j_req = {{"jsonrpc", "2.0"}, {"id", "m1"}, {"method", "tools/call"}};
    auto msg_req = j_req.get<mcp::JSONRPCMessage>();
    ASSERT_TRUE(std::holds_alternative<mcp::JSONRPCRequest>(msg_req));
    EXPECT_EQ(std::get<mcp::JSONRPCRequest>(msg_req).method, "tools/call");

    // Notification (no id)
    nlohmann::json j_notif = {{"jsonrpc", "2.0"}, {"method", "notifications/progress"}};
    auto msg_notif = j_notif.get<mcp::JSONRPCMessage>();
    ASSERT_TRUE(std::holds_alternative<mcp::JSONRPCNotification>(msg_notif));
    EXPECT_EQ(std::get<mcp::JSONRPCNotification>(msg_notif).method, "notifications/progress");

    // Result response
    nlohmann::json j_result = {{"jsonrpc", "2.0"}, {"id", 1}, {"result", nlohmann::json::object()}};
    auto msg_result = j_result.get<mcp::JSONRPCMessage>();
    ASSERT_TRUE(std::holds_alternative<mcp::JSONRPCResultResponse>(msg_result));

    // Error response
    nlohmann::json j_error = {{"jsonrpc", "2.0"},
                              {"error", {{"code", -32601}, {"message", "Method not found"}}}};
    auto msg_error = j_error.get<mcp::JSONRPCMessage>();
    ASSERT_TRUE(std::holds_alternative<mcp::JSONRPCErrorResponse>(msg_error));

    // Round-trip each
    nlohmann::json rt_req = msg_req;
    EXPECT_EQ(rt_req["method"], "tools/call");

    nlohmann::json rt_notif = msg_notif;
    EXPECT_EQ(rt_notif["method"], "notifications/progress");
}

TEST(ProtocolTest, LoggingLevelSerialization) {
    nlohmann::json j;

    j = mcp::LoggingLevel::Emergency;
    EXPECT_EQ(j.get<std::string>(), "emergency");
    EXPECT_EQ(j.get<mcp::LoggingLevel>(), mcp::LoggingLevel::Emergency);

    j = mcp::LoggingLevel::Alert;
    EXPECT_EQ(j.get<std::string>(), "alert");
    EXPECT_EQ(j.get<mcp::LoggingLevel>(), mcp::LoggingLevel::Alert);

    j = mcp::LoggingLevel::Critical;
    EXPECT_EQ(j.get<std::string>(), "critical");
    EXPECT_EQ(j.get<mcp::LoggingLevel>(), mcp::LoggingLevel::Critical);

    j = mcp::LoggingLevel::Error;
    EXPECT_EQ(j.get<std::string>(), "error");
    EXPECT_EQ(j.get<mcp::LoggingLevel>(), mcp::LoggingLevel::Error);

    j = mcp::LoggingLevel::Warning;
    EXPECT_EQ(j.get<std::string>(), "warning");
    EXPECT_EQ(j.get<mcp::LoggingLevel>(), mcp::LoggingLevel::Warning);

    j = mcp::LoggingLevel::Notice;
    EXPECT_EQ(j.get<std::string>(), "notice");
    EXPECT_EQ(j.get<mcp::LoggingLevel>(), mcp::LoggingLevel::Notice);

    j = mcp::LoggingLevel::Info;
    EXPECT_EQ(j.get<std::string>(), "info");
    EXPECT_EQ(j.get<mcp::LoggingLevel>(), mcp::LoggingLevel::Info);

    j = mcp::LoggingLevel::Debug;
    EXPECT_EQ(j.get<std::string>(), "debug");
    EXPECT_EQ(j.get<mcp::LoggingLevel>(), mcp::LoggingLevel::Debug);
}

TEST(ProtocolTest, SetLevelRequestSerialization) {
    mcp::SetLevelRequest req;
    req.params.level = mcp::LoggingLevel::Warning;
    req.params.meta = nlohmann::json{{"progressToken", "tok-1"}};

    nlohmann::json j = req;
    EXPECT_EQ(j["method"], "logging/setLevel");
    EXPECT_EQ(j["params"]["level"], "warning");
    EXPECT_EQ(j["params"]["_meta"]["progressToken"], "tok-1");

    auto deserialized = j.get<mcp::SetLevelRequest>();
    EXPECT_EQ(deserialized.method, "logging/setLevel");
    EXPECT_EQ(deserialized.params.level, mcp::LoggingLevel::Warning);
    ASSERT_TRUE(deserialized.params.meta.has_value());

    // Without _meta
    mcp::SetLevelRequest minimal;
    minimal.params.level = mcp::LoggingLevel::Debug;

    nlohmann::json j2 = minimal;
    EXPECT_EQ(j2["params"]["level"], "debug");
    EXPECT_FALSE(j2["params"].contains("_meta"));

    auto deserialized2 = j2.get<mcp::SetLevelRequest>();
    EXPECT_EQ(deserialized2.params.level, mcp::LoggingLevel::Debug);
    EXPECT_FALSE(deserialized2.params.meta.has_value());
}

TEST(ProtocolTest, ImplementationDescriptionField) {
    mcp::Implementation impl;
    impl.name = "test-server";
    impl.version = "2.0.0";
    impl.description = "A test MCP server providing math tools";

    json j = impl;
    EXPECT_EQ(j["name"], "test-server");
    EXPECT_EQ(j["description"], "A test MCP server providing math tools");

    auto rt = j.get<mcp::Implementation>();
    EXPECT_EQ(rt.name, "test-server");
    ASSERT_TRUE(rt.description.has_value());
    EXPECT_EQ(*rt.description, "A test MCP server providing math tools");

    mcp::Implementation impl2;
    impl2.name = "minimal";
    impl2.version = "1.0";

    json j2 = impl2;
    EXPECT_FALSE(j2.contains("description"));

    auto rt2 = j2.get<mcp::Implementation>();
    EXPECT_FALSE(rt2.description.has_value());
}

TEST(ProtocolTest, LatestProtocolVersion) {
    EXPECT_EQ(mcp::LATEST_PROTOCOL_VERSION, "2025-11-25");
    EXPECT_EQ(mcp::PROTOCOL_VERSION_2025_11_25, "2025-11-25");
}

TEST(ProtocolTest, ClientCapabilitiesEmptySerialization) {
    mcp::ClientCapabilities caps{};

    json j = caps;
    EXPECT_TRUE(j.is_object());
    EXPECT_TRUE(j.empty());

    auto rt = j.get<mcp::ClientCapabilities>();
    EXPECT_FALSE(rt.elicitation.has_value());
    EXPECT_FALSE(rt.experimental.has_value());
    EXPECT_FALSE(rt.roots.has_value());
    EXPECT_FALSE(rt.sampling.has_value());
    EXPECT_FALSE(rt.tasks.has_value());
}

TEST(ProtocolTest, ClientCapabilitiesFullSerialization) {
    mcp::ClientCapabilities caps;
    caps.elicitation = mcp::ClientCapabilities::ElicitationCapability{
        .form = json::object(),
        .url = json::object(),
    };
    caps.experimental = json{{"customFeature", json::object()}};
    caps.roots = mcp::ClientCapabilities::RootsCapability{.listChanged = true};
    caps.sampling = mcp::ClientCapabilities::SamplingCapability{
        .context = json::object(),
        .tools = json::object(),
    };

    mcp::ClientCapabilities::TaskRequestsCapability::ElicitationTaskCapability elicit_task;
    elicit_task.create = json::object();
    mcp::ClientCapabilities::TaskRequestsCapability::SamplingTaskCapability sampling_task;
    sampling_task.createMessage = json::object();

    mcp::ClientCapabilities::TaskRequestsCapability task_reqs;
    task_reqs.elicitation = elicit_task;
    task_reqs.sampling = sampling_task;

    caps.tasks = mcp::ClientCapabilities::TasksCapability{
        .cancel = json::object(),
        .list = json::object(),
        .requests = task_reqs,
    };

    json j = caps;

    EXPECT_TRUE(j.contains("elicitation"));
    EXPECT_TRUE(j["elicitation"].contains("form"));
    EXPECT_TRUE(j["elicitation"].contains("url"));

    EXPECT_TRUE(j.contains("experimental"));
    EXPECT_TRUE(j["experimental"].contains("customFeature"));

    EXPECT_TRUE(j.contains("roots"));
    EXPECT_EQ(j["roots"]["listChanged"], true);

    EXPECT_TRUE(j.contains("sampling"));
    EXPECT_TRUE(j["sampling"].contains("context"));
    EXPECT_TRUE(j["sampling"].contains("tools"));

    EXPECT_TRUE(j.contains("tasks"));
    EXPECT_TRUE(j["tasks"].contains("cancel"));
    EXPECT_TRUE(j["tasks"].contains("list"));
    EXPECT_TRUE(j["tasks"].contains("requests"));
    EXPECT_TRUE(j["tasks"]["requests"].contains("elicitation"));
    EXPECT_TRUE(j["tasks"]["requests"]["elicitation"].contains("create"));
    EXPECT_TRUE(j["tasks"]["requests"].contains("sampling"));
    EXPECT_TRUE(j["tasks"]["requests"]["sampling"].contains("createMessage"));

    auto rt = j.get<mcp::ClientCapabilities>();
    ASSERT_TRUE(rt.elicitation.has_value());
    ASSERT_TRUE(rt.elicitation->form.has_value());
    ASSERT_TRUE(rt.elicitation->url.has_value());
    ASSERT_TRUE(rt.roots.has_value());
    EXPECT_EQ(*rt.roots->listChanged, true);
    ASSERT_TRUE(rt.sampling.has_value());
    ASSERT_TRUE(rt.sampling->context.has_value());
    ASSERT_TRUE(rt.sampling->tools.has_value());
    ASSERT_TRUE(rt.tasks.has_value());
    ASSERT_TRUE(rt.tasks->cancel.has_value());
    ASSERT_TRUE(rt.tasks->list.has_value());
    ASSERT_TRUE(rt.tasks->requests.has_value());
    ASSERT_TRUE(rt.tasks->requests->elicitation.has_value());
    ASSERT_TRUE(rt.tasks->requests->elicitation->create.has_value());
    ASSERT_TRUE(rt.tasks->requests->sampling.has_value());
    ASSERT_TRUE(rt.tasks->requests->sampling->createMessage.has_value());
}

TEST(ProtocolTest, ClientCapabilitiesPartialSerialization) {
    mcp::ClientCapabilities caps;
    caps.roots = mcp::ClientCapabilities::RootsCapability{.listChanged = false};
    caps.sampling = mcp::ClientCapabilities::SamplingCapability{};

    json j = caps;
    EXPECT_FALSE(j.contains("elicitation"));
    EXPECT_FALSE(j.contains("experimental"));
    EXPECT_FALSE(j.contains("tasks"));
    EXPECT_TRUE(j.contains("roots"));
    EXPECT_EQ(j["roots"]["listChanged"], false);
    EXPECT_TRUE(j.contains("sampling"));

    auto rt = j.get<mcp::ClientCapabilities>();
    EXPECT_FALSE(rt.elicitation.has_value());
    ASSERT_TRUE(rt.roots.has_value());
    EXPECT_EQ(*rt.roots->listChanged, false);
    ASSERT_TRUE(rt.sampling.has_value());
    EXPECT_FALSE(rt.sampling->context.has_value());
    EXPECT_FALSE(rt.sampling->tools.has_value());
}

TEST(ProtocolTest, ServerCapabilitiesEmptySerialization) {
    mcp::ServerCapabilities caps{};

    json j = caps;
    EXPECT_TRUE(j.is_object());
    EXPECT_TRUE(j.empty());

    auto rt = j.get<mcp::ServerCapabilities>();
    EXPECT_FALSE(rt.completions.has_value());
    EXPECT_FALSE(rt.experimental.has_value());
    EXPECT_FALSE(rt.logging.has_value());
    EXPECT_FALSE(rt.prompts.has_value());
    EXPECT_FALSE(rt.resources.has_value());
    EXPECT_FALSE(rt.tasks.has_value());
    EXPECT_FALSE(rt.tools.has_value());
}

TEST(ProtocolTest, ServerCapabilitiesFullSerialization) {
    mcp::ServerCapabilities caps;
    caps.completions = json::object();
    caps.experimental = json{{"beta", json::object()}};
    caps.logging = json::object();
    caps.prompts = mcp::ServerCapabilities::PromptsCapability{.listChanged = true};
    caps.resources = mcp::ServerCapabilities::ResourcesCapability{
        .listChanged = true,
        .subscribe = true,
    };

    mcp::ServerCapabilities::TaskRequestsCapability::ToolsTaskCapability tools_task;
    tools_task.call = json::object();

    mcp::ServerCapabilities::TaskRequestsCapability task_reqs;
    task_reqs.tools = tools_task;

    caps.tasks = mcp::ServerCapabilities::TasksCapability{
        .cancel = json::object(),
        .list = json::object(),
        .requests = task_reqs,
    };
    caps.tools = mcp::ServerCapabilities::ToolsCapability{.listChanged = true};

    json j = caps;

    EXPECT_TRUE(j.contains("completions"));
    EXPECT_TRUE(j.contains("experimental"));
    EXPECT_TRUE(j.contains("logging"));
    EXPECT_TRUE(j.contains("prompts"));
    EXPECT_EQ(j["prompts"]["listChanged"], true);
    EXPECT_TRUE(j.contains("resources"));
    EXPECT_EQ(j["resources"]["listChanged"], true);
    EXPECT_EQ(j["resources"]["subscribe"], true);
    EXPECT_TRUE(j.contains("tasks"));
    EXPECT_TRUE(j["tasks"].contains("cancel"));
    EXPECT_TRUE(j["tasks"].contains("list"));
    EXPECT_TRUE(j["tasks"]["requests"]["tools"]["call"].is_object());
    EXPECT_TRUE(j.contains("tools"));
    EXPECT_EQ(j["tools"]["listChanged"], true);

    auto rt = j.get<mcp::ServerCapabilities>();
    ASSERT_TRUE(rt.completions.has_value());
    ASSERT_TRUE(rt.logging.has_value());
    ASSERT_TRUE(rt.prompts.has_value());
    EXPECT_EQ(*rt.prompts->listChanged, true);
    ASSERT_TRUE(rt.resources.has_value());
    EXPECT_EQ(*rt.resources->listChanged, true);
    EXPECT_EQ(*rt.resources->subscribe, true);
    ASSERT_TRUE(rt.tasks.has_value());
    ASSERT_TRUE(rt.tasks->cancel.has_value());
    ASSERT_TRUE(rt.tasks->list.has_value());
    ASSERT_TRUE(rt.tasks->requests.has_value());
    ASSERT_TRUE(rt.tasks->requests->tools.has_value());
    ASSERT_TRUE(rt.tasks->requests->tools->call.has_value());
    ASSERT_TRUE(rt.tools.has_value());
    EXPECT_EQ(*rt.tools->listChanged, true);
}

TEST(ProtocolTest, ServerCapabilitiesPartialSerialization) {
    mcp::ServerCapabilities caps;
    caps.tools = mcp::ServerCapabilities::ToolsCapability{.listChanged = false};
    caps.prompts = mcp::ServerCapabilities::PromptsCapability{};

    json j = caps;
    EXPECT_FALSE(j.contains("completions"));
    EXPECT_FALSE(j.contains("experimental"));
    EXPECT_FALSE(j.contains("logging"));
    EXPECT_FALSE(j.contains("resources"));
    EXPECT_FALSE(j.contains("tasks"));
    EXPECT_TRUE(j.contains("tools"));
    EXPECT_EQ(j["tools"]["listChanged"], false);
    EXPECT_TRUE(j.contains("prompts"));

    auto rt = j.get<mcp::ServerCapabilities>();
    ASSERT_TRUE(rt.tools.has_value());
    EXPECT_EQ(*rt.tools->listChanged, false);
    ASSERT_TRUE(rt.prompts.has_value());
    EXPECT_FALSE(rt.prompts->listChanged.has_value());
}

TEST(ProtocolTest, InitializeRequestWithClientCapabilities) {
    mcp::InitializeRequest req;
    req.protocolVersion = std::string(mcp::LATEST_PROTOCOL_VERSION);
    req.clientInfo = {"my-client", "3.0.0"};
    req.clientInfo.description = "An advanced MCP client";
    req.capabilities.roots = mcp::ClientCapabilities::RootsCapability{.listChanged = true};
    req.capabilities.sampling = mcp::ClientCapabilities::SamplingCapability{
        .context = json::object(),
    };

    json j = req;
    EXPECT_EQ(j["protocolVersion"], "2025-11-25");
    EXPECT_EQ(j["clientInfo"]["name"], "my-client");
    EXPECT_EQ(j["clientInfo"]["description"], "An advanced MCP client");
    EXPECT_EQ(j["capabilities"]["roots"]["listChanged"], true);
    EXPECT_TRUE(j["capabilities"]["sampling"].contains("context"));
    EXPECT_FALSE(j["capabilities"].contains("elicitation"));
    EXPECT_FALSE(j["capabilities"].contains("tasks"));

    auto rt = j.get<mcp::InitializeRequest>();
    EXPECT_EQ(rt.protocolVersion, "2025-11-25");
    EXPECT_EQ(rt.clientInfo.name, "my-client");
    ASSERT_TRUE(rt.clientInfo.description.has_value());
    EXPECT_EQ(*rt.clientInfo.description, "An advanced MCP client");
    ASSERT_TRUE(rt.capabilities.roots.has_value());
    EXPECT_EQ(*rt.capabilities.roots->listChanged, true);
    ASSERT_TRUE(rt.capabilities.sampling.has_value());
    ASSERT_TRUE(rt.capabilities.sampling->context.has_value());
}

TEST(ProtocolTest, InitializeResultWithServerCapabilities) {
    mcp::InitializeResult res;
    res.protocolVersion = std::string(mcp::LATEST_PROTOCOL_VERSION);
    res.serverInfo = {"my-server", "1.0.0"};
    res.serverInfo.description = "A powerful MCP server";
    res.capabilities.tools = mcp::ServerCapabilities::ToolsCapability{.listChanged = true};
    res.capabilities.resources = mcp::ServerCapabilities::ResourcesCapability{
        .listChanged = true,
        .subscribe = false,
    };
    res.instructions = "Use the math tools for calculations.";

    json j = res;
    EXPECT_EQ(j["protocolVersion"], "2025-11-25");
    EXPECT_EQ(j["serverInfo"]["name"], "my-server");
    EXPECT_EQ(j["serverInfo"]["description"], "A powerful MCP server");
    EXPECT_EQ(j["capabilities"]["tools"]["listChanged"], true);
    EXPECT_EQ(j["capabilities"]["resources"]["listChanged"], true);
    EXPECT_EQ(j["capabilities"]["resources"]["subscribe"], false);
    EXPECT_EQ(j["instructions"], "Use the math tools for calculations.");

    auto rt = j.get<mcp::InitializeResult>();
    EXPECT_EQ(rt.protocolVersion, "2025-11-25");
    EXPECT_EQ(rt.serverInfo.name, "my-server");
    ASSERT_TRUE(rt.serverInfo.description.has_value());
    EXPECT_EQ(*rt.serverInfo.description, "A powerful MCP server");
    ASSERT_TRUE(rt.capabilities.tools.has_value());
    EXPECT_EQ(*rt.capabilities.tools->listChanged, true);
    ASSERT_TRUE(rt.capabilities.resources.has_value());
    EXPECT_EQ(*rt.capabilities.resources->listChanged, true);
    EXPECT_EQ(*rt.capabilities.resources->subscribe, false);
    ASSERT_TRUE(rt.instructions.has_value());
    EXPECT_EQ(*rt.instructions, "Use the math tools for calculations.");
}

TEST(ProtocolTest, ClientCapabilitiesFromRawJson) {
    json raw = R"({
        "elicitation": {"form": {}, "url": {}},
        "roots": {"listChanged": true},
        "sampling": {"context": {}, "tools": {}},
        "tasks": {
            "cancel": {},
            "list": {},
            "requests": {
                "elicitation": {"create": {}},
                "sampling": {"createMessage": {}}
            }
        }
    })"_json;

    auto caps = raw.get<mcp::ClientCapabilities>();
    ASSERT_TRUE(caps.elicitation.has_value());
    ASSERT_TRUE(caps.elicitation->form.has_value());
    ASSERT_TRUE(caps.elicitation->url.has_value());
    ASSERT_TRUE(caps.roots.has_value());
    EXPECT_EQ(*caps.roots->listChanged, true);
    ASSERT_TRUE(caps.sampling.has_value());
    ASSERT_TRUE(caps.tasks.has_value());
    ASSERT_TRUE(caps.tasks->requests.has_value());
    ASSERT_TRUE(caps.tasks->requests->elicitation.has_value());
    ASSERT_TRUE(caps.tasks->requests->elicitation->create.has_value());
    ASSERT_TRUE(caps.tasks->requests->sampling.has_value());
    ASSERT_TRUE(caps.tasks->requests->sampling->createMessage.has_value());

    json j2 = caps;
    EXPECT_EQ(j2, raw);
}

TEST(ProtocolTest, ServerCapabilitiesFromRawJson) {
    json raw = R"({
        "completions": {},
        "logging": {},
        "prompts": {"listChanged": true},
        "resources": {"listChanged": true, "subscribe": false},
        "tasks": {
            "cancel": {},
            "list": {},
            "requests": {
                "tools": {"call": {}}
            }
        },
        "tools": {"listChanged": false}
    })"_json;

    auto caps = raw.get<mcp::ServerCapabilities>();
    ASSERT_TRUE(caps.completions.has_value());
    ASSERT_TRUE(caps.logging.has_value());
    ASSERT_TRUE(caps.prompts.has_value());
    EXPECT_EQ(*caps.prompts->listChanged, true);
    ASSERT_TRUE(caps.resources.has_value());
    EXPECT_EQ(*caps.resources->listChanged, true);
    EXPECT_EQ(*caps.resources->subscribe, false);
    ASSERT_TRUE(caps.tasks.has_value());
    ASSERT_TRUE(caps.tasks->requests.has_value());
    ASSERT_TRUE(caps.tasks->requests->tools.has_value());
    ASSERT_TRUE(caps.tasks->requests->tools->call.has_value());
    ASSERT_TRUE(caps.tools.has_value());
    EXPECT_EQ(*caps.tools->listChanged, false);

    json j2 = caps;
    EXPECT_EQ(j2, raw);
}

TEST(ProtocolTest, PingRequestSerialization) {
    mcp::PingRequest req;
    json j = req;
    EXPECT_EQ(j["method"], "ping");

    auto deserialized = j.get<mcp::PingRequest>();
    EXPECT_EQ(deserialized.method, "ping");
}

TEST(ProtocolTest, CancelledNotificationSerialization) {
    mcp::CancelledNotification notif;
    notif.params.requestId = "req-1";
    notif.params.reason = "Timeout";

    json j = notif;
    EXPECT_EQ(j["method"], "notifications/cancelled");
    EXPECT_EQ(j["params"]["requestId"], "req-1");
    EXPECT_EQ(j["params"]["reason"], "Timeout");

    auto deserialized = j.get<mcp::CancelledNotification>();
    EXPECT_EQ(deserialized.method, "notifications/cancelled");
    EXPECT_EQ(std::get<std::string>(deserialized.params.requestId), "req-1");
    ASSERT_TRUE(deserialized.params.reason.has_value());
    EXPECT_EQ(*deserialized.params.reason, "Timeout");

    // Without reason
    mcp::CancelledNotification minimal;
    minimal.params.requestId = int64_t(42);

    json j2 = minimal;
    EXPECT_EQ(j2["params"]["requestId"], 42);
    EXPECT_FALSE(j2["params"].contains("reason"));

    auto deserialized2 = j2.get<mcp::CancelledNotification>();
    EXPECT_EQ(std::get<int64_t>(deserialized2.params.requestId), 42);
    EXPECT_FALSE(deserialized2.params.reason.has_value());
}

TEST(ProtocolTest, ProgressNotificationSerialization) {
    mcp::ProgressNotification notif;
    notif.params.progressToken = "tok-1";
    notif.params.progress = 50.5;
    notif.params.total = 100.0;
    notif.params.message = "Halfway there";

    json j = notif;
    EXPECT_EQ(j["method"], "notifications/progress");
    EXPECT_EQ(j["params"]["progressToken"], "tok-1");
    EXPECT_EQ(j["params"]["progress"], 50.5);
    EXPECT_EQ(j["params"]["total"], 100.0);
    EXPECT_EQ(j["params"]["message"], "Halfway there");

    auto deserialized = j.get<mcp::ProgressNotification>();
    EXPECT_EQ(deserialized.method, "notifications/progress");
    EXPECT_EQ(std::get<std::string>(deserialized.params.progressToken), "tok-1");
    EXPECT_DOUBLE_EQ(deserialized.params.progress, 50.5);
    ASSERT_TRUE(deserialized.params.total.has_value());
    EXPECT_DOUBLE_EQ(*deserialized.params.total, 100.0);
    ASSERT_TRUE(deserialized.params.message.has_value());
    EXPECT_EQ(*deserialized.params.message, "Halfway there");

    // Minimal
    mcp::ProgressNotification minimal;
    minimal.params.progressToken = int64_t(1);
    minimal.params.progress = 10.0;

    json j2 = minimal;
    EXPECT_EQ(j2["params"]["progress"], 10.0);
    EXPECT_FALSE(j2["params"].contains("total"));
    EXPECT_FALSE(j2["params"].contains("message"));

    auto deserialized2 = j2.get<mcp::ProgressNotification>();
    EXPECT_DOUBLE_EQ(deserialized2.params.progress, 10.0);
    EXPECT_FALSE(deserialized2.params.total.has_value());
    EXPECT_FALSE(deserialized2.params.message.has_value());
}

TEST(ProtocolTest, LoggingMessageNotificationSerialization) {
    mcp::LoggingMessageNotification notif;
    notif.params.level = mcp::LoggingLevel::Error;
    notif.params.logger = "sys-logger";
    notif.params.data = json{{"error_code", 500}};

    json j = notif;
    EXPECT_EQ(j["method"], "notifications/message");
    EXPECT_EQ(j["params"]["level"], "error");
    EXPECT_EQ(j["params"]["logger"], "sys-logger");
    EXPECT_EQ(j["params"]["data"]["error_code"], 500);

    auto deserialized = j.get<mcp::LoggingMessageNotification>();
    EXPECT_EQ(deserialized.method, "notifications/message");
    EXPECT_EQ(deserialized.params.level, mcp::LoggingLevel::Error);
    ASSERT_TRUE(deserialized.params.logger.has_value());
    EXPECT_EQ(*deserialized.params.logger, "sys-logger");
    EXPECT_EQ(deserialized.params.data["error_code"], 500);

    // Minimal
    mcp::LoggingMessageNotification minimal;
    minimal.params.level = mcp::LoggingLevel::Info;
    minimal.params.data = "Just a string message";

    json j2 = minimal;
    EXPECT_EQ(j2["params"]["level"], "info");
    EXPECT_FALSE(j2["params"].contains("logger"));

    auto deserialized2 = j2.get<mcp::LoggingMessageNotification>();
    EXPECT_EQ(deserialized2.params.level, mcp::LoggingLevel::Info);
    EXPECT_FALSE(deserialized2.params.logger.has_value());
}

TEST(ProtocolTest, InitializedNotificationSerialization) {
    mcp::InitializedNotification notif;
    json j = notif;
    EXPECT_EQ(j["method"], "notifications/initialized");

    auto deserialized = j.get<mcp::InitializedNotification>();
    EXPECT_EQ(deserialized.method, "notifications/initialized");
}

TEST(ProtocolTest, ListChangedNotificationsSerialization) {
    mcp::PromptListChangedNotification p_notif;
    EXPECT_EQ(json(p_notif)["method"], "notifications/prompts/list_changed");

    mcp::ResourceListChangedNotification r_notif;
    EXPECT_EQ(json(r_notif)["method"], "notifications/resources/list_changed");

    mcp::ToolListChangedNotification t_notif;
    EXPECT_EQ(json(t_notif)["method"], "notifications/tools/list_changed");

    mcp::RootsListChangedNotification root_notif;
    EXPECT_EQ(json(root_notif)["method"], "notifications/roots/list_changed");
}

TEST(ProtocolTest, ResourceUpdatedNotificationSerialization) {
    mcp::ResourceUpdatedNotification notif;
    notif.params.uri = "file:///test.txt";

    json j = notif;
    EXPECT_EQ(j["method"], "notifications/resources/updated");
    EXPECT_EQ(j["params"]["uri"], "file:///test.txt");

    auto deserialized = j.get<mcp::ResourceUpdatedNotification>();
    EXPECT_EQ(deserialized.method, "notifications/resources/updated");
    EXPECT_EQ(deserialized.params.uri, "file:///test.txt");
}

TEST(ProtocolTest, SubscribeRequestSerialization) {
    mcp::SubscribeRequest req;
    req.params.uri = "file:///sub.txt";

    json j = req;
    EXPECT_EQ(j["method"], "resources/subscribe");
    EXPECT_EQ(j["params"]["uri"], "file:///sub.txt");

    auto deserialized = j.get<mcp::SubscribeRequest>();
    EXPECT_EQ(deserialized.method, "resources/subscribe");
    EXPECT_EQ(deserialized.params.uri, "file:///sub.txt");
}

TEST(ProtocolTest, UnsubscribeRequestSerialization) {
    mcp::UnsubscribeRequest req;
    req.params.uri = "file:///unsub.txt";

    json j = req;
    EXPECT_EQ(j["method"], "resources/unsubscribe");
    EXPECT_EQ(j["params"]["uri"], "file:///unsub.txt");

    auto deserialized = j.get<mcp::UnsubscribeRequest>();
    EXPECT_EQ(deserialized.method, "resources/unsubscribe");
    EXPECT_EQ(deserialized.params.uri, "file:///unsub.txt");
}

TEST(ProtocolTest, ToolExecutionSerialization) {
    mcp::ToolExecution exec;
    exec.taskSupport = "optional";
    nlohmann::json j = exec;
    EXPECT_EQ(j["taskSupport"], "optional");
    auto rt = j.get<mcp::ToolExecution>();
    EXPECT_EQ(rt.taskSupport, "optional");
}

TEST(ProtocolTest, RelatedTaskMetadataSerialization) {
    mcp::RelatedTaskMetadata meta;
    meta.id = "task-123";
    meta.title = "Related task";
    nlohmann::json j = meta;
    EXPECT_EQ(j["id"], "task-123");
    EXPECT_EQ(j["title"], "Related task");
    auto rt = j.get<mcp::RelatedTaskMetadata>();
    EXPECT_EQ(rt.id, "task-123");
    EXPECT_EQ(rt.title.value_or(""), "Related task");
}

TEST(ProtocolTest, PrimitiveSchemaDefinitionSerialization) {
    mcp::PrimitiveSchemaDefinition def;
    def.type = "string";
    def.title = "My String";
    def.description = "A simple string";
    nlohmann::json j = def;
    EXPECT_EQ(j["type"], "string");
    EXPECT_EQ(j["title"], "My String");
    auto rt = j.get<mcp::PrimitiveSchemaDefinition>();
    EXPECT_EQ(rt.type, "string");
    EXPECT_EQ(rt.title.value_or(""), "My String");
}

TEST(ProtocolTest, EnumSchemaSerialization) {
    mcp::EnumSchema def;
    def.type = "string";
    def.enumValues = {"a", "b", "c"};
    nlohmann::json j = def;
    EXPECT_EQ(j["type"], "string");
    EXPECT_EQ(j["enum"][0], "a");
    auto rt = j.get<mcp::EnumSchema>();
    EXPECT_EQ(rt.type, "string");
    EXPECT_EQ(rt.enumValues[1], "b");
}

TEST(ProtocolTest, CallToolRequestSerialization) {
    mcp::CallToolRequest req;
    req.params.name = "tool_1";
    req.params.arguments = nlohmann::json::object();
    nlohmann::json j = req;
    EXPECT_EQ(j["method"], "tools/call");
    EXPECT_EQ(j["params"]["name"], "tool_1");
    auto rt = j.get<mcp::CallToolRequest>();
    EXPECT_EQ(rt.method, "tools/call");
    EXPECT_EQ(rt.params.name, "tool_1");
}

TEST(ProtocolTest, ListToolsRequestResultSerialization) {
    mcp::ListToolsRequest req;
    mcp::ListToolsRequestParams params;
    params.cursor = "abc";
    req.params = params;
    nlohmann::json j_req = req;
    EXPECT_EQ(j_req["method"], "tools/list");
    EXPECT_EQ(j_req["params"]["cursor"], "abc");

    mcp::ListToolsResult res;
    res.nextCursor = "def";
    nlohmann::json j_res = res;
    EXPECT_EQ(j_res["nextCursor"], "def");
}

TEST(ProtocolTest, ListResourcesRequestResultSerialization) {
    mcp::ListResourcesRequest req;
    nlohmann::json j_req = req;
    EXPECT_EQ(j_req["method"], "resources/list");

    mcp::ListResourcesResult res;
    res.nextCursor = "def";
    nlohmann::json j_res = res;
    EXPECT_EQ(j_res["nextCursor"], "def");
}

TEST(ProtocolTest, ListResourceTemplatesRequestResultSerialization) {
    mcp::ListResourceTemplatesRequest req;
    nlohmann::json j_req = req;
    EXPECT_EQ(j_req["method"], "resources/templates/list");

    mcp::ListResourceTemplatesResult res;
    res.nextCursor = "def";
    nlohmann::json j_res = res;
    EXPECT_EQ(j_res["nextCursor"], "def");
}

TEST(ProtocolTest, ReadResourceRequestSerialization) {
    mcp::ReadResourceRequest req;
    req.params.uri = "file:///a.txt";
    nlohmann::json j = req;
    EXPECT_EQ(j["method"], "resources/read");
    EXPECT_EQ(j["params"]["uri"], "file:///a.txt");
    auto rt = j.get<mcp::ReadResourceRequest>();
    EXPECT_EQ(rt.params.uri, "file:///a.txt");
}

TEST(ProtocolTest, GetTaskRequestSerialization) {
    mcp::GetTaskRequest req;
    req.params.id = "task1";
    nlohmann::json j = req;
    EXPECT_EQ(j["method"], "tasks/get");
    EXPECT_EQ(j["params"]["id"], "task1");
}

TEST(ProtocolTest, CancelTaskRequestSerialization) {
    mcp::CancelTaskRequest req;
    req.params.id = "task1";
    nlohmann::json j = req;
    EXPECT_EQ(j["method"], "tasks/cancel");
    EXPECT_EQ(j["params"]["id"], "task1");
}

TEST(ProtocolTest, CancelTaskResultSerialization) {
    mcp::CancelTaskResult res;
    res.meta = nlohmann::json::object();
    nlohmann::json j = res;
    EXPECT_TRUE(j.contains("_meta"));
}

TEST(ProtocolTest, ListTasksRequestSerialization) {
    mcp::ListTasksRequest req;
    mcp::ListTasksRequestParams params;
    params.cursor = "abc";
    req.params = params;
    nlohmann::json j = req;
    EXPECT_EQ(j["method"], "tasks/list");
    EXPECT_EQ(j["params"]["cursor"], "abc");
}

TEST(ProtocolTest, GetTaskPayloadRequestResultSerialization) {
    mcp::GetTaskPayloadRequest req;
    req.params.id = "task1";
    nlohmann::json j = req;
    EXPECT_EQ(j["method"], "tasks/getPayload");
    EXPECT_EQ(j["params"]["id"], "task1");

    mcp::GetTaskPayloadResult res;
    res.payload = nlohmann::json::object();
    nlohmann::json j_res = res;
    EXPECT_TRUE(j_res.contains("payload"));
}

TEST(ProtocolTest, TaskStatusNotificationSerialization) {
    mcp::TaskStatusNotification notif;
    notif.params.id = "task1";
    notif.params.status = mcp::TaskStatus::Working;
    notif.params.message = "Working";
    nlohmann::json j = notif;
    EXPECT_EQ(j["method"], "notifications/tasks/status");
    EXPECT_EQ(j["params"]["id"], "task1");
    EXPECT_EQ(j["params"]["status"], "working");
    EXPECT_EQ(j["params"]["message"], "Working");
}

TEST(ProtocolTest, ElicitationCompleteNotificationSerialization) {
    mcp::ElicitationCompleteNotification notif;
    notif.params.requestId = "req1";
    nlohmann::json j = notif;
    EXPECT_EQ(j["method"], "notifications/elicitation/complete");
    EXPECT_EQ(j["params"]["requestId"], "req1");
}

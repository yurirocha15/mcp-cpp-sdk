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

TEST(ProtocolTest, ContentSerialization) {
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

TEST(ProtocolTest, MixedContentSerialization) {
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

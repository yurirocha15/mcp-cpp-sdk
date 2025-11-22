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
    EXPECT_EQ(*req.clientInfo.title, "Example Client");  // NOLINT
    ASSERT_TRUE(req.clientInfo.websiteUrl.has_value());
    EXPECT_EQ(*req.clientInfo.websiteUrl, "https://example.com");  // NOLINT
}

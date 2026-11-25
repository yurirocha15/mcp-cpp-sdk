#include "mcp/protocol/capabilities.hpp"
#include "mcp/protocol/extensions.hpp"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

namespace capabilities_extensions_test {

using json = nlohmann::json;

TEST(CapabilitiesExtensionsTest, ClientCapabilitiesExtensionsRoundTripsWhenPresent) {
    mcp::ClientCapabilities caps;
    caps.extensions = std::map<std::string, nlohmann::json>{
        {"com.example/foo", json{{"version", 1}}},
        {"com.example/bar", json::object()},
    };

    json json_obj = caps;
    ASSERT_TRUE(json_obj.contains("extensions"));
    EXPECT_EQ(json_obj["extensions"]["com.example/foo"]["version"], 1);
    EXPECT_TRUE(json_obj["extensions"]["com.example/bar"].is_object());

    auto round_tripped = json_obj.get<mcp::ClientCapabilities>();
    ASSERT_TRUE(round_tripped.extensions.has_value());
    EXPECT_EQ(round_tripped.extensions->at("com.example/foo")["version"], 1);
    EXPECT_TRUE(round_tripped.extensions->count("com.example/bar"));
}

TEST(CapabilitiesExtensionsTest, ClientCapabilitiesExtensionsAbsentByDefault) {
    mcp::ClientCapabilities caps;

    json json_obj = caps;
    EXPECT_FALSE(json_obj.contains("extensions"));

    auto round_tripped = json_obj.get<mcp::ClientCapabilities>();
    EXPECT_FALSE(round_tripped.extensions.has_value());
}

TEST(CapabilitiesExtensionsTest, ServerCapabilitiesExtensionsRoundTripsWhenPresent) {
    mcp::ServerCapabilities caps;
    caps.extensions =
        std::map<std::string, nlohmann::json>{{"com.example/baz", json{{"enabled", true}}}};

    json json_obj = caps;
    ASSERT_TRUE(json_obj.contains("extensions"));
    EXPECT_EQ(json_obj["extensions"]["com.example/baz"]["enabled"], true);

    auto round_tripped = json_obj.get<mcp::ServerCapabilities>();
    ASSERT_TRUE(round_tripped.extensions.has_value());
    EXPECT_EQ(round_tripped.extensions->at("com.example/baz")["enabled"], true);
}

TEST(CapabilitiesExtensionsTest, ServerCapabilitiesExtensionsAbsentByDefault) {
    mcp::ServerCapabilities caps;

    json json_obj = caps;
    EXPECT_FALSE(json_obj.contains("extensions"));

    auto round_tripped = json_obj.get<mcp::ServerCapabilities>();
    EXPECT_FALSE(round_tripped.extensions.has_value());
}

// An extension declared without a settings object has empty settings, and one not declared has
// none, on either side.
TEST(CapabilitiesExtensionsTest, SettingsOfAnExtensionDeclaredWithoutAnObjectAreEmpty) {
    mcp::ClientCapabilities client;
    mcp::ServerCapabilities server;
    EXPECT_FALSE(mcp::extension_settings(client, "com.example/foo").has_value());
    EXPECT_FALSE(mcp::extension_settings(server, "com.example/foo").has_value());

    client.extensions = std::map<std::string, nlohmann::json>{{"com.example/foo", true}};
    server.extensions = std::map<std::string, nlohmann::json>{{"com.example/foo", true}};
    const auto client_settings = mcp::extension_settings(client, "com.example/foo");
    const auto server_settings = mcp::extension_settings(server, "com.example/foo");
    ASSERT_TRUE(client_settings.has_value());
    ASSERT_TRUE(server_settings.has_value());
    EXPECT_EQ(*client_settings, json::object());
    EXPECT_EQ(*server_settings, json::object());
    EXPECT_FALSE(mcp::extension_settings(client, "com.example/bar").has_value());
    EXPECT_FALSE(mcp::extension_settings(server, "com.example/bar").has_value());
}

}  // namespace capabilities_extensions_test

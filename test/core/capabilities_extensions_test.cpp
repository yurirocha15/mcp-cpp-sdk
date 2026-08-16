#include "mcp/protocol/capabilities.hpp"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

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

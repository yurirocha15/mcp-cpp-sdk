#include "../support/extension_harness.hpp"
#include "../test_utils.hpp"

#include "mcp/server/server.hpp"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

namespace {

using nlohmann::json;

mcp::ServerCapabilities resource_capabilities() {
    mcp::ServerCapabilities capabilities;
    capabilities.resources = mcp::ServerCapabilities::ResourcesCapability{};
    return capabilities;
}

TEST(ServerExtensionTest, DeclaredExtensionsAreAdvertisedInDiscoverAndInitialize) {
    mcp::Server server({"extension-server", "1.0"}, resource_capabilities());
    server.declare_extension("com.example/widgets", json{{"version", 2}});
    server.declare_extension("com.example/plain");

    const auto discover = mcp_test::dispatch_direct(server, make_discover_request(1));
    ASSERT_TRUE(discover.contains("result")) << discover;
    const auto& extensions = discover["result"]["capabilities"]["extensions"];
    EXPECT_EQ(extensions["com.example/widgets"], (json{{"version", 2}}));
    EXPECT_EQ(extensions["com.example/plain"], json::object());

    const auto initialize = mcp_test::dispatch_direct(server, make_initialize_request("init"));
    ASSERT_TRUE(initialize.contains("result")) << initialize;
    EXPECT_EQ(initialize["result"]["capabilities"]["extensions"]["com.example/widgets"],
              (json{{"version", 2}}));

    ASSERT_TRUE(server.capabilities().extensions.has_value());
    EXPECT_EQ(server.capabilities().extensions->size(), 2U);
}

TEST(ServerExtensionTest, ADeclarationReplacesAnEarlierOneAndRejectsBadInput) {
    mcp::Server server({"extension-server", "1.0"}, {});
    server.declare_extension("com.example/widgets", json{{"version", 1}});
    server.declare_extension("com.example/widgets", json{{"version", 2}});
    EXPECT_EQ(server.capabilities().extensions->at("com.example/widgets"), (json{{"version", 2}}));

    EXPECT_THROW(server.declare_extension("", json::object()), std::invalid_argument);
    EXPECT_THROW(server.declare_extension("com.example/widgets", json::array()), std::invalid_argument);
}

TEST(ServerExtensionTest, ExtensionMethodsAreServedToModernClients) {
    mcp::Server server({"extension-server", "1.0"}, {});
    server.add_extension_method(
        "com.example/echo", [](mcp::Context& ctx, const json& params) -> mcp::Task<json> {
            co_return json{{"echo", params.at("value")}, {"modern", ctx.request_meta() != nullptr}};
        });

    const auto response = mcp_test::dispatch_direct(
        server, make_modern_request(1, "com.example/echo", {{"value", "hi"}}));
    ASSERT_TRUE(response.contains("result")) << response;
    EXPECT_EQ(response["result"]["echo"], "hi");
    EXPECT_EQ(response["result"]["modern"], true);
    EXPECT_EQ(response["result"]["resultType"], "complete");
}

TEST(ServerExtensionTest, AResultKeepsTheCacheHintsItSets) {
    mcp::Server server({"extension-server", "1.0"}, {});
    server.add_extension_method("com.example/hinted",
                                [](mcp::Context&, const json&) -> mcp::Task<json> {
                                    co_return json{{"ttlMs", 1234}, {"cacheScope", "public"}};
                                });
    const auto response =
        mcp_test::dispatch_direct(server, make_modern_request(1, "com.example/hinted"));
    ASSERT_TRUE(response.contains("result")) << response;
    EXPECT_EQ(response["result"]["ttlMs"], 1234);
    EXPECT_EQ(response["result"]["cacheScope"], "public");
}

TEST(ServerExtensionTest, InvalidArgumentBecomesInvalidParams) {
    mcp::Server server({"extension-server", "1.0"}, {});
    server.add_extension_method("com.example/strict",
                                [](mcp::Context&, const json&) -> mcp::Task<json> {
                                    throw std::invalid_argument("the value is wrong");
                                    co_return json::object();
                                });
    const auto response =
        mcp_test::dispatch_direct(server, make_modern_request(1, "com.example/strict"));
    ASSERT_TRUE(response.contains("error")) << response;
    EXPECT_EQ(response["error"]["code"], mcp::g_INVALID_PARAMS);
    EXPECT_EQ(response["error"]["message"], "the value is wrong");
}

TEST(ServerExtensionTest, OtherExceptionsAreInternalErrors) {
    mcp::Server server({"extension-server", "1.0"}, {});
    server.add_extension_method("com.example/broken",
                                [](mcp::Context&, const json&) -> mcp::Task<json> {
                                    throw std::runtime_error("broken");
                                    co_return json::object();
                                });
    const auto response =
        mcp_test::dispatch_direct(server, make_modern_request(1, "com.example/broken"));
    ASSERT_TRUE(response.contains("error")) << response;
    EXPECT_EQ(response["error"]["code"], mcp::g_INTERNAL_ERROR);
}

TEST(ServerExtensionTest, MiddlewareWrapsExtensionHandlers) {
    mcp::Server server({"extension-server", "1.0"}, {});
    int handler_calls = 0;
    server.use([](mcp::Context&, const json&, mcp::TypeErasedHandler) -> mcp::Task<json> {
        throw std::runtime_error("denied");
        co_return json::object();
    });
    server.add_extension_method("com.example/echo",
                                [&handler_calls](mcp::Context&, const json&) -> mcp::Task<json> {
                                    ++handler_calls;
                                    co_return json::object();
                                });
    const auto response = mcp_test::dispatch_direct(server, make_modern_request(1, "com.example/echo"));
    ASSERT_TRUE(response.contains("error")) << response;
    EXPECT_EQ(response["error"]["code"], mcp::g_INTERNAL_ERROR);
    EXPECT_EQ(handler_calls, 0);
}

TEST(ServerExtensionTest, ExtensionMethodsAreNotFoundWithoutRegistrationOrBeforeModernEra) {
    mcp::Server server({"extension-server", "1.0"}, {});
    server.add_extension_method("com.example/echo", [](mcp::Context&, const json&) -> mcp::Task<json> {
        co_return json::object();
    });

    const auto unregistered =
        mcp_test::dispatch_direct(server, make_modern_request(1, "com.example/other"));
    EXPECT_EQ(unregistered["error"]["code"], mcp::g_METHOD_NOT_FOUND) << unregistered;

    // A request that carries no 2026-07-28 `_meta` is a legacy request, which has no extension methods.
    const json legacy = {{"jsonrpc", "2.0"}, {"id", 2}, {"method", "com.example/echo"}};
    const auto response = mcp_test::dispatch_direct(server, legacy);
    ASSERT_TRUE(response.contains("error")) << response;
    EXPECT_EQ(response["error"]["code"], mcp::g_METHOD_NOT_FOUND);
}

TEST(ServerExtensionTest, MethodRegistrationRejectsClashesAndEmptyInput) {
    mcp::Server server({"extension-server", "1.0"}, {});
    const mcp::TypeErasedHandler handler = [](mcp::Context&, const json&) -> mcp::Task<json> {
        co_return json::object();
    };
    server.add_extension_method("com.example/echo", handler);
    EXPECT_THROW(server.add_extension_method("com.example/echo", handler), std::invalid_argument);
    EXPECT_THROW(server.add_extension_method("tools/list", handler), std::invalid_argument);
    EXPECT_THROW(server.add_extension_method("", handler), std::invalid_argument);
    EXPECT_THROW(server.add_extension_method("com.example/none", nullptr), std::invalid_argument);
}

}  // namespace

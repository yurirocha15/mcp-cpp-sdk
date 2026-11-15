#include "../support/extension_harness.hpp"
#include "../test_utils.hpp"

#include "mcp/client/apps.hpp"
#include "mcp/protocol/apps.hpp"
#include "mcp/server/apps.hpp"
#include "mcp/server/server.hpp"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

namespace server_apps_test {

namespace {

using nlohmann::json;
namespace apps = mcp::apps;

mcp::ServerCapabilities app_capabilities() {
    mcp::ServerCapabilities capabilities;
    capabilities.tools = mcp::ServerCapabilities::ToolsCapability{};
    capabilities.resources = mcp::ServerCapabilities::ResourcesCapability{};
    return capabilities;
}

apps::ViewResource weather_view() {
    apps::ViewResource view;
    view.uri = "ui://weather-server/dashboard-template";
    view.name = "weather_dashboard";
    view.description = "Interactive weather dashboard view";
    view.html = "<!DOCTYPE html><html><body>weather</body></html>";
    apps::ResourceMeta meta;
    meta.csp = apps::ResourceCsp{};
    meta.csp->connectDomains = std::vector<std::string>{"https://api.openweathermap.org"};
    meta.csp->resourceDomains = std::vector<std::string>{"https://cdn.jsdelivr.net"};
    meta.prefersBorder = true;
    view.meta = meta;
    return view;
}

mcp::Tool weather_tool() {
    mcp::Tool tool;
    tool.name = "get_weather";
    tool.description = "Get current weather for a location";
    tool.inputSchema = {{"type", "object"}, {"properties", {{"location", {{"type", "string"}}}}}};
    return tool;
}

json weather_result(const json& /*arguments*/) {
    return {{"content", json::array({{{"type", "text"}, {"text", "sunny"}}})}};
}

// The tool and resource metadata the specification shows for its weather example.
TEST(AppsWireTest, ToolMetaMatchesTheSpecificationExample) {
    const auto parsed = json::parse(R"({
        "name": "get_weather",
        "inputSchema": {"type": "object"},
        "_meta": {"ui": {"resourceUri": "ui://weather-server/dashboard-template",
                         "visibility": ["model", "app"]}}
    })")
                            .get<mcp::Tool>();

    const auto meta = apps::tool_meta(parsed);
    ASSERT_TRUE(meta.has_value());
    EXPECT_EQ(meta->resourceUri, "ui://weather-server/dashboard-template");
    ASSERT_TRUE(meta->visibility.has_value());
    EXPECT_EQ(meta->visibility->size(), 2U);
    EXPECT_EQ(apps::tool_view_uri(parsed), "ui://weather-server/dashboard-template");
    EXPECT_TRUE(apps::visible_to_model(parsed));
    EXPECT_TRUE(apps::visible_to_app(parsed));

    EXPECT_EQ(json(*meta), (json{{"resourceUri", "ui://weather-server/dashboard-template"},
                                 {"visibility", json::array({"model", "app"})}}));
}

TEST(AppsWireTest, AnAppOnlyToolIsHiddenFromTheModel) {
    const auto tool = json::parse(R"({
        "name": "refresh_dashboard",
        "inputSchema": {"type": "object"},
        "_meta": {"ui": {"resourceUri": "ui://weather-server/dashboard-template",
                         "visibility": ["app"]}}
    })")
                          .get<mcp::Tool>();
    EXPECT_FALSE(apps::visible_to_model(tool));
    EXPECT_TRUE(apps::visible_to_app(tool));
}

TEST(AppsWireTest, ATombstoneVisibilityEntryDoesNotHideAKnownOne) {
    const auto tool = json::parse(R"({
        "name": "t", "inputSchema": {"type": "object"},
        "_meta": {"ui": {"visibility": ["app", "future-audience"]}}
    })")
                          .get<mcp::Tool>();
    EXPECT_TRUE(apps::visible_to_app(tool));
    EXPECT_FALSE(apps::visible_to_model(tool));
}

TEST(AppsWireTest, ToolWithoutMetadataIsVisibleToBothAndLinksNothing) {
    mcp::Tool tool = weather_tool();
    EXPECT_FALSE(apps::tool_meta(tool).has_value());
    EXPECT_FALSE(apps::tool_view_uri(tool).has_value());
    EXPECT_TRUE(apps::visible_to_model(tool));
    EXPECT_TRUE(apps::visible_to_app(tool));
}

TEST(AppsWireTest, TheDeprecatedFlatKeyStillNamesTheView) {
    const auto flat = json::parse(R"({
        "name": "t", "inputSchema": {"type": "object"},
        "_meta": {"ui/resourceUri": "ui://legacy/view"}
    })")
                          .get<mcp::Tool>();
    EXPECT_EQ(apps::tool_view_uri(flat), "ui://legacy/view");

    // The structured key wins when both are present.
    const auto both = json::parse(R"({
        "name": "t", "inputSchema": {"type": "object"},
        "_meta": {"ui/resourceUri": "ui://legacy/view", "ui": {"resourceUri": "ui://new/view"}}
    })")
                          .get<mcp::Tool>();
    EXPECT_EQ(apps::tool_view_uri(both), "ui://new/view");
}

TEST(AppsWireTest, AToolNamingANonViewUriLinksNoView) {
    const auto tool = json::parse(R"({
        "name": "t", "inputSchema": {"type": "object"},
        "_meta": {"ui": {"resourceUri": "https://example.com/view"}}
    })")
                          .get<mcp::Tool>();
    EXPECT_FALSE(apps::tool_view_uri(tool).has_value());
}

TEST(AppsWireTest, ResourceMetaRoundTripsEveryMember) {
    const auto wire = json::parse(R"({
        "csp": {"connectDomains": ["https://api.example.com"],
                "resourceDomains": ["https://cdn.example.com"],
                "frameDomains": ["https://www.youtube.com"],
                "baseUriDomains": ["https://base.example.com"]},
        "permissions": {"camera": {}, "microphone": {}, "geolocation": {}, "clipboardWrite": {}},
        "domain": "a904794854a047f6.apps.example.com",
        "prefersBorder": false
    })");
    const auto meta = wire.get<apps::ResourceMeta>();
    ASSERT_TRUE(meta.csp.has_value());
    EXPECT_EQ(*meta.csp->frameDomains, std::vector<std::string>{"https://www.youtube.com"});
    ASSERT_TRUE(meta.permissions.has_value());
    EXPECT_TRUE(meta.permissions->camera && meta.permissions->microphone &&
                meta.permissions->geolocation && meta.permissions->clipboardWrite);
    EXPECT_EQ(meta.domain, "a904794854a047f6.apps.example.com");
    EXPECT_EQ(meta.prefersBorder, false);
    EXPECT_EQ(json(meta), wire);
}

TEST(AppsWireTest, OnlyGrantedPermissionsAreWritten) {
    apps::ResourcePermissions permissions;
    permissions.geolocation = true;
    EXPECT_EQ(json(permissions), (json{{"geolocation", json::object()}}));
}

TEST(AppsCapabilityTest, AHostDeclaresAndAServerReadsTheMimeTypes) {
    mcp::ClientCapabilities capabilities;
    EXPECT_FALSE(apps::supports_views(capabilities));

    apps::declare_support(capabilities);
    EXPECT_TRUE(apps::supports_views(capabilities));
    EXPECT_EQ(json(capabilities)["extensions"]["io.modelcontextprotocol/ui"],
              (json{{"mimeTypes", json::array({"text/html;profile=mcp-app"})}}));
}

TEST(AppsCapabilityTest, AnotherMimeTypeOrAnEmptySettingsObjectIsNotSupport) {
    mcp::ClientCapabilities other;
    other.extensions = std::map<std::string, json>{
        {"io.modelcontextprotocol/ui", json{{"mimeTypes", json::array({"text/html"})}}}};
    EXPECT_FALSE(apps::supports_views(other));

    mcp::ClientCapabilities empty;
    empty.extensions = std::map<std::string, json>{{"io.modelcontextprotocol/ui", json::object()}};
    EXPECT_FALSE(apps::supports_views(empty));
}

TEST(AppsServerTest, ViewResourcesAreListedAndReadWithTheirMetadata) {
    mcp::Server server({"apps-server", "1.0"}, app_capabilities());
    auto view = weather_view();
    view.list_meta = true;
    apps::add_view_resource(server, view);

    const auto listed = mcp_test::dispatch_direct(server, make_modern_request(1, "resources/list"));
    ASSERT_TRUE(listed.contains("result")) << listed;
    const auto& entry = listed["result"]["resources"][0];
    EXPECT_EQ(entry["uri"], "ui://weather-server/dashboard-template");
    EXPECT_EQ(entry["mimeType"], "text/html;profile=mcp-app");
    EXPECT_EQ(entry["_meta"]["ui"]["prefersBorder"], true);

    const auto read = mcp_test::dispatch_direct(
        server,
        make_modern_request(2, "resources/read", {{"uri", "ui://weather-server/dashboard-template"}}));
    ASSERT_TRUE(read.contains("result")) << read;
    const auto& content = read["result"]["contents"][0];
    EXPECT_EQ(content["mimeType"], "text/html;profile=mcp-app");
    EXPECT_EQ(content["text"], view.html);
    EXPECT_EQ(content["_meta"]["ui"]["csp"]["connectDomains"],
              json::array({"https://api.openweathermap.org"}));
    EXPECT_EQ(content["_meta"]["ui"]["csp"]["resourceDomains"],
              json::array({"https://cdn.jsdelivr.net"}));
}

TEST(AppsServerTest, ListingMetadataIsOmittedUnlessAsked) {
    mcp::Server server({"apps-server", "1.0"}, app_capabilities());
    apps::add_view_resource(server, weather_view());
    const auto listed = mcp_test::dispatch_direct(server, make_modern_request(1, "resources/list"));
    ASSERT_TRUE(listed.contains("result")) << listed;
    EXPECT_FALSE(listed["result"]["resources"][0].contains("_meta"));
}

TEST(AppsServerTest, ViewRegistrationRejectsBadInput) {
    mcp::Server server({"apps-server", "1.0"}, app_capabilities());
    auto view = weather_view();
    view.uri = "https://example.com/view";
    EXPECT_THROW(apps::add_view_resource(server, view), std::invalid_argument);

    view = weather_view();
    view.html.clear();
    EXPECT_THROW(apps::add_view_resource(server, view), std::invalid_argument);

    apps::add_view_resource(server, weather_view());
    EXPECT_THROW(apps::add_view_resource(server, weather_view()), std::invalid_argument);
}

TEST(AppsServerTest, AViewToolCarriesItsLinkInTheToolListing) {
    mcp::Server server({"apps-server", "1.0"}, app_capabilities());
    auto tool = weather_tool();
    tool.meta = json{{"vendor/key", 1}};
    apps::ToolMeta meta;
    meta.resourceUri = "ui://weather-server/dashboard-template";
    meta.visibility = std::vector<apps::Visibility>{apps::Visibility::eModel, apps::Visibility::eApp};
    apps::add_view_tool(server, tool, meta, weather_result);

    const auto listed = mcp_test::dispatch_direct(server, make_modern_request(1, "tools/list"));
    ASSERT_TRUE(listed.contains("result")) << listed;
    const auto& listed_tool = listed["result"]["tools"][0];
    EXPECT_EQ(listed_tool["_meta"]["ui"]["resourceUri"], "ui://weather-server/dashboard-template");
    EXPECT_EQ(listed_tool["_meta"]["ui"]["visibility"], json::array({"model", "app"}));
    EXPECT_EQ(listed_tool["_meta"]["vendor/key"], 1);

    const auto called = mcp_test::dispatch_direct(
        server, make_modern_request(2, "tools/call",
                                    {{"name", "get_weather"}, {"arguments", {{"location", "Oslo"}}}}));
    ASSERT_TRUE(called.contains("result")) << called;
    EXPECT_EQ(called["result"]["content"][0]["text"], "sunny");
}

TEST(AppsServerTest, ALinkToANonViewUriIsRefused) {
    apps::ToolMeta meta;
    meta.resourceUri = "https://example.com/view";
    EXPECT_THROW((void)apps::link_tool_to_view(weather_tool(), meta), std::invalid_argument);
}

TEST(AppsServerTest, AHandlerSeesWhetherTheHostDeclaredSupport) {
    mcp::Server server({"apps-server", "1.0"}, app_capabilities());
    server.add_raw_tool(weather_tool(), [](mcp::Context& ctx, const json&) -> mcp::Task<json> {
        const bool views = apps::supports_views(ctx);
        co_return json{
            {"content", json::array({{{"type", "text"}, {"text", views ? "view" : "text only"}}})}};
    });

    auto with_support = make_modern_request(1, "tools/call", {{"name", "get_weather"}});
    mcp::ClientCapabilities capabilities;
    apps::declare_support(capabilities);
    with_support["params"]["_meta"]["io.modelcontextprotocol/clientCapabilities"] = capabilities;
    const auto supported = mcp_test::dispatch_direct(server, with_support);
    ASSERT_TRUE(supported.contains("result")) << supported;
    EXPECT_EQ(supported["result"]["content"][0]["text"], "view");

    const auto plain = mcp_test::dispatch_direct(
        server, make_modern_request(2, "tools/call", {{"name", "get_weather"}}));
    ASSERT_TRUE(plain.contains("result")) << plain;
    EXPECT_EQ(plain["result"]["content"][0]["text"], "text only");
}

TEST(AppsClientTest, AHostFindsAToolsViewAndReadsItOverTheWire) {
    mcp::Server server({"apps-server", "1.0"}, app_capabilities());
    apps::add_view_resource(server, weather_view());
    apps::ToolMeta meta;
    meta.resourceUri = "ui://weather-server/dashboard-template";
    apps::add_view_tool(server, weather_tool(), meta, weather_result);

    mcp::ClientCapabilities capabilities;
    apps::declare_support(capabilities);
    mcp_test::run_modern_client(
        server, capabilities, [](mcp::Client& client, const mcp::ConnectResult&) -> mcp::Task<void> {
            const auto tools = co_await client.list_tools();
            EXPECT_EQ(tools.tools.size(), 1U);
            const auto uri = apps::tool_view_uri(tools.tools.at(0));
            EXPECT_TRUE(uri.has_value());

            const auto view = co_await apps::read_tool_view(client, tools.tools.at(0));
            EXPECT_EQ(view.uri, "ui://weather-server/dashboard-template");
            EXPECT_EQ(view.mimeType, "text/html;profile=mcp-app");
            EXPECT_EQ(view.html, "<!DOCTYPE html><html><body>weather</body></html>");
            EXPECT_TRUE(view.meta.has_value());
            if (view.meta) {
                EXPECT_EQ(view.meta->prefersBorder, true);
                EXPECT_EQ(*view.meta->csp->connectDomains,
                          std::vector<std::string>{"https://api.openweathermap.org"});
            }
        });
}

TEST(AppsClientTest, ListingMetadataIsTheFallbackWhenTheContentHasNone) {
    mcp::Server server({"apps-server", "1.0"}, app_capabilities());
    mcp::Resource resource;
    resource.uri = "ui://plain/view";
    resource.name = "plain";
    resource.mimeType = "text/html;profile=mcp-app";
    server.add_resource<mcp::ReadResourceRequestParams, json>(
        resource, [](const mcp::ReadResourceRequestParams&) {
            return json{{"contents", json::array({{{"uri", "ui://plain/view"},
                                                   {"mimeType", "text/html;profile=mcp-app"},
                                                   {"text", "<html></html>"}}})}};
        });

    mcp::Resource listed = resource;
    listed.meta =
        apps::make_meta(apps::ResourceMeta{std::nullopt, std::nullopt, "view.example.com", false});

    mcp_test::run_modern_client(
        server, {}, [listed](mcp::Client& client, const mcp::ConnectResult&) -> mcp::Task<void> {
            const auto bare = co_await apps::read_view(client, "ui://plain/view");
            EXPECT_FALSE(bare.meta.has_value());
            const auto view = co_await apps::read_view(client, "ui://plain/view", listed);
            EXPECT_TRUE(view.meta.has_value());
            if (view.meta) {
                EXPECT_EQ(view.meta->domain, "view.example.com");
            }
        });
}

TEST(AppsClientTest, ABlobViewIsDecodedAndAWrongMimeTypeIsRefused) {
    mcp::Server server({"apps-server", "1.0"}, app_capabilities());
    mcp::Resource blob;
    blob.uri = "ui://blob/view";
    blob.name = "blob";
    server.add_resource<mcp::ReadResourceRequestParams, json>(
        blob, [](const mcp::ReadResourceRequestParams&) {
            // "<html></html>" in base64.
            return json{{"contents", json::array({{{"uri", "ui://blob/view"},
                                                   {"mimeType", "text/html;profile=mcp-app"},
                                                   {"blob", "PGh0bWw+PC9odG1sPg=="}}})}};
        });
    mcp::Resource wrong;
    wrong.uri = "ui://wrong/view";
    wrong.name = "wrong";
    server.add_resource<mcp::ReadResourceRequestParams, json>(
        wrong, [](const mcp::ReadResourceRequestParams&) {
            return json{{"contents", json::array({{{"uri", "ui://wrong/view"},
                                                   {"mimeType", "text/html"},
                                                   {"text", "<html></html>"}}})}};
        });

    mcp_test::run_modern_client(
        server, {}, [](mcp::Client& client, const mcp::ConnectResult&) -> mcp::Task<void> {
            const auto view = co_await apps::read_view(client, "ui://blob/view");
            EXPECT_EQ(view.html, "<html></html>");

            bool refused = false;
            try {
                (void)co_await apps::read_view(client, "ui://wrong/view");
            } catch (const std::runtime_error&) {
                refused = true;
            }
            EXPECT_TRUE(refused);

            bool not_a_view = false;
            try {
                (void)co_await apps::read_view(client, "https://example.com/view");
            } catch (const std::invalid_argument&) {
                not_a_view = true;
            }
            EXPECT_TRUE(not_a_view);
        });
}

}  // namespace

}  // namespace server_apps_test

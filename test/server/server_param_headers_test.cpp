#include "../test_utils.hpp"

#include "../../src/detail/param_headers.hpp"
#include "mcp/protocol/schema.hpp"
#include "mcp/server/server.hpp"

#include <gtest/gtest.h>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace {

using nlohmann::json;

json object_with(json properties) {
    return {{"type", "object"}, {"properties", std::move(properties)}};
}

json annotated(std::string_view type, std::string_view header) {
    return {{"type", type}, {"x-mcp-header", header}};
}

void add_tool_with_schema(mcp::Server& server, json input_schema) {
    mcp::Tool tool;
    tool.name = "t";
    tool.inputSchema = std::move(input_schema);
    server.add_raw_tool(tool, [](const json&) { return json{{"content", json::array()}}; });
}

TEST(ServerParamHeaders, AddToolRejectsEachInvalidAnnotation) {
    const std::vector<std::pair<std::string, json>> invalid = {
        {"empty name", object_with({{"a", annotated("string", "")}})},
        {"space in name", object_with({{"a", annotated("string", "Bad Name")}})},
        {"CR LF in name", object_with({{"a", annotated("string", "A\r\nB")}})},
        {"colon in name", object_with({{"a", annotated("string", "A:B")}})},
        {"non-ASCII name", object_with({{"a", annotated("string", "Régión")}})},
        {"non-string annotation", object_with({{"a", {{"type", "string"}, {"x-mcp-header", 7}}}})},
        {"duplicate ignoring case",
         object_with({{"a", annotated("string", "Region")}, {"b", annotated("integer", "region")}})},
        {"number type", object_with({{"a", annotated("number", "A")}})},
        {"object type", object_with({{"a", {{"type", "object"}, {"x-mcp-header", "A"}}}})},
        {"array type", object_with({{"a", {{"type", "array"}, {"x-mcp-header", "A"}}}})},
        {"no type", object_with({{"a", {{"x-mcp-header", "A"}}}})},
        {"two primitive types",
         object_with({{"a", {{"type", json::array({"string", "integer"})}, {"x-mcp-header", "A"}}}})},
        {"on the root", {{"type", "object"}, {"x-mcp-header", "A"}}},
        {"under items", object_with({{"a", {{"type", "array"}, {"items", annotated("string", "A")}}}})},
        {"under prefixItems",
         object_with(
             {{"a", {{"type", "array"}, {"prefixItems", json::array({annotated("string", "A")})}}}})},
        {"under oneOf",
         {{"type", "object"},
          {"oneOf", json::array({object_with({{"a", annotated("string", "A")}})})}}},
        {"under allOf",
         {{"type", "object"},
          {"allOf", json::array({object_with({{"a", annotated("string", "A")}})})}}},
        {"under not", {{"type", "object"}, {"not", object_with({{"a", annotated("string", "A")}})}}},
        {"under then",
         {{"type", "object"},
          {"if", json::object()},
          {"then", object_with({{"a", annotated("string", "A")}})}}},
        {"under $defs",
         {{"type", "object"},
          {"$defs", {{"d", annotated("string", "A")}}},
          {"properties", json::object()}}},
        {"under additionalProperties",
         {{"type", "object"}, {"additionalProperties", annotated("string", "A")}}},
        {"under patternProperties",
         {{"type", "object"}, {"patternProperties", {{"^a", annotated("string", "A")}}}}},
        {"nested under items",
         object_with(
             {{"a", {{"type", "array"}, {"items", object_with({{"b", annotated("string", "B")}})}}}})},
    };
    for (const auto& [why, schema] : invalid) {
        mcp::Server server({"s", "1.0"}, mcp::ServerCapabilities{});
        EXPECT_THROW(add_tool_with_schema(server, schema), mcp::SchemaError) << why;
        EXPECT_THROW(add_tool_with_schema(server, schema), std::invalid_argument) << why;
    }
}

TEST(ServerParamHeaders, RefusalNamesTheTool) {
    mcp::Server server({"s", "1.0"}, mcp::ServerCapabilities{});
    try {
        add_tool_with_schema(
            server, object_with({{"a", {{"type", "array"}, {"items", annotated("string", "A")}}}}));
        FAIL() << "expected a SchemaError";
    } catch (const mcp::SchemaError& error) {
        const std::string text = error.what();
        EXPECT_NE(text.find("Tool 't'"), std::string::npos) << text;
        EXPECT_NE(text.find("x-mcp-header"), std::string::npos) << text;
    }
}

TEST(ServerParamHeaders, AddToolWithoutAnnotationNeverThrows) {
    const std::vector<json> schemas = {
        {{"type", "object"}},
        object_with({{"n", {{"type", "number"}}},
                     {"list", {{"type", "array"}, {"items", {{"type", "string"}}}}}}),
        {{"type", "object"},
         {"$defs", {{"d", {{"type", "string"}}}}},
         {"properties", {{"r", {{"$ref", "#/$defs/d"}}}}}},
        {{"type", "object"}, {"oneOf", json::array({{{"required", json::array({"a"})}}})}},
    };
    for (const auto& schema : schemas) {
        mcp::Server server({"s", "1.0"}, mcp::ServerCapabilities{});
        EXPECT_NO_THROW(add_tool_with_schema(server, schema)) << schema;
    }
}

TEST(ServerParamHeaders, ValidAnnotationsAreCollectedWithTheirPaths) {
    const auto schema =
        object_with({{"region", annotated("string", "Region")},
                     {"count", annotated("integer", "Count")},
                     {"flag", {{"type", json::array({"boolean", "null"})}, {"x-mcp-header", "Flag"}}},
                     {"scope", object_with({{"tenant", annotated("string", "Tenant-Id")}})}});
    const auto headers = mcp::detail::collect_param_headers(schema);
    ASSERT_EQ(headers.size(), 4u);
    std::vector<std::pair<std::string, std::vector<std::string>>> seen;
    for (const auto& header : headers) {
        seen.emplace_back(header.name, header.path);
    }
    EXPECT_NE(
        std::find(seen.begin(), seen.end(),
                  std::pair<std::string, std::vector<std::string>>{"Tenant-Id", {"scope", "tenant"}}),
        seen.end());
    EXPECT_NE(std::find(seen.begin(), seen.end(),
                        std::pair<std::string, std::vector<std::string>>{"Count", {"count"}}),
              seen.end());

    mcp::Server server({"s", "1.0"}, mcp::ServerCapabilities{});
    EXPECT_NO_THROW(add_tool_with_schema(server, schema));
}

TEST(ServerParamHeaders, PropertyLiterallyNamedXMcpHeaderIsNotAnAnnotation) {
    const auto schema = object_with({{"x-mcp-header", {{"type", "object"}}}});
    EXPECT_TRUE(mcp::detail::collect_param_headers(schema).empty());
    mcp::Server server({"s", "1.0"}, mcp::ServerCapabilities{});
    EXPECT_NO_THROW(add_tool_with_schema(server, schema));
}

TEST(ServerParamHeaders, AnnotationLookalikesInsideNonSchemaKeywordsAreIgnored) {
    const json lookalike = {{"x-mcp-header", "A"}};
    const auto schema = object_with({{"a",
                                      {{"type", "object"},
                                       {"enum", json::array({lookalike})},
                                       {"const", lookalike},
                                       {"default", lookalike},
                                       {"examples", json::array({lookalike})},
                                       {"description", "x-mcp-header: A"}}}});
    EXPECT_TRUE(mcp::detail::collect_param_headers(schema).empty());
    mcp::Server server({"s", "1.0"}, mcp::ServerCapabilities{});
    EXPECT_NO_THROW(add_tool_with_schema(server, schema));
}

TEST(ServerParamHeaders, MismatchRulesForEachArgumentType) {
    using mcp::detail::param_header_mismatch;
    const auto params =
        mcp::detail::collect_param_headers(object_with({{"s", annotated("string", "S")},
                                                        {"i", annotated("integer", "I")},
                                                        {"b", annotated("boolean", "B")}}));
    const auto ok = [&](const json& arguments, const mcp::detail::ParamHeaderValues& headers) {
        return !param_header_mismatch(params, arguments, headers).has_value();
    };

    EXPECT_TRUE(ok({{"s", "x"}, {"i", 42}, {"b", true}}, {{"S", "x"}, {"I", "42"}, {"B", "true"}}));
    EXPECT_TRUE(ok({{"i", 42}}, {{"i", "42.0"}}));  // numeric comparison, any name case
    EXPECT_TRUE(ok({{"i", 42.0}}, {{"I", "42"}}));  // an integral float in the body
    EXPECT_TRUE(ok({{"i", -7}}, {{"I", "-7"}}));
    EXPECT_FALSE(ok({{"i", 42}}, {{"I", "42.5"}}));
    EXPECT_FALSE(ok({{"i", 42}}, {{"I", "042x"}}));
    EXPECT_FALSE(ok({{"i", 1.5}}, {{"I", "1.5"}}));  // an integer header cannot carry a fraction
    EXPECT_FALSE(ok({{"b", false}}, {{"B", "False"}}));
    EXPECT_FALSE(ok({{"s", json::array()}}, {{"S", "[]"}}));

    const auto beyond = (std::int64_t{1} << 53);
    EXPECT_FALSE(ok({{"i", beyond}}, {{"I", std::to_string(beyond)}}));
    EXPECT_FALSE(ok({{"i", -beyond}}, {{"I", std::to_string(-beyond)}}));
    EXPECT_FALSE(ok({{"i", std::numeric_limits<std::uint64_t>::max()}},
                    {{"I", std::to_string(std::numeric_limits<std::uint64_t>::max())}}));
    EXPECT_FALSE(ok({{"i", 1e300}}, {{"I", "1"}}));
    EXPECT_TRUE(ok({{"i", beyond - 1}}, {{"I", std::to_string(beyond - 1)}}));

    // Absent and null arguments expect no header, and an unannotated header is ignored.
    EXPECT_TRUE(ok({{"s", nullptr}}, {}));
    EXPECT_TRUE(ok(json::object(), {{"Other", "anything"}}));
    EXPECT_FALSE(ok({{"s", "x"}}, {}));
    EXPECT_FALSE(ok({{"s", "x"}}, {{"S", "x"}, {"s", "x"}}));  // repeated header

    // A header the tool names is checked for a valid value even when its argument is absent or
    // null; only the comparison with the body is skipped.
    EXPECT_TRUE(ok({{"s", nullptr}}, {{"S", "x"}}));
    EXPECT_TRUE(ok(json::object(), {{"S", "=?base64?eA==?="}}));
    EXPECT_FALSE(ok(json::object(), {{"S", std::string("bad\x01value")}}));
    EXPECT_FALSE(ok({{"s", nullptr}}, {{"S", "=?base64?!!!?="}}));
    EXPECT_FALSE(ok(json::object(), {{"S", "a"}, {"s", "b"}}));
}

class ServerParamHeadersDispatch : public ::testing::Test {
   protected:
    boost::asio::io_context io_ctx_;

    static std::unique_ptr<mcp::Server> make_server() {
        mcp::ServerCapabilities caps;
        caps.tools = mcp::ServerCapabilities::ToolsCapability{};
        auto server = std::make_unique<mcp::Server>(mcp::Implementation{"s", "1.0"}, caps);
        server->add_tool<json, json>("route", "Routed by region",
                                     object_with({{"region", annotated("string", "Region")}}),
                                     [](json args) -> mcp::Task<json> { co_return args; });
        return server;
    }

    json dispatch(mcp::Server& server, json request, mcp::RequestContext context) {
        json response;
        boost::asio::co_spawn(io_ctx_,
                              server.dispatch_request_direct(std::move(request), std::move(context)),
                              [&response](std::exception_ptr error, std::string wire) {
                                  EXPECT_EQ(error, nullptr);
                                  if (!error) {
                                      response = json::parse(wire);
                                  }
                              });
        io_ctx_.restart();
        io_ctx_.run();
        return response;
    }

    static json route_call(std::string_view region) {
        return make_modern_request(1, "tools/call",
                                   {{"name", "route"}, {"arguments", {{"region", region}}}});
    }
};

TEST_F(ServerParamHeadersDispatch, CheckedOnlyWhenTheTransportSuppliesHeaders) {
    auto server = make_server();
    // No headers supplied: the request did not come over HTTP.
    auto unchecked = dispatch(*server, route_call("us"), {});
    EXPECT_TRUE(unchecked.contains("result")) << unchecked;

    mcp::RequestContext mismatched;
    mismatched.param_headers = {{{"Region", "eu"}}};
    auto refused = dispatch(*server, route_call("us"), std::move(mismatched));
    ASSERT_TRUE(refused.contains("error")) << refused;
    EXPECT_EQ(refused["error"]["code"], mcp::g_HEADER_MISMATCH);

    mcp::RequestContext matched;
    matched.param_headers = {{{"Region", "us"}}};
    auto served = dispatch(*server, route_call("us"), std::move(matched));
    EXPECT_TRUE(served.contains("result")) << served;
}

// The argument is absent, so the header is not compared, but a value no header may carry is
// still refused.
TEST_F(ServerParamHeadersDispatch, InvalidHeaderIsRefusedWhenItsArgumentIsAbsent) {
    auto server = make_server();
    const auto call =
        make_modern_request(1, "tools/call", {{"name", "route"}, {"arguments", json::object()}});

    mcp::RequestContext invalid;
    invalid.param_headers = {{{"Region", std::string("eu\x01")}}};
    auto refused = dispatch(*server, call, std::move(invalid));
    ASSERT_TRUE(refused.contains("error")) << refused;
    EXPECT_EQ(refused["error"]["code"], mcp::g_HEADER_MISMATCH);

    mcp::RequestContext valid;
    valid.param_headers = {{{"Region", "eu"}}};
    auto served = dispatch(*server, call, std::move(valid));
    EXPECT_TRUE(served.contains("result")) << served;
}

TEST_F(ServerParamHeadersDispatch, LegacyRequestsAreNeverChecked) {
    auto server = make_server();
    mcp::RequestContext context;
    context.param_headers = {{{"Region", "eu"}}};
    json legacy = {{"jsonrpc", "2.0"},
                   {"id", 1},
                   {"method", "tools/call"},
                   {"params", {{"name", "route"}, {"arguments", {{"region", "us"}}}}}};
    auto response = dispatch(*server, std::move(legacy), std::move(context));
    EXPECT_TRUE(response.contains("result")) << response;
}

}  // namespace

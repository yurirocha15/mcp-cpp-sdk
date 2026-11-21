// Compiling schemas the dialect checks do not reach, refusing them with the right category, and
// the evaluation paths that only a few values take: content keywords, a limit of one reported
// violation, and the comparisons of unusual values.

#include <mcp/protocol/schema.hpp>

#include "../../src/protocol/schema_cache.hpp"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <cstddef>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace json_schema_compile_test {

namespace {

using mcp::CompiledSchema;
using mcp::SchemaError;
using nlohmann::json;

constexpr const char* g_draft4 = "http://json-schema.org/draft-04/schema#";
constexpr const char* g_draft7 = "http://json-schema.org/draft-07/schema#";
constexpr const char* g_draft201909 = "https://json-schema.org/draft/2019-09/schema";

/// The category and message of the SchemaError compiling @p schema throws.
std::pair<SchemaError::Category, std::string> refusal(const json& schema,
                                                      const mcp::SchemaDocuments& documents = {}) {
    try {
        (void)CompiledSchema::compile(schema, {}, documents);
    } catch (const SchemaError& error) {
        return {error.category(), error.what()};
    }
    ADD_FAILURE() << "compiled: " << schema.dump();
    return {SchemaError::Category::eInvalidSchema, ""};
}

/// @p target as the schema a `$ref` reaches from a member no keyword reads, which the dialect
/// checks therefore do not visit.
json referred(const json& target) { return {{"$ref", "#/x"}, {"x", target}}; }

/// @p target as the root of its own resource of @p dialect, reached the same way.
json referred_resource(const char* dialect, json target) {
    target["$schema"] = dialect;
    target[std::string(dialect) == g_draft4 ? "id" : "$id"] = "https://example.com/inner";
    return referred(target);
}

// A keyword of the wrong shape in a schema only a reference reaches is still refused, and named.
TEST(SchemaCompile, MalformedReferenceTargetsAreInvalid) {
    const std::vector<std::pair<json, const char*>> cases = {
        {referred({{"minLength", -1}}), "minLength"},
        {referred({{"minLength", "a"}}), "minLength"},
        {referred({{"minLength", -1.5}}), "minLength"},
        {referred({{"required", "a"}}), "required"},
        {referred({{"required", {1}}}), "required"},
        {referred({{"allOf", json::array()}}), "allOf"},
        {referred({{"properties", 1}}), "properties"},
        {referred({{"minimum", "a"}}), "minimum"},
        {referred({{"multipleOf", 0}}), "multipleOf"},
        {referred({{"pattern", 1}}), "pattern"},
        {referred({{"patternProperties", 1}}), "patternProperties"},
        {referred({{"type", 1}}), "type"},
        {referred({{"type", {1}}}), "type"},
        {referred({{"enum", 1}}), "enum"},
        {referred({{"uniqueItems", 1}}), "uniqueItems"},
        {referred({{"$defs", 1}}), "$defs"},
        {referred({{"$anchor", ""}}), "$anchor"},
        {referred({{"dependentRequired", 1}}), "dependentRequired"},
        {referred({{"dependentRequired", {{"a", 1}}}}), "dependentRequired"},
        {referred({{"not", 5}}), "not"},
        {referred({{"items", {true}}}), "items"},
        {referred({{"items", 5}}), "items"},
        {referred({{"$schema", 5}}), "$schema"},
        {referred({{"$id", 5}}), "$id"},
        {referred({{"$id", "https://example.com/a#frag"}}), "$id"},
        {referred_resource(g_draft4, {{"exclusiveMinimum", 1}}), "exclusiveMinimum"},
        // draft-04's metaschema does not check $ref.
        {{{"$schema", g_draft4}, {"properties", {{"a", {{"$ref", 0}}}}}}, "$ref"},
        {referred_resource(g_draft4, {{"items", {true}}, {"additionalItems", 5}}), "additionalItems"},
        {referred_resource(g_draft7, {{"contentEncoding", 1}}), "contentEncoding"},
        {referred_resource(g_draft7, {{"dependencies", 1}}), "dependencies"},
        {referred_resource(g_draft7, {{"dependencies", {{"a", {1}}}}}), "dependencies"},
        {referred_resource(g_draft7, {{"dependencies", {{"a", 5}}}}), "dependencies"},
        {referred_resource(g_draft201909, {{"$recursiveAnchor", "x"}}), "$recursiveAnchor"},
    };
    for (const auto& [schema, keyword] : cases) {
        SCOPED_TRACE(schema.dump());
        const auto [category, message] = refusal(schema);
        EXPECT_EQ(category, SchemaError::Category::eInvalidSchema);
        EXPECT_EQ(message, "JSON Schema keyword '" + std::string(keyword) + "' cannot be compiled");
    }
}

// An embedded resource of another dialect is read in that dialect, even where the root's dialect
// would refuse the keyword.
TEST(SchemaCompile, EmbeddedResourcesUseTheirOwnDialect) {
    const auto schema = CompiledSchema::compile(
        referred_resource(g_draft4, {{"maximum", 5}, {"exclusiveMaximum", true}}));
    EXPECT_TRUE(schema.validate(4).valid);
    EXPECT_FALSE(schema.validate(5).valid);
}

TEST(SchemaCompile, IdentifiersDeclaredTwiceAreInvalid) {
    EXPECT_EQ(refusal({{"$defs",
                        {{"a", {{"$id", "https://example.com/x"}}},
                         {"b", {{"$id", "https://example.com/x"}}}}}})
                  .second,
              "JSON Schema declares the resource https://example.com/x twice");
    EXPECT_EQ(refusal({{"$schema", g_draft7},
                       {"definitions", {{"a", {{"$id", "#n"}}}, {"b", {{"$id", "#n"}}}}}})
                  .second,
              "JSON Schema declares the anchor 'n' twice");
    EXPECT_EQ(refusal({{"$defs", {{"a", {{"$dynamicAnchor", "n"}}}, {"b", {{"$dynamicAnchor", "n"}}}}}})
                  .second,
              "JSON Schema declares the anchor 'n' twice");
    EXPECT_EQ(refusal({{"$defs", {{"a", {{"$anchor", "n"}}}, {"b", {{"$anchor", "n"}}}}}}).second,
              "JSON Schema declares the anchor 'n' twice");
}

TEST(SchemaCompile, DialectChangesOnlyAtTheRootOfAResource) {
    EXPECT_EQ(refusal({{"$defs", {{"a", {{"$schema", g_draft7}}}}}}).second,
              "JSON Schema changes its dialect outside the root of a resource");
    // The same dialect again, or a new resource, is fine.
    EXPECT_NO_THROW((void)CompiledSchema::compile(
        {{"$defs", {{"a", {{"$schema", "https://json-schema.org/draft/2020-12/schema"}}}}}}));
    EXPECT_NO_THROW((void)CompiledSchema::compile(
        {{"$defs", {{"a", {{"$schema", g_draft7}, {"$id", "https://example.com/a"}}}}}}));
}

// A pointer walks members and array items of any value, not only of schema keywords.
TEST(SchemaCompile, PointersWalkMembersAndArrayItems) {
    const json schema = {{"x", {{{"type", "integer"}}, {{"type", "string"}}}},
                         {"properties", {{"a", {{"$ref", "#/x/0"}}}, {"b", {{"$ref", "#/x/1"}}}}}};
    const auto compiled = CompiledSchema::compile(schema);
    EXPECT_TRUE(compiled.validate({{"a", 1}, {"b", "s"}}).valid);
    EXPECT_FALSE(compiled.validate({{"a", "s"}}).valid);
    EXPECT_FALSE(compiled.validate({{"b", 1}}).valid);
}

TEST(SchemaCompile, PointersToNothingAreUnresolved) {
    for (const char* pointer : {"#/x/2", "#/x/01", "#/x/a", "#/x/", "#/x/99999999999999999999",
                                "#/x/0/type/y", "#/x/0/type", "#/x~2", "#/%zz", "#/missing"}) {
        SCOPED_TRACE(pointer);
        const auto [category, message] = refusal(
            {{"x", {{{"type", "integer"}}, true}}, {"properties", {{"a", {{"$ref", pointer}}}}}});
        EXPECT_EQ(category, SchemaError::Category::eUnresolvedRef);
        EXPECT_NE(message.find(pointer), std::string::npos) << message;
    }
}

// A registered document passes its dialect's checks and can still fail to compile, at its root or
// in a part only a reference reaches.
TEST(SchemaCompile, RegisteredDocumentThatCannotCompileIsInvalid) {
    for (const auto& [document, keyword] :
         std::vector<std::pair<json, const char*>>{{{{"$schema", g_draft4}, {"$ref", 0}}, "$ref"},
                                                   {referred({{"minLength", -1}}), "minLength"}}) {
        SCOPED_TRACE(document.dump());
        const auto [category, message] =
            refusal({{"$ref", "https://example.com/d"}}, {{"https://example.com/d", document}});
        EXPECT_EQ(category, SchemaError::Category::eInvalidSchema);
        EXPECT_EQ(message, "JSON Schema keyword '" + std::string(keyword) + "' cannot be compiled");
    }
}

// Checking a schema against its dialect's rules spends the evaluation work allowed too.
TEST(SchemaCompile, CheckingTheSchemaIsCharged) {
    json properties = json::object();
    for (int i = 0; i < 200; ++i) {
        properties["p" + std::to_string(i)] = {{"type", "string"}};
    }
    mcp::SchemaOptions options;
    options.max_evaluation_work = 100;
    try {
        (void)CompiledSchema::compile({{"properties", properties}}, options);
        ADD_FAILURE() << "compiled";
    } catch (const SchemaError& error) {
        EXPECT_EQ(error.category(), SchemaError::Category::eInvalidSchema);
        EXPECT_STREQ(error.what(), "checking the JSON Schema takes too much work");
    }
}

// A reference to a metaschema stops the validation of a value that nests deeper than the bound of
// nested evaluations, which a value may when the instance depth allows it.
TEST(SchemaCompile, MetaschemaReferenceStopsAtTheEvaluationBound) {
    mcp::SchemaOptions options;
    options.max_instance_depth = 1000;
    const auto schema =
        CompiledSchema::compile({{"$ref", "https://json-schema.org/draft/2020-12/schema"}}, options);
    json value = true;
    for (int i = 0; i < 300; ++i) {
        value = {{"not", std::move(value)}};
    }
    const auto result = schema.validate(value);
    ASSERT_EQ(result.violations.size(), 1U);
    EXPECT_EQ(result.violations[0].message, "validation stopped: schema evaluation nests too deeply");
}

std::string first_message(const json& schema, const json& value) {
    const auto result = CompiledSchema::compile(schema).validate(value);
    return result.violations.empty() ? std::string() : result.violations[0].message;
}

json draft7(json schema) {
    schema["$schema"] = g_draft7;
    return schema;
}

// draft-07 asserts contentEncoding and contentMediaType.
TEST(SchemaContent, Draft7AssertsEncodingAndMediaType) {
    const auto base64 = draft7({{"contentEncoding", "base64"}});
    EXPECT_EQ(first_message(base64, "QUJDZGVm+/09"), "");
    EXPECT_EQ(first_message(base64, "QQ=="), "");
    EXPECT_EQ(first_message(base64, "QU*J"), "Content is not a base64 string");
    EXPECT_EQ(first_message(draft7({{"contentEncoding", "quoted-printable"}}), "x"),
              "Content encoding 'quoted-printable' cannot be checked");
    EXPECT_EQ(first_message(draft7({{"contentEncoding", ""}}), "*"), "");

    const auto json_text = draft7({{"contentMediaType", "application/json"}});
    EXPECT_EQ(first_message(json_text, "{}"), "");
    EXPECT_EQ(first_message(json_text, "{"), "Content is not JSON");
    EXPECT_EQ(first_message(draft7({{"contentMediaType", "text/plain"}}), "{"), "");

    const auto encoded_json =
        draft7({{"contentEncoding", "base64"}, {"contentMediaType", "application/json"}});
    EXPECT_EQ(first_message(encoded_json, "e30="), "");                     // {}
    EXPECT_EQ(first_message(encoded_json, "eyJh"), "Content is not JSON");  // {"a
    // Content that is not base64 is reported once; its media type is not checked.
    const auto result = CompiledSchema::compile(encoded_json).validate("*");
    ASSERT_EQ(result.violations.size(), 1U);
    EXPECT_EQ(result.violations[0].message, "Content is not a base64 string");
}

TEST(SchemaContent, LaterDialectsOnlyAnnotate) {
    EXPECT_EQ(
        first_message({{"contentEncoding", "base64"}, {"contentMediaType", "application/json"}}, "*"),
        "");
    EXPECT_EQ(first_message({{"$schema", g_draft201909}, {"contentEncoding", "base64"}}, "*"), "");
}

// With room for one violation, each keyword stops the validation at its first violation.
TEST(SchemaViolations, ALimitOfOneStopsAtTheFirstViolation) {
    const std::vector<std::pair<json, json>> cases = {
        {{{"maximum", 1}}, 5},
        {draft7({{"contentEncoding", "base64"}}), "*"},
        {draft7({{"contentEncoding", "quoted-printable"}}), "x"},
        {draft7({{"contentMediaType", "application/json"}}), "{"},
        {{{"contains", {{"type", "integer"}}}, {"maxContains", 1}}, {1, 2}},
        {{{"patternProperties", {{"^a", {{"type", "integer"}}}}}}, {{"ab", "x"}, {"ac", "y"}}},
        {{{"additionalProperties", false}}, {{"a", 1}, {"b", 1}}},
        {{{"dependentSchemas", {{"a", {{"required", {"b", "c"}}}}}}}, {{"a", 1}}},
        {{{"dependentRequired", {{"a", {"b", "c"}}}}}, {{"a", 1}}},
        {{{"maxProperties", 0}}, {{"a", 1}}},
        {{{"unevaluatedItems", false}}, {1, 2}},
        {{{"anyOf", {{{"required", {"a", "b", "c"}}}, {{"type", "string"}}}}}, json::object()},
    };
    mcp::SchemaOptions options;
    options.max_reported_violations = 1;
    for (const auto& [schema, value] : cases) {
        SCOPED_TRACE(schema.dump() + " against " + value.dump());
        const auto result = CompiledSchema::compile(schema, options).validate(value);
        EXPECT_FALSE(result.valid);
        EXPECT_EQ(result.violations.size(), 1U);
    }
}

// A failed anyOf reports its own violation, then as many of the first branch's as there is room for.
TEST(SchemaViolations, BranchViolationsFillTheRoomLeft) {
    mcp::SchemaOptions options;
    options.max_reported_violations = 2;
    const auto result =
        CompiledSchema::compile({{"anyOf", {{{"required", {"a", "b", "c"}}}, {{"type", "string"}}}}},
                                options)
            .validate(json::object());
    ASSERT_EQ(result.violations.size(), 2U);
    EXPECT_EQ(result.violations[0].message, "Value does not match any of the anyOf schemas");
    EXPECT_EQ(result.violations[1].message, "Required property 'a' is missing");
}

TEST(SchemaValues, UnusualValuesCompare) {
    // An integer above a double below -2^63.
    EXPECT_TRUE(CompiledSchema::compile({{"minimum", -1e19}}).validate(5).valid);
    EXPECT_FALSE(CompiledSchema::compile({{"maximum", -1e19}}).validate(5).valid);
    // uniqueItems over more than a few items hashes booleans and null too.
    const auto unique = CompiledSchema::compile({{"uniqueItems", true}});
    EXPECT_TRUE(unique.validate({true, false, nullptr, 1, 2, 3, 4, 5, 6, 7}).valid);
    EXPECT_FALSE(unique.validate({true, false, nullptr, 1, 2, 3, 4, 5, 6, true}).valid);
    EXPECT_FALSE(unique.validate({true, false, nullptr, 1, 2, 3, 4, 5, 6, nullptr}).valid);
    // nlohmann's binary values have no JSON type, so no type accepts them.
    const auto result = CompiledSchema::compile({{"type", "string"}}).validate(json::binary({1, 2}));
    ASSERT_EQ(result.violations.size(), 1U);
    EXPECT_NE(result.violations[0].message.find("found unknown"), std::string::npos)
        << result.violations[0].message;
}

// Pattern steps allowed per byte of text saturate instead of overflowing.
TEST(SchemaLimits, PatternStepsSaturate) {
    mcp::SchemaOptions options;
    options.pattern_steps_per_text_byte = std::numeric_limits<std::size_t>::max();
    EXPECT_TRUE(CompiledSchema::compile({{"pattern", "^a+$"}}, options).validate("aaa").valid);
}

// The cache holds at most 256 compiled schemas that are still in use; repeats of a schema holding
// arrays and booleans are found in it.
TEST(SchemaCache, HoldsABoundedNumberOfSchemas) {
    const json repeated = {{"enum", {json::array({1, true}), false}}, {"x-flags", {true, nullptr}}};
    const auto first = CompiledSchema::compile(repeated);
    const auto before = mcp::detail::compiled_schema_cache_live_entries();
    const auto again = CompiledSchema::compile(repeated);
    EXPECT_EQ(mcp::detail::compiled_schema_cache_live_entries(), before);
    EXPECT_EQ(&first.schema(), &again.schema());

    std::vector<CompiledSchema> held;
    for (int i = 0; i < 300; ++i) {
        held.push_back(CompiledSchema::compile({{"const", i}}));
    }
    EXPECT_LE(mcp::detail::compiled_schema_cache_live_entries(), 256U);
    EXPECT_TRUE(held.back().validate(299).valid);
}

}  // namespace

}  // namespace json_schema_compile_test

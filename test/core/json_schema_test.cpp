#include <mcp/auth/metadata_policy.hpp>
#include <mcp/protocol/schema.hpp>

#include <gtest/gtest.h>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <sys/resource.h>
#endif

namespace {

using mcp::CompiledSchema;
using mcp::SchemaError;
using nlohmann::json;

/// @p schema with "not": false added, which constrains nothing but keeps the schema off the native
/// evaluation, so it is evaluated by jsoncons.
json through_jsoncons(json schema) {
    schema["not"] = false;
    return schema;
}

bool is_valid(const json& schema, const json& instance) {
    return CompiledSchema::compile(schema).validate(instance).valid;
}

/// Whether validating @p value against @p schema stops for want of work, given @p work to spend.
bool stops_for_work(const json& schema, const json& value, std::size_t work) {
    mcp::SchemaOptions options;
    options.max_evaluation_work = work;
    const auto result = CompiledSchema::compile(schema, options).validate(value);
    return !result.valid && result.violations.size() == 1U &&
           result.violations[0].message.find("takes too much work") != std::string::npos;
}

SchemaError::Category compile_error(const json& schema) {
    try {
        (void)CompiledSchema::compile(schema);
    } catch (const SchemaError& error) {
        return error.category();
    }
    ADD_FAILURE() << "schema compiled: " << schema.dump();
    return SchemaError::Category::eInvalidSchema;
}

std::string compile_message(const json& schema) {
    try {
        (void)CompiledSchema::compile(schema);
    } catch (const SchemaError& error) {
        return error.what();
    }
    return {};
}

TEST(SchemaDialect, DefaultIs2020_12WithoutDollarSchema) {
    // prefixItems exists only in 2020-12; under draft-07 it would be an ignored unknown keyword.
    const json schema = {{"prefixItems", {{{"type", "integer"}}}}};
    EXPECT_TRUE(is_valid(schema, json::array({1, "x"})));
    EXPECT_FALSE(is_valid(schema, json::array({"x"})));
}

TEST(SchemaDialect, DeclaredDraft07IsHonored) {
    // Under draft-07 an array-valued "items" is positional; 2020-12 would reject that form.
    const json schema = {{"$schema", "http://json-schema.org/draft-07/schema#"},
                         {"items", {{{"type", "integer"}}}}};
    EXPECT_TRUE(is_valid(schema, json::array({1, "anything"})));
    EXPECT_FALSE(is_valid(schema, json::array({"x"})));
}

TEST(SchemaDialect, DialectWithoutTrailingFragmentIsRecognized) {
    EXPECT_NO_THROW(
        (void)CompiledSchema::compile({{"$schema", "http://json-schema.org/draft-07/schema"}}));
    EXPECT_NO_THROW(
        (void)CompiledSchema::compile({{"$schema", "https://json-schema.org/draft/2020-12/schema#"}}));
}

TEST(SchemaDialect, EverySupportedDialectCompiles) {
    for (const char* dialect :
         {"https://json-schema.org/draft/2020-12/schema",
          "https://json-schema.org/draft/2019-09/schema", "http://json-schema.org/draft-07/schema#",
          "http://json-schema.org/draft-06/schema#", "http://json-schema.org/draft-04/schema#"}) {
        EXPECT_NO_THROW((void)CompiledSchema::compile({{"$schema", dialect}, {"type", "object"}}))
            << dialect;
    }
}

TEST(SchemaDialect, Keywords2020_12) {
    const json dynamic = json::parse(R"({
        "$id": "https://example.com/tree",
        "$dynamicAnchor": "node",
        "type": "object",
        "properties": {"children": {"type": "array", "items": {"$dynamicRef": "#node"}}},
        "unevaluatedProperties": false
    })");
    EXPECT_TRUE(is_valid(dynamic, json::parse(R"({"children": [{"children": []}]})")));
    EXPECT_FALSE(is_valid(dynamic, json::parse(R"({"children": [{"extra": 1}]})")));

    const json unevaluated_items = {{"prefixItems", {{{"type", "string"}}}},
                                    {"unevaluatedItems", false}};
    EXPECT_TRUE(is_valid(unevaluated_items, json::array({"a"})));
    EXPECT_FALSE(is_valid(unevaluated_items, json::array({"a", "b"})));

    const json dependent = {
        {"dependentSchemas", {{"credit_card", {{"required", {"billing_address"}}}}}}};
    EXPECT_TRUE(is_valid(dependent, json::object()));
    EXPECT_FALSE(is_valid(dependent, {{"credit_card", 1}}));

    const json contains = {{"contains", {{"type", "integer"}}}, {"minContains", 2}};
    EXPECT_TRUE(is_valid(contains, json::array({1, "a", 2})));
    EXPECT_FALSE(is_valid(contains, json::array({1, "a"})));

    const json defs = json::parse(R"({"$defs": {"positive": {"type": "integer", "minimum": 1}},
                                      "$ref": "#/$defs/positive"})");
    EXPECT_TRUE(is_valid(defs, 3));
    EXPECT_FALSE(is_valid(defs, 0));
}

TEST(SchemaDialect, UnknownDollarSchemaIsUnsupportedDialectError) {
    EXPECT_EQ(compile_error({{"$schema", "https://example.com/my-dialect"}}),
              SchemaError::Category::eUnsupportedDialect);
    EXPECT_EQ(compile_error({{"$schema", "http://json-schema.org/draft-03/schema#"}}),
              SchemaError::Category::eUnsupportedDialect);
}

TEST(SchemaDialect, MetaschemaViolationIsInvalidSchema) {
    EXPECT_EQ(compile_error({{"type", "integr"}}), SchemaError::Category::eInvalidSchema);
    EXPECT_EQ(compile_error({{"minLength", -1}}), SchemaError::Category::eInvalidSchema);
    EXPECT_EQ(compile_error({{"required", "name"}}), SchemaError::Category::eInvalidSchema);
    EXPECT_EQ(compile_error({{"$schema", 7}}), SchemaError::Category::eInvalidSchema);
}

// The 2020-12 metaschema wraps its vocabularies in allOf; the message names the problem inside.
TEST(SchemaDialect, MetaschemaViolationNamesTheInnermostProblem) {
    const auto message = compile_message({{"type", "object"}, {"properties", nullptr}});
    EXPECT_NE(message.find("'/properties'"), std::string::npos) << message;
    EXPECT_NE(message.find("found null"), std::string::npos) << message;
}

TEST(SchemaDialect, NullAndNonObjectSchemasRejected) {
    EXPECT_EQ(compile_error(nullptr), SchemaError::Category::eInvalidSchema);
    EXPECT_EQ(compile_error(json::array()), SchemaError::Category::eInvalidSchema);
    EXPECT_EQ(compile_error("schema"), SchemaError::Category::eInvalidSchema);
}

TEST(SchemaDialect, BooleanSchemaAccepted) {
    EXPECT_TRUE(is_valid(true, json::object()));
    EXPECT_FALSE(is_valid(false, json::object()));
}

TEST(SchemaDialect, UnsupportedPatternSyntaxHasItsOwnCategory) {
    EXPECT_EQ(compile_error({{"pattern", "(a)\\1"}}), SchemaError::Category::eUnsupportedPattern);
    EXPECT_EQ(compile_error({{"pattern", "^(?=x)"}}), SchemaError::Category::eUnsupportedPattern);
    EXPECT_EQ(compile_error({{"patternProperties", {{"\\p{L}", true}}}}),
              SchemaError::Category::eUnsupportedPattern);
}

// `$schema` names the dialect wherever it appears; an embedded resource that names one the SDK does
// not implement is an unsupported dialect, not a reference that failed to resolve.
TEST(SchemaDialect, UnsupportedDialectOfAnEmbeddedResourceIsReportedAsSuch) {
    const json schema = {{"type", "object"},
                         {"$defs",
                          {{"inner",
                            {{"$id", "https://example.com/inner"},
                             {"$schema", "https://example.com/dialect"},
                             {"type", "string"}}}}},
                         {"properties", {{"a", {{"$ref", "https://example.com/inner"}}}}}};
    EXPECT_EQ(compile_error(schema), SchemaError::Category::eUnsupportedDialect)
        << compile_message(schema);
    EXPECT_NE(compile_message(schema).find("https://example.com/dialect"), std::string::npos);
}

TEST(SchemaValidation, ReportsViolationsWithInstancePaths) {
    const auto schema = CompiledSchema::compile(json::parse(R"({
        "type": "object",
        "properties": {"age": {"type": "integer", "minimum": 0}, "name": {"type": "string"}},
        "required": ["name"]
    })"));
    const auto result = schema.validate({{"age", -1}});
    EXPECT_FALSE(result.valid);
    ASSERT_FALSE(result.violations.empty());
    std::set<std::string> paths;
    for (const auto& violation : result.violations) {
        paths.insert(violation.instance_path);
        EXPECT_FALSE(violation.message.empty());
    }
    EXPECT_TRUE(paths.count("/age") == 1) << "paths reported: " << paths.size();
}

TEST(SchemaValidation, ViolationCountIsBounded) {
    mcp::SchemaOptions options;
    options.max_reported_violations = 2;
    json properties = json::object();
    json instance = json::object();
    for (int i = 0; i < 10; ++i) {
        properties["p" + std::to_string(i)] = {{"type", "string"}};
        instance["p" + std::to_string(i)] = i;
    }
    const auto result =
        CompiledSchema::compile({{"properties", properties}}, options).validate(instance);
    EXPECT_FALSE(result.valid);
    EXPECT_EQ(result.violations.size(), 2U);
}

TEST(SchemaValidation, ViolationTextIsFlattenedAndBounded) {
    const auto schema = CompiledSchema::compile({{"pattern", "^a+$"}});
    const std::string value = "line one\nforged line\r\n" + std::string(100'000, 'b');
    const auto result = schema.validate(value);
    ASSERT_FALSE(result.valid);
    ASSERT_FALSE(result.violations.empty());
    const auto& message = result.violations.front().message;
    EXPECT_EQ(message.find('\n'), std::string::npos);
    EXPECT_EQ(message.find('\r'), std::string::npos);
    EXPECT_LT(message.size(), std::size_t{1024});
}

TEST(SchemaValidation, InstanceDeeperThanTheLimitIsInvalidWithoutRecursing) {
    mcp::SchemaOptions options;
    options.max_instance_depth = 8;
    const auto schema = CompiledSchema::compile(json::parse(R"({"items": {"$ref": "#"}})"), options);
    json shallow = json::array();
    for (int i = 0; i < 7; ++i) {
        shallow = json::array({shallow});
    }
    EXPECT_TRUE(schema.validate(shallow).valid);
    json deep = json::array();
    for (int i = 0; i < 100'000; ++i) {
        deep = json::array({std::move(deep)});
    }
    const auto result = schema.validate(deep);
    EXPECT_FALSE(result.valid);
    ASSERT_EQ(result.violations.size(), 1U);
    EXPECT_EQ(result.violations.front().instance_path, "");
}

TEST(SchemaValidation, DeeplyNestedSchemaIsRejected) {
    json schema = {{"type", "object"}};
    for (int i = 0; i < 40; ++i) {
        schema = {{"properties", {{"a", std::move(schema)}}}};
    }
    EXPECT_EQ(compile_error(schema), SchemaError::Category::eInvalidSchema);
}

TEST(SchemaRefPolicy, LocalFragmentAndAnchorResolve) {
    const json schema = json::parse(R"({
        "$defs": {"positive": {"type": "integer", "minimum": 1},
                  "named": {"$anchor": "word", "type": "string"}},
        "properties": {"n": {"$ref": "#/$defs/positive"}, "w": {"$ref": "#word"}}
    })");
    EXPECT_TRUE(is_valid(schema, {{"n", 2}, {"w", "x"}}));
    EXPECT_FALSE(is_valid(schema, {{"n", 0}}));
    EXPECT_FALSE(is_valid(schema, {{"w", 1}}));
}

// "#/definitions/name" is a JSON Pointer into the document, so it resolves in every dialect. The
// 2019-09 and 2020-12 metaschemas keep `definitions` next to `$defs`, and schemas written for
// draft-07 keep their subschemas there.
TEST(SchemaRefPolicy, PointerIntoDefinitionsResolvesInEveryDialect) {
    for (const char* dialect :
         {"", "https://json-schema.org/draft/2020-12/schema",
          "https://json-schema.org/draft/2019-09/schema", "http://json-schema.org/draft-07/schema#"}) {
        SCOPED_TRACE(dialect);
        json schema = {{"definitions", {{"name", {{"type", "string"}}}}},
                       {"properties", {{"a", {{"$ref", "#/definitions/name"}}}}}};
        if (*dialect != '\0') {
            schema["$schema"] = dialect;
        }
        try {
            const auto compiled = CompiledSchema::compile(schema);
            EXPECT_TRUE(compiled.validate({{"a", "x"}}).valid);
            EXPECT_FALSE(compiled.validate({{"a", 5}}).valid);
        } catch (const SchemaError& error) {
            ADD_FAILURE() << error.what();
        }
    }
}

// `definitions` and `$defs` are separate locations, so the same name under both is two subschemas.
TEST(SchemaRefPolicy, SameNameUnderDefinitionsAndDefsKeepsBothSubschemas) {
    for (const char* dialect : {"", "https://json-schema.org/draft/2020-12/schema",
                                "https://json-schema.org/draft/2019-09/schema"}) {
        SCOPED_TRACE(dialect);
        json schema = {{"$defs", {{"name", {{"type", "string"}}}}},
                       {"definitions", {{"name", {{"type", "integer"}}}}},
                       {"properties",
                        {{"a", {{"$ref", "#/$defs/name"}}}, {"b", {{"$ref", "#/definitions/name"}}}}}};
        if (*dialect != '\0') {
            schema["$schema"] = dialect;
        }
        try {
            const auto compiled = CompiledSchema::compile(schema);
            EXPECT_TRUE(compiled.validate({{"a", "x"}, {"b", 5}}).valid);
            EXPECT_FALSE(compiled.validate({{"a", 5}}).valid);
            EXPECT_FALSE(compiled.validate({{"b", "x"}}).valid);
        } catch (const SchemaError& error) {
            ADD_FAILURE() << error.what();
        }
    }
    // The anchor makes the `$defs` subschema register a reference target of its own.
    const auto anchored =
        json::parse(R"({"$defs":{"a":{"$dynamicAnchor":"a"}},"definitions":{"a":true},"if":true})");
    EXPECT_TRUE(CompiledSchema::compile(anchored).validate(json::object()).valid);
}

TEST(SchemaRefPolicy, EmbeddedIdResolvesWithoutFetching) {
    const json schema = json::parse(R"({
        "$id": "https://example.com/root.json",
        "$defs": {"item": {"$id": "item.json", "type": "string"}},
        "items": {"$ref": "item.json"}
    })");
    EXPECT_TRUE(is_valid(schema, {"a", "b"}));
    EXPECT_FALSE(is_valid(schema, {"a", 1}));
}

TEST(SchemaRefPolicy, MetaschemaReferenceResolves) {
    const json schema = {{"$ref", "https://json-schema.org/draft/2020-12/schema"}};
    EXPECT_TRUE(is_valid(schema, {{"type", "string"}}));
    EXPECT_FALSE(is_valid(schema, {{"type", 5}}));
}

// A $ref to a remote schema is refused at compile time and no connection is attempted: a listener
// on the referenced port never sees one.
TEST(SchemaRefPolicy, HttpReferenceIsUnresolvedWithoutConnecting) {
    boost::asio::io_context io_ctx;
    boost::asio::ip::tcp::acceptor acceptor(io_ctx, {boost::asio::ip::make_address("127.0.0.1"), 0});
    acceptor.non_blocking(true);
    const auto url =
        "http://127.0.0.1:" + std::to_string(acceptor.local_endpoint().port()) + "/schema.json";
    const json schema = {{"properties", {{"a", {{"$ref", url}}}}}};
    EXPECT_EQ(compile_error(schema), SchemaError::Category::eUnresolvedRef);
    EXPECT_NE(compile_message(schema).find(url), std::string::npos);
    boost::system::error_code accepted;
    boost::asio::ip::tcp::socket socket(io_ctx);
    acceptor.accept(socket, accepted);
    EXPECT_EQ(accepted, boost::asio::error::would_block);
}

TEST(SchemaRefPolicy, FileAndRelativeReferencesAreUnresolved) {
    EXPECT_EQ(compile_error({{"$ref", "file:///etc/passwd"}}), SchemaError::Category::eUnresolvedRef);
    EXPECT_EQ(compile_error({{"$ref", "other.json"}}), SchemaError::Category::eUnresolvedRef);
    EXPECT_EQ(compile_error({{"$ref", "other.json#/$defs/x"}}), SchemaError::Category::eUnresolvedRef);
}

TEST(SchemaRefPolicy, MissingLocalTargetIsUnresolved) {
    EXPECT_EQ(compile_error({{"$ref", "#/$defs/missing"}}), SchemaError::Category::eUnresolvedRef);
    EXPECT_EQ(compile_error({{"$ref", "#nope"}}), SchemaError::Category::eUnresolvedRef);
    // Reported as written, not against the base URI jsoncons assumes for a document without $id.
    const auto message = compile_message({{"$ref", "#/$defs/missing"}});
    EXPECT_NE(message.find(": #/$defs/missing"), std::string::npos) << message;
    EXPECT_EQ(message.find("jsoncons"), std::string::npos) << message;
}

// Each of these schemas evaluates itself again without descending into the value, which would
// recurse until the stack overflows. They are refused when compiled.
TEST(SchemaLimits, NonDescendingReferenceCyclesAreRefused) {
    for (
        const auto* text : {
            R"({"$ref": "#"})",
            R"({"allOf": [{"$ref": "#"}]})",
            R"({"anyOf": [{"$ref": "#"}]})",
            R"({"$defs": {"a": {"$ref": "#/$defs/b"}, "b": {"$ref": "#/$defs/a"}}, "$ref": "#/$defs/a"})",
            R"({"$dynamicAnchor": "n", "$dynamicRef": "#n"})",
            R"({"not": {"$ref": "#"}})",
        }) {
        SCOPED_TRACE(text);
        EXPECT_EQ(compile_error(json::parse(text)), SchemaError::Category::eInvalidSchema);
    }
}

// Two branches per level that both descend give 2^depth evaluations. The work budget stops it
// long before that finishes; the value is reported as invalid. 30 levels stay within the
// evaluation path bound, so it is the work budget that stops this.
TEST(SchemaLimits, ExponentialEvaluationIsStopped) {
    const auto schema = CompiledSchema::compile(json::parse(R"({
        "anyOf": [{"items": {"$ref": "#"}, "maxItems": 0}, {"items": {"$ref": "#"}}]
    })"));
    json value = json::array();
    for (int i = 0; i < 30; ++i) {
        value = json::array({std::move(value)});
    }
    const auto result = schema.validate(value);
    EXPECT_FALSE(result.valid);
    ASSERT_EQ(result.violations.size(), 1U);
    EXPECT_NE(result.violations.front().message.find("too many steps"), std::string::npos)
        << result.violations.front().message;
}

// Each value of the array is compared with each value of the enum. No subschema is entered for a
// comparison, so nothing but a charge per comparison stops this.
TEST(SchemaLimits, EnumComparisonsAreCharged) {
    json allowed = json::array();
    for (int i = 0; i < 500; ++i) {
        allowed.push_back(1000000 + i);
    }
    const json schema = {{"items", {{"enum", allowed}}}};
    // The last value of the enum, so every comparison runs and no item is a violation: 250,000
    // comparisons at 8 units each, which a million units do not pay for and ten million do.
    const json value(500, allowed.back());
    const std::vector<json> schemas{schema, through_jsoncons(schema)};
    for (const auto& each : schemas) {
        EXPECT_TRUE(stops_for_work(each, value, 1'000'000));
        EXPECT_FALSE(stops_for_work(each, value, 10'000'000));
    }
}

// A long string that differs from each long enum value only at its end costs its length per value.
// Every item is the last enum value, so no item is a violation and every comparison runs.
TEST(SchemaLimits, EnumComparisonsOfLongStringsAreCharged) {
    const std::string text(1U << 16U, 'x');
    json allowed = json::array();
    for (int i = 0; i < 2000; ++i) {
        allowed.push_back(text + std::to_string(1000 + i));
    }
    const auto result =
        CompiledSchema::compile({{"items", {{"enum", allowed}}}}).validate(json(2000, allowed.back()));
    EXPECT_FALSE(result.valid);
    ASSERT_EQ(result.violations.size(), 1U);
    EXPECT_NE(result.violations.front().message.find("too much work"), std::string::npos)
        << result.violations.front().message;
}

#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) ||                             \
    __has_feature(memory_sanitizer)
#define MCP_TEST_SANITIZER_SHADOW_MEMORY 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define MCP_TEST_SANITIZER_SHADOW_MEMORY 1
#endif

#if defined(__linux__) && !defined(MCP_TEST_SANITIZER_SHADOW_MEMORY)
// A report may quote the whole value, and every branch of an anyOf that fails keeps its report, so
// without a cut this holds 1 MiB per branch. Reports are sanitized on their way out, so only the
// peak shows it. A peak that an earlier test in the process set can hide this, never fail it.
TEST(SchemaLimits, FailedBranchesDoNotKeepCopiesOfTheValue) {
    const auto peak_kib = [] {
        rusage usage{};
        EXPECT_EQ(getrusage(RUSAGE_SELF, &usage), 0);
        return usage.ru_maxrss;
    };
    json branches = json::array();
    for (int i = 0; i < 2000; ++i) {
        branches.push_back({{"enum", {i}}});
    }
    const auto compiled = CompiledSchema::compile({{"anyOf", branches}});
    const std::string text(1U << 20U, 'x');
    const auto before = peak_kib();
    EXPECT_FALSE(compiled.validate(text).valid);
    EXPECT_LT(peak_kib() - before, 256L * 1024L);
}
#endif

#if defined(__GLIBCXX__) && __SIZEOF_SIZE_T__ == 8 && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
/// @p count distinct 16-byte ASCII strings with one std::hash value. libstdc++ hashes a string with
/// Murmur64A, whose block step can be inverted, so a second block can be solved for any first one.
std::vector<std::string> strings_with_one_hash(std::size_t count) {
    constexpr std::uint64_t g_multiplier = 0xc6a4a7935bd1e995ULL;
    constexpr std::uint64_t g_seed = 0xc70f6907ULL;
    constexpr std::uint64_t g_length = 16;
    constexpr std::uint64_t g_target = 0x1234567890abcdefULL;
    std::uint64_t inverse = g_multiplier;
    for (int i = 0; i < 6; ++i) {
        inverse *= 2 - g_multiplier * inverse;
    }
    const auto shift_mix = [](std::uint64_t value) { return value ^ (value >> 47U); };

    std::vector<std::string> strings;
    std::mt19937_64 random(5);
    while (strings.size() < count) {
        std::uint64_t first = random() & 0x7f7f7f7f7f7f7f7fULL;
        const std::uint64_t state =
            ((g_seed ^ (g_length * g_multiplier)) ^ (shift_mix(first * g_multiplier) * g_multiplier)) *
            g_multiplier;
        const std::uint64_t needed = state ^ (g_target * inverse);
        std::uint64_t second = shift_mix(needed * inverse) * inverse;
        if ((second & 0x8080808080808080ULL) != 0) {
            continue;
        }
        std::string text(16, '\0');
        std::memcpy(text.data(), &first, sizeof first);
        std::memcpy(text.data() + sizeof first, &second, sizeof second);
        strings.push_back(std::move(text));
    }
    return strings;
}

// uniqueItems sorts by hash and compares only equal hashes, which is linear until someone chooses
// strings that all hash alike.
TEST(SchemaLimits, UniqueItemsOfCollidingStringsAreCharged) {
    const auto strings = strings_with_one_hash(2000);
    ASSERT_EQ(std::set<std::string>(strings.begin(), strings.end()).size(), strings.size());
    const std::hash<std::string> hash;
    for (const auto& text : strings) {
        ASSERT_EQ(hash(text), hash(strings.front()));
    }

    const auto result =
        CompiledSchema::compile({{"uniqueItems", true}, {"not", false}}).validate(json(strings));
    EXPECT_FALSE(result.valid);
    ASSERT_EQ(result.violations.size(), 1U);
    EXPECT_NE(result.violations.front().message.find("too much work"), std::string::npos)
        << result.violations.front().message;
}

// Elements that share a hash are compared in full, so a comparison costs what the elements hold.
// 200 arrays of 201 values share one hash and differ only in their last value: about 20,000
// comparisons of 201 values each.
TEST(SchemaLimits, UniqueItemsComparisonsAreChargedByWhatTheyHold) {
    const auto strings = strings_with_one_hash(200);
    json value = json::array();
    for (const auto& text : strings) {
        json element(200, 0);
        element.push_back(text);
        value.push_back(std::move(element));
    }
    const auto stops = [&value](std::size_t work) {
        mcp::SchemaOptions options;
        options.max_evaluation_work = work;
        const auto result = CompiledSchema::compile({{"uniqueItems", true}}, options).validate(value);
        return !result.valid && result.violations.size() == 1U &&
               result.violations[0].message.find("takes too much work") != std::string::npos;
    };
    EXPECT_FALSE(stops(100'000'000));
    EXPECT_TRUE(stops(1'000'000));
}

// Sets of property names are ordered, so names chosen to share a hash cost no more to look up than
// any others. Timed against the same number of ordinary names, so the bound holds in slow builds.
TEST(SchemaLimits, PropertyNamesThatShareAHashAreLookedUpQuickly) {
    const auto colliding = strings_with_one_hash(20000);
    std::vector<std::string> ordinary;
    for (std::size_t i = 0; i < colliding.size(); ++i) {
        ordinary.push_back("k" + std::to_string(1'000'000'000'000'000 + i));
    }
    const auto object_of = [](const std::vector<std::string>& names) {
        json object = json::object();
        for (const auto& name : names) {
            object[name] = 1;
        }
        return object;
    };
    const auto closed = CompiledSchema::compile(
        through_jsoncons({{"patternProperties", {{"^[\\x00-\\x7f]", {{"type", "integer"}}}}},
                          {"additionalProperties", false}}));
    const auto unevaluated =
        CompiledSchema::compile({{"anyOf", {{{"additionalProperties", {{"type", "integer"}}}}}},
                                 {"unevaluatedProperties", false}});
    for (const auto* schema : {&closed, &unevaluated}) {
        const auto time = [schema](const json& value) {
            const auto start = std::chrono::steady_clock::now();
            EXPECT_TRUE(schema->validate(value).valid);
            return std::chrono::steady_clock::now() - start;
        };
        const auto baseline_time = time(object_of(ordinary));
        const auto colliding_time = time(object_of(colliding));
        EXPECT_LT(colliding_time, 20 * baseline_time + std::chrono::milliseconds(500))
            << "baseline "
            << std::chrono::duration_cast<std::chrono::milliseconds>(baseline_time).count() << " ms";
    }
}
#endif

// A recursive schema through anyOf adds several keywords to the evaluation path per level of the
// value, so it reaches the path bound before max_instance_depth; the value is then invalid.
TEST(SchemaLimits, EvaluationPathIsBounded) {
    const auto schema = CompiledSchema::compile(json::parse(R"({
        "anyOf": [{"type": "array", "items": {"$ref": "#"}}, {"type": "null"}]
    })"));
    json value = nullptr;
    for (int i = 0; i < 40; ++i) {
        value = json::array({std::move(value)});
    }
    EXPECT_TRUE(schema.validate(value).valid);
    for (int i = 40; i < 63; ++i) {
        value = json::array({std::move(value)});
    }
    const auto result = schema.validate(value);
    EXPECT_FALSE(result.valid);
    ASSERT_EQ(result.violations.size(), 1U);
    EXPECT_NE(result.violations.front().message.find("nests too deeply"), std::string::npos)
        << result.violations.front().message;
}

// Values within the depth limit, and large flat values, validate normally under the limits.
TEST(SchemaLimits, OrdinaryValuesStayWithinTheLimits) {
    const auto nested =
        CompiledSchema::compile(json::parse(R"({"type": "array", "items": {"$ref": "#"}})"));
    json deep = json::array();
    for (int i = 0; i < 63; ++i) {
        deep = json::array({std::move(deep)});
    }
    EXPECT_TRUE(nested.validate(deep).valid);

    const auto records = CompiledSchema::compile(json::parse(R"({
        "type": "array",
        "items": {"type": "object", "required": ["id", "tags"],
                  "properties": {"id": {"type": "integer"},
                                 "name": {"type": "string", "pattern": "^[a-z0-9-]+$"},
                                 "tags": {"type": "array", "items": {"enum": ["a", "b", "c"]}}},
                  "additionalProperties": false}
    })"));
    json many = json::array();
    for (int i = 0; i < 20'000; ++i) {
        many.push_back({{"id", i}, {"name", "item-" + std::to_string(i)}, {"tags", {"a", "c"}}});
    }
    EXPECT_TRUE(records.validate(many).valid);

    // Any JSON value, through anyOf and $ref at every level: the most work per node and level
    // among ordinary schemas.
    const auto any_value = CompiledSchema::compile(json::parse(R"({
        "$defs": {"v": {"anyOf": [
            {"type": "null"}, {"type": "boolean"}, {"type": "number"}, {"type": "string"},
            {"type": "array", "items": {"$ref": "#/$defs/v"}},
            {"type": "object", "additionalProperties": {"$ref": "#/$defs/v"}}]}},
        "$ref": "#/$defs/v"
    })"));
    const json some(many.begin(), many.begin() + 2'000);
    EXPECT_TRUE(any_value.validate(some).valid);
    json wide_and_deep = json::array();
    for (int i = 0; i < 200; ++i) {
        wide_and_deep.push_back({{"id", i}, {"tags", {"a", "b"}}});
    }
    for (int i = 0; i < 40; ++i) {
        wide_and_deep = json::array({std::move(wide_and_deep), i});
    }
    EXPECT_TRUE(any_value.validate(wide_and_deep).valid);
}

TEST(SchemaLimits, SchemaNestingWithinTheMetaschemaBoundCompiles) {
    json schema = {{"type", "string"}};
    for (int i = 0; i < 26; ++i) {
        schema = {{"type", "object"}, {"properties", {{"c", std::move(schema)}}}};
    }
    EXPECT_NO_THROW((void)CompiledSchema::compile(schema));
    schema = {{"type", "object"}, {"properties", {{"c", std::move(schema)}}}};
    EXPECT_EQ(compile_error(schema), SchemaError::Category::eInvalidSchema);
}

// Pattern matching is linear in the input, but a pattern that runs many threads at once, such as
// an unanchored ".{1,1000}x", costs thousands of steps per character. A validation's matching is
// budgeted in proportion to the text in the value; past the budget the value is reported invalid.
// A small budget keeps the test fast in sanitizer builds.
TEST(SchemaLimits, PatternMatchingIsBounded) {
    mcp::SchemaOptions options;
    options.base_pattern_steps = 100'000;
    options.pattern_steps_per_text_byte = 64;
    // Matching all of it would take about 10 million steps; the budget for it is 420,000.
    const std::string long_text(5000, 'a');
    for (const bool through_jsoncons : {false, true}) {
        SCOPED_TRACE(through_jsoncons ? "jsoncons" : "native");
        json costly = {{"type", "string"}, {"pattern", ".{1,1000}x"}};
        json cheap = {{"type", "string"}, {"pattern", "^[a-z]+$"}};
        if (through_jsoncons) {
            costly["$defs"] = json::object();
            cheap["$defs"] = json::object();
        }
        const auto result = CompiledSchema::compile(costly, options).validate(long_text);
        ASSERT_FALSE(result.valid);
        ASSERT_EQ(result.violations.size(), 1U);
        EXPECT_EQ(result.violations[0].message,
                  "validation stopped: pattern matching takes too many steps");

        EXPECT_TRUE(CompiledSchema::compile(cheap, options).validate(long_text).valid);
        EXPECT_TRUE(
            CompiledSchema::compile(costly, options).validate(std::string(100, 'a') + "x").valid);
    }
    // Property names are matched by patternProperties, which only jsoncons evaluates.
    const auto keys = CompiledSchema::compile({{"patternProperties", {{".{1,1000}x", true}}}}, options);
    const auto result = keys.validate({{long_text, 1}});
    ASSERT_FALSE(result.valid);
    EXPECT_EQ(result.violations[0].message,
              "validation stopped: pattern matching takes too many steps");
}

// uniqueItems must not compare every pair of elements: a request-sized array would then take time
// quadratic in its length, outside the evaluation work budget. The check is timed against
// validating the same array through jsoncons without it, so the bound holds in slow builds too.
TEST(SchemaLimits, UniqueItemsOnALargeArrayTakesBoundedTime) {
    const auto unique = CompiledSchema::compile({{"uniqueItems", true}});
    const auto baseline = CompiledSchema::compile({{"$defs", json::object()}, {"type", "array"}});
    json items = json::array();
    for (int i = 0; i < 100'000; ++i) {
        items.push_back(i);
    }
    const auto time = [&items](const CompiledSchema& schema) {
        const auto start = std::chrono::steady_clock::now();
        EXPECT_TRUE(schema.validate(items).valid);
        return std::chrono::steady_clock::now() - start;
    };
    const auto baseline_time = time(baseline);
    const auto unique_time = time(unique);
    EXPECT_LT(unique_time, 20 * baseline_time + std::chrono::milliseconds(500))
        << "baseline " << std::chrono::duration_cast<std::chrono::milliseconds>(baseline_time).count()
        << " ms";

    items.push_back(json::array({1, {{"a", 2}}}));
    items.push_back(json::array({1.0, {{"a", 2.0}}}));
    EXPECT_FALSE(unique.validate(items).valid);
    items.erase(items.size() - 1);
    items.push_back(-0.0);
    EXPECT_FALSE(unique.validate(items).valid);
}

// Every step under a property copies the instance location, so a long name above many items would
// cost its length once per item. Locations keep the first 1024 bytes of a name, which is more than
// a reported location can show, so the report is the same as for the full name.
TEST(SchemaLimits, LongPropertyNameAboveManyItemsIsCheap) {
    const std::string name(1U << 20U, 'k');
    json value = {{"m", {{name, json(2000, 0)}}}};
    const json schema = {
        {"type", "object"},
        {"properties",
         {{"m",
           {{"type", "object"},
            {"additionalProperties", {{"type", "array"}, {"items", {{"type", "integer"}}}}}}}}}};
    mcp::SchemaOptions options;
    options.max_evaluation_work = 10'000'000;
    for (const auto& each : {schema, through_jsoncons(schema)}) {
        const auto compiled = CompiledSchema::compile(each, options);
        EXPECT_TRUE(compiled.validate(value).valid);
        value["m"][name][5] = "x";
        const auto result = compiled.validate(value);
        value["m"][name][5] = 0;
        ASSERT_EQ(result.violations.size(), 1U);
        EXPECT_EQ(result.violations[0].instance_path,
                  mcp::auth::detail::sanitize_for_diagnostics("/m/" + name + "/5"));
    }
}

// Names up to 1024 bytes are kept whole, so a deep path of them makes each copy of a location long.
// Copies are charged by their length, so a small budget stops what a large one allows.
TEST(SchemaLimits, LocationCopiesAreCharged) {
    json value = json(3000, 0);
    for (int i = 0; i < 30; ++i) {
        const std::string name = std::to_string(i) + std::string(1000, 'k');
        value = {{name, std::move(value)}};
    }
    const json schema = through_jsoncons(json::parse(R"({
        "type": ["object", "array"], "additionalProperties": {"$ref": "#"}, "items": {"type": "integer"}
    })"));
    EXPECT_TRUE(CompiledSchema::compile(schema).validate(value).valid);
    mcp::SchemaOptions options;
    options.max_evaluation_work = 4'000'000;
    const auto result = CompiledSchema::compile(schema, options).validate(value);
    EXPECT_FALSE(result.valid);
    ASSERT_EQ(result.violations.size(), 1U);
    EXPECT_EQ(result.violations[0].message,
              "validation stopped: schema evaluation takes too much work");
}

// A branching keyword counts every message its branches report but keeps only the first few, each
// holding its location. 2000 failed branches under a long path would hold 40 MB.
TEST(SchemaLimits, BranchesKeepAFewOfTheirMessages) {
    json branches = json::array({{{"type", "object"}, {"additionalProperties", {{"$ref", "#"}}}}});
    for (int i = 0; i < 2000; ++i) {
        branches.push_back({{"enum", {i}}});
    }
    const json schema = through_jsoncons({{"anyOf", branches}});
    json value = 99999;
    for (int i = 0; i < 20; ++i) {
        value = {{std::to_string(i) + std::string(1000, 'k'), std::move(value)}};
    }
    const auto result = CompiledSchema::compile(schema).validate(value);
    EXPECT_FALSE(result.valid);
    ASSERT_EQ(result.violations.size(), 1U);
    EXPECT_EQ(result.violations[0].message,
              "Must be valid against at least one schema, but found no matching schemas");
}

// Branches that fail by recursing keep messages that hold the messages under them, so the kept
// messages grow exponentially with the value's depth. What they hold is bounded.
TEST(SchemaLimits, MemoryOfKeptMessagesIsBounded) {
    const auto schema = CompiledSchema::compile(json::parse(R"({
        "$defs": {"a": {"anyOf": [{"type": "array", "items": {"$ref": "#/$defs/a"}},
                                  {"type": "array", "items": {"$ref": "#/$defs/a"}},
                                  {"type": "string"}]}},
        "properties": {"deep": {"$ref": "#/$defs/a"}}
    })"));
    json deep = 1;
    for (int i = 0; i < 40; ++i) {
        deep = json::array({std::move(deep)});
    }
    // Text the schema never reads raises the work allowed, so that memory runs out first.
    const auto result =
        schema.validate({{"padding", std::string(8U << 20U, 'x')}, {"deep", std::move(deep)}});
    EXPECT_FALSE(result.valid);
    ASSERT_EQ(result.violations.size(), 1U);
    EXPECT_EQ(result.violations[0].message,
              "validation stopped: the messages kept for failed branches take too much memory");
}

// A message names the value it rejects. Quoting all of it would make each failed branch cost time in
// proportion to the value; a structured value is named by its size, a string by an excerpt.
TEST(SchemaValidation, MessagesNameLargeValuesBriefly) {
    const auto compiled = CompiledSchema::compile(through_jsoncons({{"enum", {1, 2}}}));
    const auto first_message = [&compiled](const json& value) {
        const auto result = compiled.validate(value);
        return result.violations.empty() ? std::string() : result.violations[0].message;
    };
    EXPECT_EQ(first_message(json::array({1, 2, 3})), "An array of length 3 is not a valid enum value.");
    EXPECT_EQ(first_message({{"a", 1}}), "An object of size 1 is not a valid enum value.");
    EXPECT_EQ(first_message(7), "'7' is not a valid enum value.");
    EXPECT_EQ(first_message("seven"), "'seven' is not a valid enum value.");
}

// Each branch counts a long string's characters again, which no evaluation step pays for.
TEST(SchemaLimits, WholeStringKeywordsAreCharged) {
    json branches = json::array();
    for (int i = 0; i < 200; ++i) {
        branches.push_back({{"minLength", 2'000'000 + i}});
    }
    const json schema = through_jsoncons({{"anyOf", branches}});
    // 200 branches over 32 KiB read about 6.6 million bytes, about 410,000 units at 16 bytes each.
    // 250,000 units pay for checking the schema when it is compiled, but not for that.
    const std::string text(std::size_t{32} * 1024, 'x');
    EXPECT_FALSE(stops_for_work(schema, text, 10'000'000));
    EXPECT_FALSE(CompiledSchema::compile(schema).validate(text).violations.empty());
    mcp::SchemaOptions options;
    options.max_evaluation_work = 250'000;
    const auto result = CompiledSchema::compile(schema, options).validate(text);
    ASSERT_EQ(result.violations.size(), 1U);
    EXPECT_EQ(result.violations[0].message,
              "validation stopped: schema evaluation takes too much work");
}

// Draft-07 evaluates contentEncoding and contentMediaType, and each reads the whole string: 100
// branches over 1 MiB cost about 26 million units, which 10 million do not pay for.
TEST(SchemaLimits, ContentKeywordsAreChargedByTheStringsLength) {
    const json text = std::string(1U << 20U, 'x');
    for (const auto& [keyword, value] :
         {std::pair{"contentEncoding", "base64"}, std::pair{"contentMediaType", "text/plain"}}) {
        json branches = json::array();
        for (int i = 0; i < 100; ++i) {
            branches.push_back({{keyword, value}});
        }
        const json schema = {{"$schema", "http://json-schema.org/draft-07/schema#"},
                             {"allOf", branches}};
        EXPECT_TRUE(CompiledSchema::compile(schema).validate(text).valid) << keyword;
        EXPECT_TRUE(stops_for_work(schema, text, 10'000'000)) << keyword;
    }
}

// 40,000 enum comparisons. Both validators charge 8 units for one, so 100,000 units do not pay for
// them, whichever validator runs.
TEST(SchemaLimits, BothValidatorsChargeAComparisonAlike) {
    json allowed = json::array();
    for (int i = 0; i < 200; ++i) {
        allowed.push_back(1000 + i);
    }
    const json schema = {{"items", {{"enum", allowed}}}};
    const json value(200, allowed.back());
    EXPECT_FALSE(stops_for_work(schema, value, 10'000'000));
    EXPECT_TRUE(stops_for_work(schema, value, 100'000));
    EXPECT_TRUE(stops_for_work(through_jsoncons(schema), value, 100'000));
}

// Each item is compared with a const of 200 values: 40,000 comparisons in all.
TEST(SchemaLimits, ConstComparisonsAreCharged) {
    const json constant(200, 7);
    const json schema = {{"items", {{"const", constant}}}};
    const json value(200, constant);
    EXPECT_FALSE(stops_for_work(schema, value, 10'000'000));
    EXPECT_TRUE(stops_for_work(schema, value, 100'000));
}

// 2,500 enum comparisons of arrays of 101 values. Charged by what they compare, they cost more than
// 100,000 units on either validator.
TEST(SchemaLimits, EnumComparisonsAreChargedByWhatTheyHold) {
    json allowed = json::array();
    for (int i = 0; i < 50; ++i) {
        json member(100, 0);
        member.push_back(i);
        allowed.push_back(std::move(member));
    }
    const json schema = {{"items", {{"enum", allowed}}}};
    const json value(50, allowed.back());
    EXPECT_FALSE(stops_for_work(schema, value, 10'000'000));
    EXPECT_TRUE(stops_for_work(schema, value, 100'000));
    EXPECT_FALSE(stops_for_work(through_jsoncons(schema), value, 10'000'000));
    EXPECT_TRUE(stops_for_work(through_jsoncons(schema), value, 100'000));
}

// 400 const comparisons of arrays of 500 values, on the validator that "not" sends a schema to.
TEST(SchemaLimits, ConstComparisonsAreChargedOnEitherValidator) {
    const json constant(500, 7);
    const json schema = through_jsoncons({{"items", {{"const", constant}}}});
    const json value(400, constant);
    EXPECT_FALSE(stops_for_work(schema, value, 10'000'000));
    EXPECT_TRUE(stops_for_work(schema, value, 100'000));
}

// Each of 500 objects is checked against 500 dependentRequired entries, which enters no subschema.
TEST(SchemaLimits, DependentRequiredEntriesAreCharged) {
    json dependencies = json::object();
    for (int i = 0; i < 500; ++i) {
        dependencies["d" + std::to_string(i)] = json::array();
    }
    const json schema = through_jsoncons({{"items", {{"dependentRequired", dependencies}}}});
    const json value(500, json::object());
    EXPECT_FALSE(stops_for_work(schema, value, 10'000'000));
    EXPECT_TRUE(stops_for_work(schema, value, 100'000));
}

// Each of 200,000 properties is looked up in "properties", and none enters a subschema.
TEST(SchemaLimits, PropertiesLookedUpAreCharged) {
    json value = json::object();
    for (int i = 0; i < 200000; ++i) {
        value["p" + std::to_string(i)] = 0;
    }
    const json schema = through_jsoncons({{"properties", {{"q", json::object()}}}});
    EXPECT_FALSE(stops_for_work(schema, value, 10'000'000));
    EXPECT_TRUE(stops_for_work(schema, value, 100'000));
}

// The work allowed grows with the schema as well as the value, so a large enum or a union of many
// object types over an ordinary array is not stopped.
TEST(SchemaLimits, LargeSchemasOverOrdinaryArraysAreNotStopped) {
    json allowed = json::array();
    for (int i = 0; i < 200; ++i) {
        allowed.push_back("v" + std::to_string(1000 + i));
    }
    const json codes = {{"type", "array"}, {"items", {{"enum", allowed}}}};
    for (const auto& each : {codes, through_jsoncons(codes)}) {
        EXPECT_TRUE(is_valid(each, json(1000, allowed.back())));
    }

    json defs = json::object();
    json branches = json::array();
    for (int i = 0; i < 20; ++i) {
        const auto name = "T" + std::to_string(i);
        defs[name] = {{"type", "object"},
                      {"properties",
                       {{"kind", {{"const", "t" + std::to_string(i)}}},
                        {"name", {{"type", "string"}}},
                        {"n", {{"type", "integer"}}}}},
                      {"required", {"kind", "name"}},
                      {"additionalProperties", false}};
        branches.push_back({{"$ref", "#/$defs/" + name}});
    }
    const json union_schema = {{"$defs", defs}, {"type", "array"}, {"items", {{"oneOf", branches}}}};
    // Each item costs the same, so 500 items within a tenth of the default budget stand for 5000
    // within all of it.
    const json items(500, {{"kind", "t19"}, {"name", "x"}, {"n", 1}});
    EXPECT_TRUE(is_valid(union_schema, items));
    EXPECT_FALSE(stops_for_work(union_schema, items, mcp::SchemaOptions{}.max_evaluation_work / 10));
}

// Property names are compared whole: a NUL inside one does not end it, in values and in schemas.
TEST(SchemaValidation, PropertyNamesWithNulAreComparedWhole) {
    const std::string name("a\0b", 3);
    const json schema = {{"type", "object"},
                         {"properties", {{"a", {{"type", "integer"}}}, {name, {{"type", "string"}}}}}};
    for (const auto& each : {schema, through_jsoncons(schema)}) {
        EXPECT_TRUE(is_valid(each, {{name, "text"}}));
        EXPECT_FALSE(is_valid(each, {{name, 1}}));
        EXPECT_FALSE(is_valid(each, {{"a", "text"}}));
    }
    const json closed =
        through_jsoncons({{"properties", {{"a", true}}}, {"additionalProperties", false}});
    EXPECT_FALSE(is_valid(closed, {{name, 1}}));
    // An unknown keyword, not "type".
    EXPECT_TRUE(is_valid(through_jsoncons({{std::string("type\0x", 6), "integer"}}), "text"));
}

// A pattern that is not a regular expression makes the schema invalid. A pattern that cannot be run
// here is reported only when nothing else is wrong with the schema, since a client sends a call
// whose output schema has only that problem.
TEST(SchemaDialect, PatternProblemsComeAfterOtherProblems) {
    EXPECT_EQ(compile_error({{"pattern", "("}}), SchemaError::Category::eInvalidSchema);
    EXPECT_EQ(compile_error({{"pattern", "[z-a]"}}), SchemaError::Category::eInvalidSchema);
    EXPECT_EQ(compile_error({{"patternProperties", {{"*", true}}}}),
              SchemaError::Category::eInvalidSchema);
    EXPECT_EQ(
        compile_error({{"properties", {{"a", {{"pattern", "(?=x)"}}}, {"b", {{"pattern", "("}}}}}}),
        SchemaError::Category::eInvalidSchema);
    for (
        const auto* text : {
            R"json({"properties": {"a": {"pattern": "(?=x)"}, "b": {"$ref": "https://example.com/a.json"}}})json",
            R"json({"properties": {"b": {"$ref": "https://example.com/a.json"}, "a": {"pattern": "(?=x)"}}})json",
            R"json({"properties": {"a": {"pattern": "(?=x)"}, "b": {"$ref": "#/$defs/missing"}}})json",
        }) {
        SCOPED_TRACE(text);
        EXPECT_EQ(compile_error(json::parse(text)), SchemaError::Category::eUnresolvedRef);
    }
    // Every evaluation of this schema nests too deeply.
    EXPECT_EQ(compile_error({{"$ref", "#"}, {"properties", {{"a", {{"pattern", "(?=x)"}}}}}}),
              SchemaError::Category::eInvalidSchema);
}

// Each pattern is bounded on its own, and the patterns of one schema are bounded together.
TEST(SchemaLimits, PatternProgramsAreBoundedPerSchema) {
    json properties = json::object();
    for (int i = 0; i < 100; ++i) {
        properties["p" + std::to_string(i)] = {{"pattern", "(?:a{1000}){19}"}};
    }
    const json schema = {{"type", "object"}, {"properties", properties}};
    EXPECT_EQ(compile_error(schema), SchemaError::Category::eUnsupportedPattern);
    mcp::SchemaOptions options;
    options.max_pattern_instructions = 3'000'000;
    EXPECT_NO_THROW((void)CompiledSchema::compile(schema, options));
}

// A 2020-12 schema that constrains only the value's type is evaluated natively, with the same
// message as jsoncons gives. Adding "not": false, which constrains nothing, sends the same schema
// through jsoncons; both must agree on every value, messages included.
TEST(SchemaValidation, TypeOnlySchemasMatchFullEvaluation) {
    const std::vector<json> values = {
        nullptr,
        true,
        0,
        -7,
        std::numeric_limits<std::uint64_t>::max(),
        1.5,
        1e308,
        "",
        "text",
        json::array(),
        json::array({1, "a"}),
        json::object(),
        json{{"a", {{"b", 1}}}},
    };
    std::vector<json> schemas = {json::object(), json{{"title", "anything"}, {"x-note", 1}}};
    for (const auto* type : {"object", "array", "string", "boolean", "null", "number"}) {
        schemas.push_back({{"type", type}});
        schemas.push_back({{"type", type},
                           {"description", "d"},
                           {"$comment", "c"},
                           {"default", nullptr},
                           {"$schema", "http://json-schema.org/draft-07/schema#"}});
    }
    for (const auto& schema : schemas) {
        auto full_schema = schema;
        full_schema["not"] = false;
        const auto fast = CompiledSchema::compile(schema);
        const auto full = CompiledSchema::compile(full_schema);
        for (const auto& value : values) {
            SCOPED_TRACE(schema.dump() + " against " + value.dump());
            const auto fast_result = fast.validate(value);
            const auto full_result = full.validate(value);
            EXPECT_EQ(fast_result.valid, full_result.valid);
            ASSERT_EQ(fast_result.violations.size(), full_result.violations.size());
            for (std::size_t i = 0; i < fast_result.violations.size(); ++i) {
                EXPECT_EQ(fast_result.violations[i].instance_path,
                          full_result.violations[i].instance_path);
                EXPECT_EQ(fast_result.violations[i].message, full_result.violations[i].message);
            }
        }
    }
    // The depth limit applies on both paths.
    mcp::SchemaOptions options;
    options.max_instance_depth = 4;
    json deep = json::array();
    for (int i = 0; i < 10; ++i) {
        deep = json::array({std::move(deep)});
    }
    EXPECT_FALSE(CompiledSchema::compile({{"type", "array"}}, options).validate(deep).valid);
    EXPECT_FALSE(CompiledSchema::compile(true, options).validate(deep).valid);
}

// Messages only the native evaluation produces show that a schema using just the keywords it
// implements is evaluated that way.
TEST(SchemaNative, SupportedSchemaIsEvaluatedNatively) {
    const auto schema = CompiledSchema::compile(json::parse(R"({
        "type": "object",
        "properties": {"n": {"type": "integer", "minimum": 1}},
        "required": ["n", "m"],
        "additionalProperties": false
    })"));
    const auto result = schema.validate({{"n", 0}, {"x~/y", true}});
    ASSERT_FALSE(result.valid);
    std::vector<std::pair<std::string, std::string>> violations;
    for (const auto& violation : result.violations) {
        violations.emplace_back(violation.instance_path, violation.message);
    }
    const std::vector<std::pair<std::string, std::string>> expected = {
        {"", "Required property 'm' is missing"},
        {"/n", "Value must be at least 1"},
        {"/x~0~1y", "Additional property 'x~/y' is not allowed"},
    };
    EXPECT_EQ(violations, expected);
}

// A member that fails the additionalProperties subschema is reported at the member, as `properties`
// reports its members, whichever evaluation runs: adding `$defs` sends the schema through jsoncons.
// Only the instance paths are compared; the two evaluations word some messages differently.
TEST(SchemaValidation, AdditionalPropertyViolationsNameTheMember) {
    for (const bool through_jsoncons : {false, true}) {
        SCOPED_TRACE(through_jsoncons ? "jsoncons" : "native");
        json schema = {{"type", "object"},
                       {"properties", {{"known", {{"type", "string"}}}}},
                       {"additionalProperties", {{"type", "integer"}, {"minimum", 3}}}};
        if (through_jsoncons) {
            schema["$defs"] = json::object();
        }
        const auto result =
            CompiledSchema::compile(schema).validate({{"known", 1}, {"a", "x"}, {"b", 1}, {"ok", 5}});
        ASSERT_FALSE(result.valid);
        std::set<std::string> paths;
        for (const auto& violation : result.violations) {
            paths.insert(violation.instance_path);
        }
        EXPECT_EQ(paths, (std::set<std::string>{"/a", "/b", "/known"}));
    }
}

// JSON Schema compares numbers by value. Converting an integer beyond 2^53 to a double rounds it,
// so both evaluations compare an integer with a double exactly.
TEST(SchemaNative, IntegersAndDoublesCompareExactly) {
    const auto expect = [](bool valid, const char* schema_text, const json& instance) {
        SCOPED_TRACE(std::string(schema_text) + " against " + instance.dump());
        const auto schema = json::parse(schema_text);
        EXPECT_EQ(is_valid(schema, instance), valid);
        EXPECT_EQ(is_valid(through_jsoncons(schema), instance), valid);
    };
    // 18446744073709551616 (2^64) does not fit std::uint64_t and is read as a double.
    const json above_uint64_max = json::parse("18446744073709551616");
    ASSERT_TRUE(above_uint64_max.is_number_float());
    expect(false, R"({"maximum": 18446744073709551615})", above_uint64_max);
    expect(false, R"({"exclusiveMaximum": 18446744073709551615})", above_uint64_max);
    expect(true, R"({"minimum": 18446744073709551615})", above_uint64_max);
    expect(false, R"({"const": 18446744073709551615})", above_uint64_max);
    expect(false, R"({"enum": [9223372036854775807]})", json::parse("9223372036854775808.0"));
    expect(false, R"({"type": "integer", "maximum": 9223372036854775807})",
           json::parse("9223372036854775808.0"));
    expect(true, R"({"maximum": 9007199254740993})", json::parse("9007199254740992.0"));
    expect(false, R"({"maximum": 9007199254740992.0})", json::parse("9007199254740993"));
    expect(false, R"({"exclusiveMinimum": 9007199254740993})", json::parse("9007199254740992.0"));
    expect(false, R"({"minimum": -9007199254740992.0})", json::parse("-9007199254740993"));
    expect(false, R"({"const": 9007199254740993})", json::parse("9007199254740992.0"));
    expect(false, R"({"const": [9007199254740993]})", json::parse("[9007199254740992.0]"));
    expect(true, R"({"uniqueItems": true})", json::parse("[9007199254740993, 9007199254740992.0]"));
    expect(false, R"({"maximum": -2.5})", json(-2));
    expect(true, R"({"minimum": -2.5})", json(-2));
    // Equal values stay equal whatever their representation.
    expect(true, R"({"const": 9007199254740992})", json::parse("9007199254740992.0"));
    expect(true, R"({"const": -9223372036854775808})", json::parse("-9223372036854775808.0"));
    expect(true, R"({"const": 0})", json(-0.0));
    expect(false, R"({"uniqueItems": true})", json::parse("[9007199254740992, 9007199254740992.0]"));
}

// Generated schemas over the keywords the native evaluation implements, against generated values:
// both evaluations must agree on every one.
TEST(SchemaNative, AgreesWithJsonconsOnGeneratedSchemas) {
    std::mt19937 random(20261019);
    const auto pick = [&](std::size_t count) {
        return std::uniform_int_distribution<std::size_t>(0, count - 1)(random);
    };
    const auto chance = [&](int percent) { return static_cast<int>(pick(100)) < percent; };
    const std::vector<json> scalars = {
        nullptr,
        true,
        false,
        0,
        1,
        -1,
        2,
        3,
        1.0,
        2.5,
        -0.5,
        1e300,
        std::numeric_limits<std::uint64_t>::max(),
        "",
        "a",
        "ab1",
        "zzz",
        "\xc3\xa9\xc3\xa9",
    };
    const std::vector<std::string> names = {"a", "b", "c", "d"};
    const std::vector<std::string> types = {"null",   "boolean", "integer", "number",
                                            "string", "array",   "object"};
    const std::vector<std::string> patterns = {"^a", "1$", "[0-9]", "^[a-z]*$", "^.{2}$"};

    std::function<json(int)> make_value = [&](int depth) -> json {
        const auto kind = pick(depth > 0 ? 4 : 2);
        if (kind < 2) {
            return scalars[pick(scalars.size())];
        }
        if (kind == 2) {
            json array = json::array();
            for (std::size_t i = pick(4); i > 0; --i) {
                array.push_back(make_value(depth - 1));
            }
            return array;
        }
        json object = json::object();
        for (std::size_t i = pick(4); i > 0; --i) {
            object[names[pick(names.size())]] = make_value(depth - 1);
        }
        return object;
    };
    const auto number = [&]() -> json {
        const std::vector<json> numbers = {0, 1, 2, -1, 1.5, 2.0, -0.5};
        return numbers[pick(numbers.size())];
    };
    std::function<json(int)> make_schema = [&](int depth) -> json {
        if (chance(10)) {
            return chance(70);
        }
        json schema = json::object();
        if (chance(50)) {
            if (chance(70)) {
                schema["type"] = types[pick(types.size())];
            } else {
                json list = json::array();
                for (const auto& type : types) {
                    if (chance(35)) {
                        list.push_back(type);
                    }
                }
                if (list.empty()) {
                    list.push_back("string");
                }
                schema["type"] = list;
            }
        }
        if (chance(15)) {
            json values = json::array();
            for (std::size_t i = pick(3) + 1; i > 0; --i) {
                values.push_back(make_value(1));
            }
            schema["enum"] = values;
        }
        if (chance(8)) {
            schema["const"] = make_value(1);
        }
        for (const auto* keyword : {"minimum", "maximum", "exclusiveMinimum", "exclusiveMaximum"}) {
            if (chance(12)) {
                schema[keyword] = number();
            }
        }
        for (const auto* keyword :
             {"minLength", "maxLength", "minItems", "maxItems", "minProperties", "maxProperties"}) {
            if (chance(12)) {
                schema[keyword] = pick(4);
            }
        }
        if (chance(15)) {
            schema["pattern"] = patterns[pick(patterns.size())];
        }
        if (depth > 0 && chance(30)) {
            schema["items"] = make_schema(depth - 1);
        }
        if (depth > 0 && chance(40)) {
            json properties = json::object();
            for (const auto& name : names) {
                if (chance(40)) {
                    properties[name] = make_schema(depth - 1);
                }
            }
            schema["properties"] = properties;
        }
        if (chance(25)) {
            json required = json::array();
            for (const auto& name : names) {
                if (chance(30)) {
                    required.push_back(name);
                }
            }
            schema["required"] = required;
        }
        if (depth > 0 && chance(25)) {
            schema["additionalProperties"] = make_schema(depth - 1);
        }
        if (chance(10)) {
            schema["description"] = "annotation";
        }
        return schema;
    };

    int compared = 0;
    for (int i = 0; i < 600; ++i) {
        const auto schema = make_schema(2);
        if (!schema.is_object()) {
            continue;
        }
        const auto native = CompiledSchema::compile(schema);
        const auto full = CompiledSchema::compile(through_jsoncons(schema));
        for (int j = 0; j < 12; ++j) {
            const auto value = make_value(2);
            SCOPED_TRACE(schema.dump() + " against " + value.dump());
            EXPECT_EQ(native.validate(value).valid, full.validate(value).valid);
            ++compared;
        }
    }
    EXPECT_GT(compared, 5000);
}

TEST(SchemaBridge, NumbersStringsAndNullsKeepTheirMeaning) {
    EXPECT_TRUE(is_valid({{"const", 1}}, 1.0));
    EXPECT_TRUE(is_valid({{"type", "integer"}}, 1.0));
    EXPECT_FALSE(is_valid({{"type", "integer"}}, 1.5));
    EXPECT_TRUE(is_valid({{"type", "integer"}}, std::numeric_limits<std::uint64_t>::max()));
    EXPECT_TRUE(
        is_valid({{"type", "integer"}, {"minimum", 0}}, std::numeric_limits<std::uint64_t>::max()));
    EXPECT_FALSE(
        is_valid({{"type", "integer"}, {"maximum", 0}}, std::numeric_limits<std::uint64_t>::max()));
    EXPECT_TRUE(
        is_valid({{"type", "integer"}, {"maximum", 0}}, std::numeric_limits<std::int64_t>::min()));
    EXPECT_TRUE(is_valid({{"type", "null"}}, nullptr));
    EXPECT_FALSE(is_valid({{"type", "null"}}, json::object()));
    EXPECT_TRUE(
        is_valid({{"const", "h\xc3\xa9llo \xf0\x9f\x98\x80"}}, "h\xc3\xa9llo \xf0\x9f\x98\x80"));
    EXPECT_TRUE(is_valid({{"maxLength", 1}}, "\xf0\x9f\x98\x80"));  // one code point
    EXPECT_TRUE(
        is_valid({{"const", {{"a", {1, 2}}, {"b", nullptr}}}}, {{"b", nullptr}, {"a", {1, 2}}}));
}

// Doubles beyond the range of the integer types: JSON Schema counts any number with a zero
// fractional part as an integer, and none of these may convert with undefined behavior (the
// sanitizer jobs check that).
TEST(SchemaBridge, LargeDoublesAreHandled) {
    EXPECT_TRUE(is_valid({{"type", "integer"}}, 1e308));
    EXPECT_TRUE(is_valid({{"type", "integer"}}, -1e19));
    EXPECT_FALSE(is_valid({{"type", "integer"}}, 2.5));
    // The quotient overflows, so 1e308 is not a multiple (JSON-Schema-Test-Suite "float
    // division = inf").
    EXPECT_FALSE(is_valid({{"multipleOf", 0.123456789}}, 1e308));
    EXPECT_TRUE(is_valid({{"multipleOf", 0.5}}, 1e300));
    // Size keywords beyond std::size_t saturate.
    EXPECT_TRUE(is_valid({{"maxLength", 1e308}}, "abc"));
    EXPECT_FALSE(is_valid({{"minLength", 1e308}}, "abc"));
    EXPECT_TRUE(is_valid({{"maxItems", 1e300}}, {1, 2, 3}));
    EXPECT_FALSE(is_valid({{"maxItems", 2.0}}, {1, 2, 3}));
}

// JSON has no NaN or Infinity, and nlohmann::json writes them as null, so a value holding one is
// checked as the null a peer would receive. A schema holding one is refused: what a peer would
// receive is not the schema that was compiled.
TEST(SchemaBridge, NonFiniteNumbersAreCheckedAsNull) {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double infinity = std::numeric_limits<double>::infinity();
    for (const bool through_jsoncons : {false, true}) {
        SCOPED_TRACE(through_jsoncons ? "jsoncons" : "native");
        json number = {{"type", "number"}};
        json null = {{"type", "null"}};
        if (through_jsoncons) {
            number["$defs"] = json::object();
            null["$defs"] = json::object();
        }
        for (const double value : {nan, infinity, -infinity}) {
            EXPECT_FALSE(is_valid(number, value));
            EXPECT_TRUE(is_valid(null, value));
            EXPECT_TRUE(is_valid({{"items", null}}, json::array({value, value})));
        }
    }
    EXPECT_EQ(compile_error({{"maximum", nan}}), SchemaError::Category::eInvalidSchema);
    EXPECT_EQ(compile_error({{"properties", {{"a", {{"const", -infinity}}}}}}),
              SchemaError::Category::eInvalidSchema);
}

TEST(CompiledSchema, CopiesShareTheSchema) {
    const auto original = CompiledSchema::compile({{"type", "string"}});
    const CompiledSchema copy = original;  // NOLINT(performance-unnecessary-copy-initialization)
    EXPECT_EQ(copy.schema(), json({{"type", "string"}}));
    EXPECT_TRUE(copy.validate("x").valid);
}

// One compiled schema validated from several threads at once must give the same answers as
// single-threaded validation. The tsan job runs this under ThreadSanitizer.
TEST(CompiledSchema, ConcurrentValidateMatchesSequential) {
    const auto schema = CompiledSchema::compile(json::parse(R"({
        "type": "object",
        "properties": {"n": {"type": "integer", "minimum": 0},
                       "s": {"type": "string", "pattern": "^[a-z]+$"},
                       "e": {"enum": ["x", "y"]}},
        "required": ["n"],
        "additionalProperties": false
    })"));
    const json good = {{"n", 3}, {"s", "abc"}, {"e", "x"}};
    const json bad = {{"n", -1}, {"s", "ABC"}, {"z", 1}};
    ASSERT_TRUE(schema.validate(good).valid);
    ASSERT_FALSE(schema.validate(bad).valid);
    std::vector<int> wrong(8, 0);
    std::vector<std::thread> threads;
    for (std::size_t t = 0; t < wrong.size(); ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < 2000; ++i) {
                if (!schema.validate(good).valid || schema.validate(bad).valid) {
                    ++wrong[t];
                }
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    for (int count : wrong) {
        EXPECT_EQ(count, 0);
    }
}

// --- JSON-Schema-Test-Suite (test/data/json-schema, see scripts/vendor_json_schema_test_suite.py).

/// A suite group left out of the run, with the reason. `group` "*" leaves out the whole file.
struct SuiteExclusion {
    const char* file;
    const char* group;
    const char* reason;
};

struct SuiteOutcome {
    std::size_t passed = 0;
    std::size_t total = 0;
    std::size_t excluded = 0;
    std::vector<std::string> failures;
    std::set<std::string> unused_exclusions;
};

/// Runs the suite's files for one dialect. The suite's schemas do not name their dialect; when
/// @p dialect_uri is set it is added as `$schema` to every object schema, because a schema without
/// `$schema` is read as 2020-12.
SuiteOutcome run_suite(const std::string& dialect_dir, const std::vector<SuiteExclusion>& exclusions,
                       const char* dialect_uri = nullptr) {
    SuiteOutcome outcome;
    std::set<std::string> whole_file_exclusions;
    for (const auto& exclusion : exclusions) {
        const auto key = std::string(exclusion.file) + " | " + exclusion.group;
        outcome.unused_exclusions.insert(key);
        if (std::string_view(exclusion.group) == "*") {
            whole_file_exclusions.insert(key);
        }
    }
    const std::filesystem::path dir =
        std::filesystem::path(MCP_TEST_DATA_DIR) / "json-schema" / dialect_dir;
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (entry.path().extension() == ".json") {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    for (const auto& file : files) {
        std::ifstream stream(file);
        const json groups = json::parse(stream);
        for (const auto& group : groups) {
            const auto key =
                file.filename().string() + " | " + group.at("description").get<std::string>();
            const auto whole_file = file.filename().string() + " | *";
            bool excluded = outcome.unused_exclusions.erase(key) == 1;
            if (whole_file_exclusions.count(whole_file) == 1) {
                outcome.unused_exclusions.erase(whole_file);
                excluded = true;
            }
            const auto& tests = group.at("tests");
            if (excluded) {
                outcome.excluded += tests.size();
                continue;
            }
            json schema_json = group.at("schema");
            if (dialect_uri != nullptr && schema_json.is_object() && !schema_json.contains("$schema")) {
                schema_json["$schema"] = dialect_uri;
            }
            std::optional<CompiledSchema> schema;
            try {
                schema = CompiledSchema::compile(schema_json);
            } catch (const std::exception& error) {
                outcome.total += tests.size();
                outcome.failures.push_back(key + " | compile: " + error.what());
                continue;
            }
            for (const auto& test : tests) {
                ++outcome.total;
                if (schema->validate(test.at("data")).valid == test.at("valid").get<bool>()) {
                    ++outcome.passed;
                } else {
                    outcome.failures.push_back(key + " | " + test.at("description").get<std::string>());
                }
            }
        }
    }
    return outcome;
}

void expect_suite(const SuiteOutcome& outcome, std::size_t expected_total,
                  std::size_t expected_excluded) {
    for (const auto& failure : outcome.failures) {
        ADD_FAILURE() << failure;
    }
    for (const auto& unused : outcome.unused_exclusions) {
        ADD_FAILURE() << "exclusion matches no test group: " << unused;
    }
    // The counts pin the data: a case cannot disappear from the run without changing them.
    EXPECT_EQ(outcome.total, expected_total);
    EXPECT_EQ(outcome.excluded, expected_excluded);
    EXPECT_EQ(outcome.passed, outcome.total);
}

constexpr const char* g_remote_reason =
    "references a schema served from http://localhost:1234; remote references are refused by design";
constexpr const char* g_remote_metaschema_reason =
    "declares a custom metaschema served from http://localhost:1234; only the bundled dialects are "
    "supported";

const std::vector<SuiteExclusion> g_2020_12_exclusions = {
    {"refRemote.json", "*", g_remote_reason},
    {"dynamicRef.json", "strict-tree schema, guards against misspelled properties", g_remote_reason},
    {"dynamicRef.json", "tests for implementation dynamic anchor and reference link", g_remote_reason},
    {"dynamicRef.json", "$ref and $dynamicAnchor are independent of order - $defs first",
     g_remote_reason},
    {"dynamicRef.json", "$ref and $dynamicAnchor are independent of order - $ref first",
     g_remote_reason},
    {"dynamicRef.json", "$ref to $dynamicRef finds detached $dynamicAnchor", g_remote_reason},
    {"vocabulary.json", "schema that uses custom metaschema with with no validation vocabulary",
     g_remote_metaschema_reason},
    {"vocabulary.json", "ignore unrecognized optional vocabulary", g_remote_metaschema_reason},
    {"pattern.json", "pattern with Unicode property escape requires unicode mode",
     "mcp::detail::Regex rejects Unicode property escapes"},
    {"patternProperties.json", "patternProperties with Unicode property escape",
     "mcp::detail::Regex rejects Unicode property escapes"},
    {"dynamicRef.json", "$dynamicRef avoids the root of each schema, but scopes are still registered",
     "jsoncons resolves this $dynamicRef scope case differently"},
};

const std::vector<SuiteExclusion> g_draft7_exclusions = {
    {"refRemote.json", "*", g_remote_reason},
};

TEST(Suite, Required2020_12) {
    const auto outcome = run_suite("draft2020-12", g_2020_12_exclusions);
    expect_suite(outcome, 1245, 57);
}

TEST(Suite, RequiredDraft7) {
    const auto outcome =
        run_suite("draft7", g_draft7_exclusions, "http://json-schema.org/draft-07/schema#");
    expect_suite(outcome, 906, 23);
}

}  // namespace

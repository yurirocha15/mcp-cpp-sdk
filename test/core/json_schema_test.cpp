#include <mcp/protocol/schema.hpp>

#include <gtest/gtest.h>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using mcp::CompiledSchema;
using mcp::SchemaError;
using nlohmann::json;

bool is_valid(const json& schema, const json& instance) {
    return CompiledSchema::compile(schema).validate(instance).valid;
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

TEST(SchemaDialect, UnsupportedPatternSyntaxIsInvalidSchema) {
    EXPECT_EQ(compile_error({{"pattern", "(a)\\1"}}), SchemaError::Category::eInvalidSchema);
    EXPECT_EQ(compile_error({{"patternProperties", {{"\\p{L}", true}}}}),
              SchemaError::Category::eInvalidSchema);
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
    const auto start = std::chrono::steady_clock::now();
    const auto result = schema.validate(value);
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(10));
    EXPECT_FALSE(result.valid);
    ASSERT_EQ(result.violations.size(), 1U);
    EXPECT_NE(result.violations.front().message.find("too many steps"), std::string::npos)
        << result.violations.front().message;
}

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

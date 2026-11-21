// The checks a schema gets against its dialect's rules, compared with evaluating the dialect's
// metaschema: test/data/json-schema-meta holds the published metaschemas, which the SDK does not
// ship, and the native evaluation runs them. For every suite schema, every generated schema and
// every mutant, a schema must pass the checks exactly when its metaschema accepts it.

#include <mcp/protocol/schema.hpp>

#include "../support/schema_corpus.hpp"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace json_schema_checker_test {

namespace {

namespace schema_corpus = mcp::test::schema_corpus;
using mcp::CompiledSchema;
using mcp::SchemaDocuments;
using mcp::SchemaError;
using nlohmann::json;

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define MCP_SCHEMA_CHECKER_SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) ||                             \
    __has_feature(memory_sanitizer)
#define MCP_SCHEMA_CHECKER_SANITIZED 1
#endif
#endif

/// Every how many generated schemas and mutants this build compares: every fiftieth in sanitized
/// and unoptimized builds, which take longer than a test may run for all of them.
constexpr std::size_t stride() {
#if defined(MCP_SCHEMA_CHECKER_SANITIZED) || !defined(NDEBUG)
    return 50;
#else
    return 1;
#endif
}

json load(const std::filesystem::path& path) {
    std::ifstream stream(path);
    return json::parse(stream);
}

/// The metaschema of @p dialect compiled from the published files, its vocabularies registered.
CompiledSchema metaschema(const schema_corpus::Dialect& dialect) {
    const auto directory =
        std::filesystem::path(MCP_TEST_DATA_DIR) / "json-schema-meta" / dialect.directory;
    SchemaDocuments vocabularies;
    if (std::filesystem::exists(directory / "meta")) {
        for (const auto& entry : std::filesystem::directory_iterator(directory / "meta")) {
            auto document = load(entry.path());
            vocabularies.emplace(document.at("$id").get<std::string>(), std::move(document));
        }
    }
    return CompiledSchema::compile(load(directory / "schema.json"), {}, vocabularies);
}

/// Whether @p schema passes the checks against its dialect's rules: compiling it is not refused
/// for breaking them.
bool passes_checks(const json& schema) {
    try {
        (void)CompiledSchema::compile(schema);
        return true;
    } catch (const SchemaError& error) {
        return std::string_view(error.what()).rfind("JSON Schema is invalid at '", 0) != 0;
    }
}

/// Compares the checks with the metaschema on @p schema when it declares @p dialect; counts it.
void compare(const CompiledSchema& meta, const schema_corpus::Dialect& dialect, const json& schema,
             std::size_t& compared) {
    if (!schema.is_object() || schema.value("$schema", json()) != dialect.uri) {
        return;
    }
    ++compared;
    const auto verdict = meta.validate(schema);
    EXPECT_EQ(passes_checks(schema), verdict.valid)
        << dialect.directory << ": " << schema.dump()
        << (verdict.violations.empty() ? std::string()
                                       : "\nmetaschema: " + verdict.violations[0].instance_path + ": " +
                                             verdict.violations[0].message);
}

TEST(SchemaChecker, AgreesWithTheMetaschemas) {
    constexpr std::size_t g_generated = 200;
    for (const auto& dialect : schema_corpus::dialects()) {
        SCOPED_TRACE(dialect.directory);
        const auto meta = metaschema(dialect);
        std::size_t compared = 0;
        std::size_t rejected = 0;
        std::size_t index = 0;
        for (const auto& file : schema_corpus::suite_files(dialect)) {
            for (const auto& group : file.groups) {
                compare(meta, dialect, group.schema, compared);
                schema_corpus::for_each_mutant(group.schema, [&](const json& mutant) {
                    if (index++ % stride() == 0) {
                        compare(meta, dialect, mutant, compared);
                        rejected += meta.validate(mutant).valid ? 0 : 1;
                    }
                });
            }
        }
        for (const auto& item : schema_corpus::generated_cases(dialect, g_generated)) {
            compare(meta, dialect, item.schema, compared);
        }
        // Both outcomes occur, so the comparison is not vacuous.
        EXPECT_GT(compared, 0U);
        EXPECT_GT(rejected, 0U);
    }
}

// Each problem is reported where it is, with what was found.
TEST(SchemaChecker, ReportsWhereAndWhat) {
    const auto message = [](const json& schema) {
        try {
            (void)CompiledSchema::compile(schema);
        } catch (const SchemaError& error) {
            return std::string(error.what());
        }
        return std::string();
    };
    EXPECT_EQ(message({{"properties", {{"a~b", {{"type", "integr"}}}}}}),
              "JSON Schema is invalid at '/properties/a~0b/type': 'integr' is not a JSON Schema type");
    EXPECT_EQ(message({{"minLength", -1}}),
              "JSON Schema is invalid at '/minLength': must not be negative");
    EXPECT_EQ(message({{"allOf", json::array()}}),
              "JSON Schema is invalid at '/allOf': must not be empty");
    EXPECT_EQ(message({{"required", {"a", "a"}}}),
              "JSON Schema is invalid at '/required': items must be unique");
    EXPECT_EQ(message({{"$schema", "http://json-schema.org/draft-04/schema#"}, {"not", true}}),
              "JSON Schema is invalid at '/not': expected object, found boolean");
    EXPECT_EQ(
        message({{"$schema", "http://json-schema.org/draft-04/schema#"}, {"exclusiveMinimum", true}}),
        "JSON Schema is invalid at '': exclusiveMinimum requires minimum");
    EXPECT_EQ(message({{"$id", "https://example.com/a#b"}}),
              "JSON Schema is invalid at '/$id': must not have a non-empty fragment");
    EXPECT_EQ(message({{"$anchor", "1a"}}),
              "JSON Schema is invalid at '/$anchor': '1a' is not an anchor name");
    EXPECT_EQ(message({{"multipleOf", -1}}),
              "JSON Schema is invalid at '/multipleOf': must be greater than 0");
    EXPECT_EQ(message({{"type", {"string", "integr"}}}),
              "JSON Schema is invalid at '/type/1': 'integr' is not a JSON Schema type");
    EXPECT_EQ(message({{"type", {1}}}),
              "JSON Schema is invalid at '/type/0': expected string, found integer");
    EXPECT_EQ(message({{"type", true}}),
              "JSON Schema is invalid at '/type': expected string or array, found boolean");
    EXPECT_EQ(message({{"type", json::array()}}),
              "JSON Schema is invalid at '/type': must not be empty");
    EXPECT_EQ(message({{"type", {"string", "string"}}}),
              "JSON Schema is invalid at '/type': items must be unique");
    EXPECT_EQ(message({{"$vocabulary", 1}}),
              "JSON Schema is invalid at '/$vocabulary': expected object, found integer");
    EXPECT_EQ(message({{"$vocabulary", {{"https://example.com/v", 1}}}}),
              "JSON Schema is invalid at '/$vocabulary/https:~1~1example.com~1v': expected boolean, "
              "found integer");
    EXPECT_EQ(message({{"prefixItems", json::array()}}),
              "JSON Schema is invalid at '/prefixItems': must not be empty");
    EXPECT_EQ(message({{"$dynamicAnchor", "a:b"}}),
              "JSON Schema is invalid at '/$dynamicAnchor': 'a:b' is not an anchor name");
    EXPECT_EQ(message({{"$schema", "http://json-schema.org/draft-07/schema#"}, {"examples", 1}}),
              "JSON Schema is invalid at '/examples': expected array, found integer");
    EXPECT_EQ(message({{"$schema", "http://json-schema.org/draft-07/schema#"}, {"readOnly", 1}}),
              "JSON Schema is invalid at '/readOnly': expected boolean, found integer");
    EXPECT_EQ(
        message({{"$schema", "http://json-schema.org/draft-04/schema#"}, {"required", json::array()}}),
        "JSON Schema is invalid at '/required': must not be empty");
    EXPECT_EQ(
        message({{"$schema", "http://json-schema.org/draft-04/schema#"}, {"enum", json::array()}}),
        "JSON Schema is invalid at '/enum': must not be empty");
    EXPECT_EQ(message({{"$schema", "http://json-schema.org/draft-04/schema#"}, {"id", 1}}),
              "JSON Schema is invalid at '/id': expected string, found integer");
    EXPECT_EQ(
        message({{"$schema", "https://json-schema.org/draft/2019-09/schema"}, {"$recursiveAnchor", 1}}),
        "JSON Schema is invalid at '/$recursiveAnchor': expected boolean, found integer");
    EXPECT_EQ(message({{"$schema", "https://json-schema.org/draft/2019-09/schema"}, {"items", {1}}}),
              "JSON Schema is invalid at '/items/0': expected object or boolean, found integer");
}

// Anchor names follow each dialect's pattern: 2019-09 allows ':' and 2020-12 a leading '_'.
TEST(SchemaChecker, AnchorNamesFollowTheDialect) {
    const auto passes = [](const char* dialect, const char* name) {
        try {
            (void)CompiledSchema::compile({{"$schema", dialect}, {"$anchor", name}});
            return true;
        } catch (const SchemaError&) {
            return false;
        }
    };
    constexpr const char* g_2019 = "https://json-schema.org/draft/2019-09/schema";
    constexpr const char* g_2020 = "https://json-schema.org/draft/2020-12/schema";
    EXPECT_TRUE(passes(g_2019, "a:b-c.d_9"));
    EXPECT_FALSE(passes(g_2019, "_a"));
    EXPECT_TRUE(passes(g_2020, "_a-b.c9"));
    EXPECT_FALSE(passes(g_2020, "a:b"));
    EXPECT_FALSE(passes(g_2020, "a b"));
}

// Keywords a dialect does not define are left alone, whatever their value.
TEST(SchemaChecker, LeavesUnknownKeywordsAlone) {
    EXPECT_NO_THROW((void)CompiledSchema::compile({{"x-anything", {{"type", 5}}}}));
    EXPECT_NO_THROW(
        (void)CompiledSchema::compile({{"$schema", "http://json-schema.org/draft-04/schema#"},
                                       {"const", {{"type", 5}}},
                                       {"$id", 5}}));
}

// draft-04 allows a boolean for additionalItems and additionalProperties only.
TEST(SchemaChecker, Draft4BooleansOnlyWhereItsMetaschemaAllowsThem) {
    const json schema = {{"$schema", "http://json-schema.org/draft-04/schema#"},
                         {"items", json::array({{{"type", "integer"}}})},
                         {"additionalItems", false},
                         {"additionalProperties", false}};
    const auto compiled = CompiledSchema::compile(schema);
    EXPECT_TRUE(compiled.validate(json::array({1})).valid);
    EXPECT_FALSE(compiled.validate(json::array({1, 2})).valid);
    EXPECT_FALSE(compiled.validate({{"a", 1}}).valid);
}

}  // namespace

}  // namespace json_schema_checker_test

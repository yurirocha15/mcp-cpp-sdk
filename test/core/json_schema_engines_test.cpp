#include <mcp/protocol/schema.hpp>

#include "../../src/protocol/schema_cache.hpp"
#include "../support/schema_corpus.hpp"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>

namespace json_schema_engines_test {

namespace {

using mcp::CompiledSchema;
namespace schema_corpus = mcp::test::schema_corpus;
using mcp::detail::SchemaEngine;
using mcp::detail::ScopedSchemaEngine;
using nlohmann::json;

/// @p schema compiled with @p engine, or nothing when it is refused.
std::optional<CompiledSchema> try_compile(const json& schema, SchemaEngine engine) {
    const ScopedSchemaEngine forced(engine);
    try {
        return CompiledSchema::compile(schema);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::string first_message(const CompiledSchema& schema, const json& value) {
    const auto result = schema.validate(value);
    return result.violations.empty() ? std::string() : result.violations[0].message;
}

// A forced evaluation is the one asked for, whatever was compiled before, and neither reads nor
// fills the cache: otherwise a comparison of the two evaluations could compare one with itself.
TEST(SchemaEngines, ForcedEngineBypassesTheCache) {
    const json schema = {{"enum", {1, 2}}};
    const auto cached = CompiledSchema::compile(schema);
    const auto live = mcp::detail::compiled_schema_cache_live_entries();

    const auto native = try_compile(schema, SchemaEngine::eNative);
    const auto jsoncons = try_compile(schema, SchemaEngine::eJsoncons);
    ASSERT_TRUE(native.has_value());
    ASSERT_TRUE(jsoncons.has_value());
    EXPECT_EQ(mcp::detail::compiled_schema_cache_live_entries(), live);
    EXPECT_NE(&native->schema(), &cached.schema());
    EXPECT_NE(&jsoncons->schema(), &cached.schema());
    EXPECT_NE(&native->schema(), &jsoncons->schema());

    // The two evaluations word an enum violation differently, which tells them apart.
    EXPECT_EQ(first_message(*native, 7), "Value is not one of the values the enum lists");
    EXPECT_EQ(first_message(*jsoncons, 7), "'7' is not a valid enum value.");
    EXPECT_EQ(first_message(cached, 7), first_message(*native, 7));

    // Scopes nest and restore the evaluation in force before them.
    {
        const ScopedSchemaEngine outer(SchemaEngine::eJsoncons);
        {
            const ScopedSchemaEngine inner(SchemaEngine::eNative);
            EXPECT_EQ(first_message(CompiledSchema::compile(schema), 7), first_message(*native, 7));
        }
        EXPECT_EQ(first_message(CompiledSchema::compile(schema), 7), first_message(*jsoncons, 7));
    }
    EXPECT_EQ(mcp::detail::compiled_schema_cache_live_entries(), live);
}

// The native evaluation refuses a schema it does not implement instead of handing it to jsoncons.
TEST(SchemaEngines, ForcedNativeRefusesWhatItDoesNotImplement) {
    const ScopedSchemaEngine native(SchemaEngine::eNative);
    EXPECT_THROW((void)CompiledSchema::compile({{"unevaluatedProperties", false}}), std::domain_error);
    // The checks every schema gets still come first.
    EXPECT_THROW((void)CompiledSchema::compile({{"type", 1}}), mcp::SchemaError);
}

// A violation's location is the same JSON Pointer from both evaluations, also deeper than the
// steps the native evaluation keeps inline, with member names escaped and element indexes.
TEST(SchemaEngines, ReportTheSameDeepLocations) {
    json schema = {{"type", "integer"}};
    json value = "leaf";
    std::string expected;
    for (int level = 11; level >= 0; --level) {
        if (level % 3 == 0) {
            schema = {{"type", "array"}, {"items", schema}};
            value = json::array({value});
            expected.insert(0, "/0");
        } else {
            const std::string name = level % 3 == 1 ? "a/b" : "c~d";
            schema = {{"type", "object"}, {"properties", {{name, schema}}}};
            value = {{name, value}};
            expected.insert(0, level % 3 == 1 ? "/a~1b" : "/c~0d");
        }
    }
    const auto native = try_compile(schema, SchemaEngine::eNative);
    const auto jsoncons = try_compile(schema, SchemaEngine::eJsoncons);
    ASSERT_TRUE(native.has_value());
    ASSERT_TRUE(jsoncons.has_value());
    const auto native_result = native->validate(value);
    const auto jsoncons_result = jsoncons->validate(value);
    ASSERT_EQ(native_result.violations.size(), 1U);
    ASSERT_EQ(jsoncons_result.violations.size(), 1U);
    EXPECT_EQ(native_result.violations[0].instance_path, expected);
    EXPECT_EQ(jsoncons_result.violations[0].instance_path, expected);
}

// Every case of the JSON-Schema-Test-Suite data of the five dialects, the excluded groups too,
// through the evaluation a schema gets on its own and through the jsoncons evaluation: both must
// give the same verdict, so a keyword or dialect moving to the native evaluation cannot change a
// result.
TEST(SchemaEngines, AgreeOnEverySuiteCase) {
    std::size_t compared = 0;
    for (const auto& dialect : schema_corpus::dialects()) {
        for (const auto& file : schema_corpus::suite_files(dialect)) {
            for (std::size_t index = 0; index < file.groups.size(); ++index) {
                const auto& group = file.groups[index];
                const auto name = std::string(dialect.directory) + "/" + file.name + " group " +
                                  std::to_string(index);
                const auto own = try_compile(group.schema, SchemaEngine::eAutomatic);
                const auto forced = try_compile(group.schema, SchemaEngine::eJsoncons);
                EXPECT_EQ(own.has_value(), forced.has_value()) << name << ": compile outcomes differ";
                compared += group.values.size();
                if (!own || !forced) {
                    continue;
                }
                for (std::size_t value = 0; value < group.values.size(); ++value) {
                    EXPECT_EQ(own->validate(group.values[value]).valid,
                              forced->validate(group.values[value]).valid)
                        << name << " value " << value;
                }
            }
        }
    }
    // 4941 cases: the run and the excluded ones of the five dialects' suite data.
    EXPECT_EQ(compared, 4941U);
}

}  // namespace

}  // namespace json_schema_engines_test

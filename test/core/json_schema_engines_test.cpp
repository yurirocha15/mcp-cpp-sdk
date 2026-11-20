#include <mcp/protocol/schema.hpp>

#include "../../src/protocol/schema_cache.hpp"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace json_schema_engines_test {

namespace {

using mcp::CompiledSchema;
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
    EXPECT_THROW((void)CompiledSchema::compile({{"anyOf", {{{"type", "integer"}}}}}),
                 std::domain_error);
    // The checks every schema gets still come first.
    EXPECT_THROW((void)CompiledSchema::compile({{"type", 1}}), mcp::SchemaError);
}

// Every case of the JSON-Schema-Test-Suite's 2020-12 data, the excluded groups too, through the
// evaluation a schema gets on its own and through the jsoncons evaluation: both must give the
// same verdict, so a keyword moving to the native evaluation cannot change a result. Draft-07
// schemas always go to jsoncons, so only 2020-12 has two evaluations to compare.
TEST(SchemaEngines, AgreeOnEverySuiteCase) {
    const std::filesystem::path dir =
        std::filesystem::path(MCP_TEST_DATA_DIR) / "json-schema" / "draft2020-12";
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (entry.path().extension() == ".json") {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());

    std::size_t compared = 0;
    for (const auto& file : files) {
        std::ifstream stream(file);
        const json groups = json::parse(stream);
        for (const auto& group : groups) {
            const auto name =
                file.filename().string() + " | " + group.at("description").get<std::string>();
            const auto& schema_json = group.at("schema");
            const auto own = try_compile(schema_json, SchemaEngine::eAutomatic);
            const auto forced = try_compile(schema_json, SchemaEngine::eJsoncons);
            EXPECT_EQ(own.has_value(), forced.has_value()) << name << ": compile outcomes differ";
            const auto& tests = group.at("tests");
            if (!own || !forced) {
                compared += tests.size();
                continue;
            }
            for (const auto& test : tests) {
                ++compared;
                EXPECT_EQ(own->validate(test.at("data")).valid, forced->validate(test.at("data")).valid)
                    << name << " | " << test.at("description").get<std::string>();
            }
        }
    }
    // 1300 cases: the run and the excluded ones of the 2020-12 suite data.
    EXPECT_EQ(compared, 1300u);
}

}  // namespace

}  // namespace json_schema_engines_test

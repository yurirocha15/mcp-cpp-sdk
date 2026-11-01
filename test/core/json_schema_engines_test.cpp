#include <mcp/protocol/schema.hpp>

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace {

using mcp::CompiledSchema;
using nlohmann::json;

/// A schema with the same meaning that only the jsoncons evaluation takes. `$defs` (and `allOf`,
/// for a boolean schema) is a keyword the native evaluation leaves to jsoncons, and it adds no
/// constraint.
json through_jsoncons(const json& schema) {
    if (schema.is_object()) {
        json forced = schema;
        if (!forced.contains("$defs")) {
            forced["$defs"] = json::object();
        }
        return forced;
    }
    return {{"allOf", json::array({schema})}};
}

std::optional<CompiledSchema> try_compile(const json& schema) {
    try {
        return CompiledSchema::compile(schema);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

// Every case of the JSON-Schema-Test-Suite's 2020-12 data, the excluded groups too, through the
// evaluation a schema gets on its own and through the one that forces jsoncons: both must give the
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
            const auto own = try_compile(schema_json);
            const auto forced = try_compile(through_jsoncons(schema_json));
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
    // 1302 cases: the run and the excluded ones of the 2020-12 suite data.
    EXPECT_EQ(compared, 1302u);
}

}  // namespace

#include <mcp/protocol/schema.hpp>

#include "../../src/detail/constants.hpp"
#include "../../src/protocol/schema_cache.hpp"
#include "../support/schema_corpus.hpp"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdio>
#include <fstream>
#include <functional>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <pthread.h>
#endif

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

/// Runs @p work on a thread whose stack is @p stack_bytes. Windows threads here get the default
/// stack, which is larger.
void run_on_small_stack(std::size_t stack_bytes, const std::function<void()>& work) {
#if defined(_WIN32)
    (void)stack_bytes;
    std::thread thread(work);
    thread.join();
#else
    pthread_attr_t attributes;
    ASSERT_EQ(pthread_attr_init(&attributes), 0);
    ASSERT_EQ(pthread_attr_setstacksize(&attributes, stack_bytes), 0);
    pthread_t thread{};
    auto* task = new std::function<void()>(work);
    const auto start = [](void* argument) -> void* {
        auto* job = static_cast<std::function<void()>*>(argument);
        (*job)();
        delete job;
        return nullptr;
    };
    ASSERT_EQ(pthread_create(&thread, &attributes, start, task), 0);
    pthread_attr_destroy(&attributes);
    ASSERT_EQ(pthread_join(thread, nullptr), 0);
#endif
}

/// A way of nesting schemas: the schema and a value that reaches its innermost level.
struct Nesting {
    const char* name;
    std::function<json(const json&)> wrap_schema;
    std::function<json(const json&)> wrap_value;
    bool flips;  ///< Each level inverts the verdict.
};

json nested(const std::function<json(const json&)>& wrap, json inner, std::size_t levels) {
    for (std::size_t level = 0; level < levels; ++level) {
        inner = wrap(inner);
    }
    return inner;
}

// The deepest schema of each nesting that compiles is evaluated natively on a 512 KiB stack, the
// smallest a secondary thread gets, with a value that reaches its innermost level.
TEST(SchemaEngines, DeepestNativeSchemasRunOnASmallStack) {
    constexpr std::size_t g_small_stack = 512 * 1024;
    constexpr std::size_t g_most_levels = 192;
    const auto same = [](const json& inner) { return inner; };
    const std::vector<Nesting> nestings = {
        {"properties", [](const json& inner) { return json{{"properties", {{"n", inner}}}}; },
         [](const json& inner) { return json{{"n", inner}}; }, false},
        {"items", [](const json& inner) { return json{{"items", inner}}; },
         [](const json& inner) { return json::array({inner}); }, false},
        {"allOf", [](const json& inner) { return json{{"allOf", json::array({inner})}}; }, same, false},
        {"anyOf", [](const json& inner) { return json{{"anyOf", json::array({inner})}}; }, same, false},
        {"not", [](const json& inner) { return json{{"not", inner}}; }, same, true},
    };
    const json leaf = {{"type", "integer"}};
    for (const auto& nesting : nestings) {
        const auto compiles = [&](std::size_t levels) {
            return try_compile(nested(nesting.wrap_schema, leaf, levels), SchemaEngine::eNative)
                .has_value();
        };
        // The deepest nesting that compiles: compiling fails from some depth on.
        std::size_t deepest = 0;
        std::size_t refused = g_most_levels + 1;
        while (refused - deepest > 1) {
            const auto middle = deepest + (refused - deepest) / 2;
            (compiles(middle) ? deepest : refused) = middle;
        }
        std::printf("%s: %zu levels compile natively\n", nesting.name, deepest);
        EXPECT_GE(deepest, 8U) << nesting.name;
        const auto schema =
            try_compile(nested(nesting.wrap_schema, leaf, deepest), SchemaEngine::eNative);
        ASSERT_TRUE(schema.has_value()) << nesting.name;
        const auto good = nested(nesting.wrap_value, 1, deepest);
        const auto bad = nested(nesting.wrap_value, "x", deepest);
        const bool good_passes = !nesting.flips || deepest % 2 == 0;
        run_on_small_stack(g_small_stack, [&] {
            const auto good_result = schema->validate(good);
            const auto bad_result = schema->validate(bad);
            EXPECT_EQ(good_result.valid, good_passes) << nesting.name;
            EXPECT_EQ(bad_result.valid, !good_passes) << nesting.name;
            for (const auto& violation : good_result.violations) {
                EXPECT_EQ(violation.message.find("validation stopped"), std::string::npos)
                    << nesting.name;
            }
            for (const auto& violation : bad_result.violations) {
                EXPECT_EQ(violation.message.find("validation stopped"), std::string::npos)
                    << nesting.name;
            }
        });
    }
}

/// A chain of @p links `$defs` entries, each reaching the next through @p wrap, the last checking
/// for an integer; the root refers to the first.
json reference_chain(std::size_t links, const std::function<json(const json&)>& wrap) {
    json defs = json::object();
    for (std::size_t link = 0; link < links; ++link) {
        defs["a" + std::to_string(link)] = wrap({{"$ref", "#/$defs/a" + std::to_string(link + 1)}});
    }
    defs["a" + std::to_string(links)] = {{"type", "integer"}};
    return {{"$defs", defs}, {"$ref", "#/$defs/a0"}};
}

// The longest reference chains that compile reach the bound of nested evaluations. They are
// evaluated on a 512 KiB stack; one link more is refused when compiled, since it stops for any
// value.
TEST(SchemaEngines, DeepestReferenceChainsRunOnASmallStack) {
    constexpr std::size_t g_small_stack = 512 * 1024;
    const std::vector<std::pair<const char*, std::function<json(const json&)>>> shapes = {
        {"$ref", [](const json& reference) { return reference; }},
        {"allOf", [](const json& reference) { return json{{"allOf", json::array({reference})}}; }},
    };
    for (const auto& [name, wrap] : shapes) {
        const auto compiles = [&wrap = wrap](std::size_t links) {
            return try_compile(reference_chain(links, wrap), SchemaEngine::eNative).has_value();
        };
        std::size_t deepest = 0;
        std::size_t refused = mcp::detail::constants::schema::g_MAX_EVAL_DEPTH + 1;
        while (refused - deepest > 1) {
            const auto middle = deepest + (refused - deepest) / 2;
            (compiles(middle) ? deepest : refused) = middle;
        }
        std::printf("%s chain: %zu links compile natively\n", name, deepest);
        EXPECT_GE(deepest, 90U) << name;
        EXPECT_FALSE(compiles(deepest + 1)) << name;
        const auto schema = try_compile(reference_chain(deepest, wrap), SchemaEngine::eNative);
        ASSERT_TRUE(schema.has_value()) << name;
        run_on_small_stack(g_small_stack, [&schema, name = name] {
            EXPECT_TRUE(schema->validate(1).valid) << name;
            const auto result = schema->validate("x");
            ASSERT_EQ(result.violations.size(), 1U) << name;
            EXPECT_EQ(result.violations[0].message.find("validation stopped"), std::string::npos)
                << name;
        });
    }
}

// Compiling resolves references after walking the document, so a chain far longer than the bound
// of nested evaluations does not deepen compilation; it compiles on a 512 KiB stack and is refused,
// since it stops for any value. The same holds for $dynamicRef through many resources that declare
// the anchor it looks for.
TEST(SchemaEngines, LongReferenceChainsCompileOnASmallStack) {
    constexpr std::size_t g_small_stack = 512 * 1024;
    const auto chain = reference_chain(5000, [](const json& reference) { return reference; });
    json resources = json::object();
    for (int i = 0; i < 1000; ++i) {
        resources["r" + std::to_string(i)] = {
            {"$id", "r" + std::to_string(i)}, {"$dynamicAnchor", "x"}, {"$dynamicRef", "#x"}};
    }
    const json dynamic = {{"$id", "https://example.com/root"},
                          {"$dynamicAnchor", "x"},
                          {"$dynamicRef", "#x"},
                          {"$defs", resources}};
    for (const auto* document : {&chain, &dynamic}) {
        run_on_small_stack(g_small_stack, [document] {
            const ScopedSchemaEngine native(SchemaEngine::eNative);
            try {
                (void)CompiledSchema::compile(*document);
                ADD_FAILURE() << "a reference cycle or overlong chain compiled";
            } catch (const mcp::SchemaError& error) {
                EXPECT_EQ(error.category(), mcp::SchemaError::Category::eInvalidSchema);
                EXPECT_NE(std::string(error.what()).find("nests too deeply"), std::string::npos)
                    << error.what();
            }
        });
    }
}

/// The suite groups test/data/schema-engine-differences.json lists, as "dialect/file group index".
std::set<std::string> listed_suite_differences() {
    std::ifstream stream(std::string(MCP_TEST_DATA_DIR) + "/schema-engine-differences.json");
    const auto table = json::parse(stream);
    std::set<std::string> listed;
    for (const auto& row : table.at("differences")) {
        if (row.at("corpus") == "suite") {
            listed.insert(row.at("dialect").get<std::string>() + "/" +
                          row.at("file").get<std::string>() + " group " +
                          std::to_string(row.at("index").get<std::size_t>()));
        }
    }
    return listed;
}

// Every case of the JSON-Schema-Test-Suite data of the five dialects, the excluded groups too,
// through the evaluation a schema gets on its own and through the jsoncons evaluation: both must
// give the same verdict, so a keyword or dialect moving to the native evaluation cannot change a
// result. A group the difference table lists must differ in at least one case.
TEST(SchemaEngines, AgreeOnEverySuiteCase) {
    const auto listed = listed_suite_differences();
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
                const bool expect_difference = listed.count(name) != 0;
                bool differs = false;
                for (std::size_t value = 0; value < group.values.size(); ++value) {
                    const bool own_valid = own->validate(group.values[value]).valid;
                    const bool forced_valid = forced->validate(group.values[value]).valid;
                    differs = differs || own_valid != forced_valid;
                    if (!expect_difference) {
                        EXPECT_EQ(own_valid, forced_valid) << name << " value " << value;
                    }
                }
                EXPECT_TRUE(!expect_difference || differs)
                    << name << ": listed, but the evaluations agree";
            }
        }
    }
    // 4941 cases: the run and the excluded ones of the five dialects' suite data.
    EXPECT_EQ(compared, 4941U);
}

}  // namespace

}  // namespace json_schema_engines_test

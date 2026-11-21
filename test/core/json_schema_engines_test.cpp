#include <mcp/protocol/schema.hpp>

#include "../../src/protocol/schema_cache.hpp"
#include "../support/schema_corpus.hpp"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdio>
#include <functional>
#include <optional>
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

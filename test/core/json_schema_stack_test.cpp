#include <mcp/protocol/schema.hpp>

#include "../../src/detail/constants.hpp"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdio>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <pthread.h>
#endif

namespace json_schema_stack_test {

namespace {

using mcp::CompiledSchema;
using nlohmann::json;

/// @p schema compiled, or nothing when it is refused.
std::optional<CompiledSchema> try_compile(const json& schema) {
    try {
        return CompiledSchema::compile(schema);
    } catch (const std::exception&) {
        return std::nullopt;
    }
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

// The deepest schema of each nesting that compiles is evaluated on a 512 KiB stack, the
// smallest a secondary thread gets, with a value that reaches its innermost level.
TEST(SchemaStack, DeepestNativeSchemasRunOnASmallStack) {
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
            return try_compile(nested(nesting.wrap_schema, leaf, levels)).has_value();
        };
        // The deepest nesting that compiles: compiling fails from some depth on.
        std::size_t deepest = 0;
        std::size_t refused = g_most_levels + 1;
        while (refused - deepest > 1) {
            const auto middle = deepest + (refused - deepest) / 2;
            (compiles(middle) ? deepest : refused) = middle;
        }
        std::printf("%s: %zu levels compile\n", nesting.name, deepest);
        EXPECT_GE(deepest, 8U) << nesting.name;
        const auto schema = try_compile(nested(nesting.wrap_schema, leaf, deepest));
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
TEST(SchemaStack, DeepestReferenceChainsRunOnASmallStack) {
    constexpr std::size_t g_small_stack = 512 * 1024;
    const std::vector<std::pair<const char*, std::function<json(const json&)>>> shapes = {
        {"$ref", [](const json& reference) { return reference; }},
        {"allOf", [](const json& reference) { return json{{"allOf", json::array({reference})}}; }},
    };
    for (const auto& [name, wrap] : shapes) {
        const auto compiles = [&wrap = wrap](std::size_t links) {
            return try_compile(reference_chain(links, wrap)).has_value();
        };
        std::size_t deepest = 0;
        std::size_t refused = mcp::detail::constants::schema::g_MAX_EVAL_DEPTH + 1;
        while (refused - deepest > 1) {
            const auto middle = deepest + (refused - deepest) / 2;
            (compiles(middle) ? deepest : refused) = middle;
        }
        std::printf("%s chain: %zu links compile\n", name, deepest);
        EXPECT_GE(deepest, 90U) << name;
        EXPECT_FALSE(compiles(deepest + 1)) << name;
        const auto schema = try_compile(reference_chain(deepest, wrap));
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
TEST(SchemaStack, LongReferenceChainsCompileOnASmallStack) {
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

// A reference to a metaschema checks the value as a schema of that dialect, within the bound of
// nested evaluations, which a schema nesting `properties` as deep as a schema document may nest
// stays within. It runs on a 512 KiB stack.
TEST(SchemaStack, MetaschemaReferenceRunsOnASmallStack) {
    constexpr std::size_t g_small_stack = 512 * 1024;
    const auto schema = CompiledSchema::compile(
        {{"properties", {{"s", {{"$ref", "https://json-schema.org/draft/2020-12/schema"}}}}}});
    // Each `properties` level is two levels of the value.
    const std::size_t levels = mcp::detail::constants::schema::g_MAX_DEPTH / 2 - 1;
    const json good = nested([](const json& inner) { return json{{"properties", {{"n", inner}}}}; },
                             {{"type", "integer"}}, levels);
    const json bad = nested([](const json& inner) { return json{{"properties", {{"n", inner}}}}; },
                            {{"type", "integr"}}, levels);
    run_on_small_stack(g_small_stack, [&] {
        EXPECT_TRUE(schema.validate({{"s", good}}).valid);
        const auto result = schema.validate({{"s", bad}});
        ASSERT_EQ(result.violations.size(), 1U);
        EXPECT_EQ(result.violations[0].instance_path, "/s");
        EXPECT_NE(result.violations[0].message.find("is not a JSON Schema type"), std::string::npos)
            << result.violations[0].message;
    });
}

}  // namespace

}  // namespace json_schema_stack_test

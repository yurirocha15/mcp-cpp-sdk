// A fixed schema validation workload for instruction counting (scripts/schema_ir_gate.py).
//
//   mcp-schema-workload <group> <iterations>
//
// Compiles the group's schemas once and validates its values against them <iterations> times; the
// "compile" group instead compiles every schema of the other groups <iterations> times. Counting
// the instructions of two runs with different iteration counts and dividing the difference gives
// the cost of one iteration without the start-up. Uses only the public API, so the same source
// measures any build of the SDK.
//
// Prints a fingerprint of the group's schemas, values and options, so that two builds are only
// compared on the same workload, and the number of valid results, so that no validation is
// optimized away.

#include <mcp/protocol/schema.hpp>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using nlohmann::json;

struct Item {
    json schema;
    std::vector<json> values;
    mcp::SchemaOptions options;
};

json nested_object(int levels) {
    json schema = {{"type", "integer"}};
    for (int i = 0; i < levels; ++i) {
        schema = {{"type", "object"}, {"properties", {{"n", schema}}}, {"required", {"n"}}};
    }
    return schema;
}

json nested_value(int levels) {
    json value = 1;
    for (int i = 0; i < levels; ++i) {
        value = {{"n", value}};
    }
    return value;
}

/// The input schemas of the benchmark's three tools (benchmark/alternatives/common).
std::vector<Item> tools() {
    const json search = json::parse(
        R"({"type":"object","properties":{"category":{"type":"string"},"min_price":{"type":"number"},"max_price":{"type":"number"},"limit":{"type":"integer"}}})");
    const json cart = json::parse(R"({"type":"object","properties":{"user_id":{"type":"string"}}})");
    const json checkout = json::parse(
        R"({"type":"object","properties":{"user_id":{"type":"string"},"items":{"type":"array","items":{"type":"object","properties":{"product_id":{"type":"integer"},"quantity":{"type":"integer"}},"required":["product_id","quantity"]}}}})");
    return {
        {search,
         {json::parse(R"({"category":"electronics","min_price":10.5,"max_price":99.9,"limit":10})"),
          json::parse(R"({"category":7,"limit":"ten"})")},
         {}},
        {cart, {json::parse(R"({"user_id":"user-42"})"), json::parse(R"({"user_id":42})")}, {}},
        {checkout,
         {json::parse(
              R"({"user_id":"user-42","items":[{"product_id":1,"quantity":2},{"product_id":7,"quantity":1}]})"),
          json::parse(R"({"user_id":"user-42","items":[{"product_id":"x"}]})")},
         {}},
    };
}

/// Schemas the native evaluation implements.
std::vector<Item> native() {
    json flat = {{"type", "object"}, {"additionalProperties", false}, {"properties", json::object()}};
    json flat_value = json::object();
    for (int i = 0; i < 16; ++i) {
        const auto name = "p" + std::to_string(i);
        flat["properties"][name] = {{"type", i % 2 == 0 ? "string" : "integer"}};
        flat_value[name] = i % 2 == 0 ? json("text") : json(i);
    }
    json allowed = json::array();
    for (int i = 0; i < 64; ++i) {
        allowed.push_back("code-" + std::to_string(i));
    }
    return {
        {flat, {flat_value}, {}},
        {nested_object(8), {nested_value(8)}, {}},
        {{{"enum", allowed}}, {"code-63", "other"}, {}},
        {{{"type", "array"}, {"items", {{"type", "number"}, {"minimum", 0}}}}, {json(1000, 1.5)}, {}},
    };
}

/// Schemas built on applicators and references.
std::vector<Item> applicators() {
    json defs = json::object();
    json branches = json::array();
    for (int i = 0; i < 20; ++i) {
        const auto name = "T" + std::to_string(i);
        defs[name] = {{"type", "object"},
                      {"properties", {{"kind", {{"const", "t" + std::to_string(i)}}}}},
                      {"required", {"kind"}}};
        branches.push_back({{"$ref", "#/$defs/" + name}});
    }
    const json tree = json::parse(R"({
        "$defs": {"node": {"type": "object", "properties": {
            "value": {"type": "integer"},
            "children": {"type": "array", "items": {"$ref": "#/$defs/node"}}}}},
        "$ref": "#/$defs/node"})");
    json tree_value = {{"value", 1}, {"children", json::array()}};
    for (int i = 0; i < 6; ++i) {
        tree_value = {{"value", i}, {"children", {tree_value, tree_value}}};
    }
    json unique = json::array();
    for (int i = 0; i < 1000; ++i) {
        unique.push_back(i);
    }
    return {
        {{{"$defs", defs}, {"type", "array"}, {"items", {{"anyOf", branches}}}},
         {json(50, {{"kind", "t19"}})},
         {}},
        {tree, {tree_value}, {}},
        {{{"oneOf", {{{"type", "string"}}, {{"type", "integer"}}, {{"type", "null"}}}}},
         {"x", 1, 1.5},
         {}},
        {{{"patternProperties", {{"^s_", {{"type", "string"}}}, {"^n_", {{"type", "number"}}}}},
          {"additionalProperties", false}},
         {json::parse(R"({"s_a": "x", "n_b": 1, "s_c": "y", "n_d": 2.5})")},
         {}},
        {json::parse(R"({"allOf": [{"properties": {"a": {"type": "integer"}}}],
                         "properties": {"b": {"type": "string"}},
                         "unevaluatedProperties": false})"),
         {json::parse(R"({"a": 1, "b": "x"})"), json::parse(R"({"a": 1, "c": true})")},
         {}},
        {{{"uniqueItems", true}}, {unique}, {}},
        {{{"if", {{"type", "string"}}}, {"then", {{"minLength", 2}}}, {"else", {{"type", "integer"}}}},
         {"ab", "a", 3, 1.5},
         {}},
    };
}

/// Values whose validation is stopped by the work budget.
std::vector<Item> stops() {
    json allowed = json::array();
    for (int i = 0; i < 500; ++i) {
        allowed.push_back(1000000 + i);
    }
    mcp::SchemaOptions enum_budget;
    enum_budget.max_evaluation_work = 1'000'000;
    json lengths = json::array();
    for (int i = 0; i < 200; ++i) {
        lengths.push_back({{"minLength", 2'000'000 + i}});
    }
    mcp::SchemaOptions length_budget;
    length_budget.max_evaluation_work = 250'000;
    mcp::SchemaOptions property_budget;
    property_budget.max_evaluation_work = 100'000;
    json many = json::object();
    for (int i = 0; i < 200000; ++i) {
        many["p" + std::to_string(i)] = 0;
    }
    return {
        {{{"items", {{"enum", allowed}}}}, {json(500, allowed.back())}, enum_budget},
        {{{"anyOf", lengths}}, {std::string(std::size_t{32} * 1024, 'x')}, length_budget},
        {{{"properties", {{"q", json::object()}}}, {"allOf", {{{"minProperties", 1}}}}},
         {many},
         property_budget},
    };
}

std::vector<Item> group_items(const std::string& group) {
    if (group == "tools") {
        return tools();
    }
    if (group == "native") {
        return native();
    }
    if (group == "applicators") {
        return applicators();
    }
    if (group == "stops") {
        return stops();
    }
    if (group == "compile") {
        std::vector<Item> all;
        for (auto&& part : {tools(), native(), applicators()}) {
            all.insert(all.end(), part.begin(), part.end());
        }
        return all;
    }
    throw std::invalid_argument("unknown group: " + group);
}

std::uint64_t fnv1a(std::uint64_t hash, const std::string& text) {
    for (const unsigned char byte : text) {
        hash ^= byte;
        hash *= 0x100000001B3ULL;
    }
    return hash;
}

std::uint64_t fingerprint(const std::string& group, const std::vector<Item>& items) {
    std::uint64_t hash = fnv1a(0xCBF29CE484222325ULL, group);
    for (const auto& item : items) {
        hash = fnv1a(hash, item.schema.dump());
        for (const auto& value : item.values) {
            hash = fnv1a(hash, value.dump());
        }
        hash = fnv1a(hash, std::to_string(item.options.max_evaluation_work) + "/" +
                               std::to_string(item.options.max_reported_violations));
    }
    return hash;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s <tools|native|applicators|stops|compile> <iterations>\n",
                     argv[0]);
        return 2;
    }
    try {
        const std::string group = argv[1];
        const auto iterations = std::stoul(argv[2]);
        const auto items = group_items(group);
        std::size_t valid = 0;
        if (group == "compile") {
            // A compiled schema nothing holds is not returned from the cache, so every iteration
            // compiles each schema again.
            for (unsigned long i = 0; i < iterations; ++i) {
                for (const auto& item : items) {
                    valid += mcp::CompiledSchema::compile(item.schema, item.options).schema().size();
                }
            }
        } else {
            std::vector<mcp::CompiledSchema> compiled;
            compiled.reserve(items.size());
            for (const auto& item : items) {
                compiled.push_back(mcp::CompiledSchema::compile(item.schema, item.options));
            }
            for (unsigned long i = 0; i < iterations; ++i) {
                for (std::size_t index = 0; index < items.size(); ++index) {
                    for (const auto& value : items[index].values) {
                        valid += compiled[index].validate(value).valid ? 1 : 0;
                    }
                }
            }
        }
        if (group == "stops" && valid != 0) {
            // A value that passes was not stopped, so it measures something else.
            std::fprintf(stderr, "workload failed: a validation of the stops group was not stopped\n");
            return 1;
        }
        std::printf("fingerprint %016llx valid %zu\n",
                    static_cast<unsigned long long>(fingerprint(group, items)), valid);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "workload failed: %s\n", error.what());
        return 1;
    }
    return 0;
}

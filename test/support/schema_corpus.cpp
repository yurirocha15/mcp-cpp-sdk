#include "schema_corpus.hpp"

#include <mcp/protocol/schema.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <utility>

namespace mcp::test::schema_corpus {

namespace {

using nlohmann::json;

enum class Generation : std::uint8_t {
    eDraft4,
    eDraft6,
    eDraft7,
    e2019_09,
    e2020_12,
};

Generation generation_of(const Dialect& dialect) {
    const std::string directory = dialect.directory;
    if (directory == "draft2020-12") {
        return Generation::e2020_12;
    }
    if (directory == "draft2019-09") {
        return Generation::e2019_09;
    }
    if (directory == "draft7") {
        return Generation::eDraft7;
    }
    if (directory == "draft6") {
        return Generation::eDraft6;
    }
    return Generation::eDraft4;
}

/// splitmix64: the same sequence on every platform, unlike the standard distributions.
class Random {
   public:
    explicit Random(std::uint64_t seed) : state_(seed) {}

    std::uint64_t next() {
        state_ += 0x9E3779B97F4A7C15ULL;
        std::uint64_t mixed = state_;
        mixed = (mixed ^ (mixed >> 30U)) * 0xBF58476D1CE4E5B9ULL;
        mixed = (mixed ^ (mixed >> 27U)) * 0x94D049BB133111EBULL;
        return mixed ^ (mixed >> 31U);
    }

    /// A number in [0, bound).
    std::size_t below(std::size_t bound) { return static_cast<std::size_t>(next() % bound); }

    /// True with @p percent percent probability.
    bool chance(std::size_t percent) { return below(100) < percent; }

    template <typename T, std::size_t N>
    const T& pick(const std::array<T, N>& items) {
        return items[below(N)];
    }

   private:
    std::uint64_t state_;
};

constexpr std::array<const char*, 4> g_NAMES{"a", "b", "c", "d"};
constexpr std::array<const char*, 7> g_TYPES{"null",   "boolean", "integer", "number",
                                             "string", "array",   "object"};
constexpr std::array<const char*, 5> g_PATTERNS{"^a", "b$", "^[0-9]+$", "x*", "^.{2}$"};

json make_scalar(Random& random) {
    switch (random.below(12)) {
        case 0:
            return nullptr;
        case 1:
            return true;
        case 2:
            return false;
        case 3:
            return 0;
        case 4:
            return -1;
        case 5:
            return static_cast<int>(random.below(6));
        case 6:
            return 1.5;
        case 7:
            return 2.0;
        case 8:
            return "";
        case 9:
            return "a";
        case 10:
            return "ab";
        default:
            return random.chance(50) ? "123" : "xb";
    }
}

json make_value(Random& random, int depth) {
    if (depth > 0 && random.chance(25)) {
        json array = json::array();
        const auto size = random.below(4);
        for (std::size_t i = 0; i < size; ++i) {
            array.push_back(make_value(random, depth - 1));
        }
        return array;
    }
    if (depth > 0 && random.chance(30)) {
        json object = json::object();
        const auto size = random.below(4);
        for (std::size_t i = 0; i < size; ++i) {
            object[random.pick(g_NAMES)] = make_value(random, depth - 1);
        }
        return object;
    }
    return make_scalar(random);
}

class Generator {
   public:
    Generator(Generation generation, std::uint64_t seed) : generation_(generation), random_(seed) {}

    json schema(int depth) {
        json root = subschema(depth, true);
        if (root.is_object() && random_.chance(20)) {
            definitions_name();
            root[definitions_name()] = {{"d0", subschema(depth - 1, false)}};
            root["allOf"] = json::array({{{"$ref", "#/" + definitions_name() + "/d0"}}});
        }
        return root;
    }

    json value() { return make_value(random_, 3); }

   private:
    bool at_least(Generation generation) const { return generation_ >= generation; }

    std::string definitions_name() const {
        return at_least(Generation::e2019_09) ? "$defs" : "definitions";
    }

    json schemas(int depth) {
        json list = json::array();
        const auto size = 1 + random_.below(3);
        for (std::size_t i = 0; i < size; ++i) {
            list.push_back(subschema(depth, false));
        }
        return list;
    }

    json subschema(int depth, bool root) {
        if (!root && at_least(Generation::eDraft6) && random_.chance(8)) {
            return random_.chance(50);
        }
        json schema = json::object();
        if (depth <= 0) {
            if (random_.chance(50)) {
                schema["type"] = random_.pick(g_TYPES);
            }
            return schema;
        }
        const auto add = [&](std::size_t percent) { return random_.chance(percent); };
        if (add(35)) {
            if (random_.chance(70)) {
                schema["type"] = random_.pick(g_TYPES);
            } else {
                schema["type"] = json::array({random_.pick(g_TYPES), "null"});
            }
        }
        if (add(8)) {
            schema["enum"] = json::array({make_scalar(random_), make_value(random_, 1)});
        }
        if (at_least(Generation::eDraft6) && add(6)) {
            schema["const"] = make_value(random_, 1);
        }
        if (add(10)) {
            schema["minimum"] = static_cast<int>(random_.below(4));
        }
        if (add(10)) {
            schema["maximum"] = random_.chance(50) ? json(3) : json(2.5);
        }
        if (at_least(Generation::eDraft6) && add(6)) {
            schema["exclusiveMinimum"] = 0;
        }
        if (!at_least(Generation::eDraft6) && add(6)) {
            schema["maximum"] = 2;
            schema["exclusiveMaximum"] = random_.chance(50);
        }
        if (add(6)) {
            const std::array<json, 4> divisors{1, 2, 0.5, 3};
            schema["multipleOf"] = divisors[random_.below(divisors.size())];
        }
        if (add(10)) {
            schema["minLength"] = random_.below(3);
        }
        if (add(10)) {
            schema["maxLength"] = random_.below(3);
        }
        if (add(10)) {
            schema["pattern"] = random_.pick(g_PATTERNS);
        }
        add_array_keywords(schema, depth);
        add_object_keywords(schema, depth);
        if (add(10)) {
            schema["allOf"] = schemas(depth - 1);
        }
        if (add(10)) {
            schema["anyOf"] = schemas(depth - 1);
        }
        if (add(10)) {
            schema["oneOf"] = schemas(depth - 1);
        }
        if (add(6)) {
            schema["not"] = subschema(depth - 1, false);
        }
        if (at_least(Generation::eDraft7) && add(10)) {
            schema["if"] = subschema(depth - 1, false);
            if (random_.chance(70)) {
                schema["then"] = subschema(depth - 1, false);
            }
            if (random_.chance(70)) {
                schema["else"] = subschema(depth - 1, false);
            }
        }
        return schema;
    }

    void add_array_keywords(json& schema, int depth) {
        const auto add = [&](std::size_t percent) { return random_.chance(percent); };
        if (at_least(Generation::e2020_12)) {
            if (add(12)) {
                schema["prefixItems"] = schemas(depth - 1);
            }
            if (add(15)) {
                schema["items"] = subschema(depth - 1, false);
            }
        } else if (add(20)) {
            if (random_.chance(50)) {
                schema["items"] = subschema(depth - 1, false);
            } else {
                schema["items"] = schemas(depth - 1);
                if (random_.chance(50)) {
                    schema["additionalItems"] = subschema(depth - 1, false);
                }
            }
        }
        if (at_least(Generation::eDraft6) && add(8)) {
            schema["contains"] = subschema(depth - 1, false);
            if (at_least(Generation::e2019_09) && random_.chance(40)) {
                schema["minContains"] = random_.below(3);
            }
            if (at_least(Generation::e2019_09) && random_.chance(30)) {
                schema["maxContains"] = 1 + random_.below(2);
            }
        }
        if (add(6)) {
            schema["uniqueItems"] = random_.chance(70);
        }
        if (add(6)) {
            schema["minItems"] = random_.below(3);
        }
        if (add(6)) {
            schema["maxItems"] = random_.below(3);
        }
        if (at_least(Generation::e2019_09) && add(6)) {
            schema["unevaluatedItems"] = subschema(depth - 1, false);
        }
    }

    void add_object_keywords(json& schema, int depth) {
        const auto add = [&](std::size_t percent) { return random_.chance(percent); };
        if (add(25)) {
            json properties = json::object();
            const auto size = 1 + random_.below(2);
            for (std::size_t i = 0; i < size; ++i) {
                properties[random_.pick(g_NAMES)] = subschema(depth - 1, false);
            }
            schema["properties"] = properties;
        }
        if (add(8)) {
            schema["patternProperties"] = {{random_.pick(g_PATTERNS), subschema(depth - 1, false)}};
        }
        if (add(12)) {
            schema["additionalProperties"] = subschema(depth - 1, false);
        }
        if (at_least(Generation::eDraft6) && add(6)) {
            schema["propertyNames"] =
                random_.chance(50) ? json{{"maxLength", 1}} : json{{"pattern", "^[ab]"}};
        }
        if (add(12)) {
            json required = json::array();
            for (const char* name : g_NAMES) {
                if (random_.chance(30)) {
                    required.push_back(name);
                }
            }
            if (!required.empty() || at_least(Generation::eDraft6)) {
                schema["required"] = required;
            }
        }
        if (add(6)) {
            const std::string name = random_.pick(g_NAMES);
            const bool as_schema = random_.chance(50);
            if (at_least(Generation::e2019_09)) {
                if (as_schema) {
                    schema["dependentSchemas"] = {{name, subschema(depth - 1, false)}};
                } else {
                    schema["dependentRequired"] = {{name, {random_.pick(g_NAMES)}}};
                }
            } else {
                schema["dependencies"] = {
                    {name, as_schema ? subschema(depth - 1, false) : json{random_.pick(g_NAMES)}}};
            }
        }
        if (add(6)) {
            schema["minProperties"] = random_.below(3);
        }
        if (add(6)) {
            schema["maxProperties"] = random_.below(3);
        }
        if (at_least(Generation::e2019_09) && add(6)) {
            schema["unevaluatedProperties"] = subschema(depth - 1, false);
        }
    }

    Generation generation_;
    Random random_;
};

void collect_mutation_sites(const json& value, const json::json_pointer& at,
                            std::vector<std::pair<json::json_pointer, std::string>>& sites) {
    if (value.is_object()) {
        for (const auto& [key, member] : value.items()) {
            sites.emplace_back(at, key);
            collect_mutation_sites(member, at / key, sites);
        }
    } else if (value.is_array()) {
        for (std::size_t index = 0; index < value.size(); ++index) {
            collect_mutation_sites(value[index], at / index, sites);
        }
    }
}

const std::array<json, 9>& replacements() {
    static const std::array<json, 9> s_values{json(nullptr), json(true),    json(0),
                                              json(-1),      json(1.5),     json(""),
                                              json("x"),     json::array(), json::object()};
    return s_values;
}

constexpr std::size_t g_MUTATIONS_PER_SITE = 11;

}  // namespace

const std::vector<Dialect>& dialects() {
    static const std::vector<Dialect> s_dialects{
        {"draft2020-12", "https://json-schema.org/draft/2020-12/schema"},
        {"draft2019-09", "https://json-schema.org/draft/2019-09/schema"},
        {"draft7", "http://json-schema.org/draft-07/schema#"},
        {"draft6", "http://json-schema.org/draft-06/schema#"},
        {"draft4", "http://json-schema.org/draft-04/schema#"},
    };
    return s_dialects;
}

std::vector<SuiteFile> suite_files(const Dialect& dialect) {
    const std::filesystem::path directory =
        std::filesystem::path(MCP_TEST_DATA_DIR) / "json-schema" / dialect.directory;
    std::vector<std::filesystem::path> paths;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.path().extension() == ".json") {
            paths.push_back(entry.path());
        }
    }
    std::sort(paths.begin(), paths.end());
    std::vector<SuiteFile> files;
    for (const auto& path : paths) {
        std::ifstream stream(path);
        const json groups = json::parse(stream);
        SuiteFile file{path.filename().string(), {}};
        for (const auto& group : groups) {
            Case item;
            item.schema = group.at("schema");
            if (item.schema.is_object() && !item.schema.contains("$schema")) {
                item.schema["$schema"] = dialect.uri;
            }
            for (const auto& test : group.at("tests")) {
                item.values.push_back(test.at("data"));
            }
            file.groups.push_back(std::move(item));
        }
        files.push_back(std::move(file));
    }
    return files;
}

std::vector<Case> generated_cases(const Dialect& dialect, std::size_t count) {
    const auto generation = generation_of(dialect);
    Generator generator(generation, 0x5EED0000ULL + static_cast<std::uint64_t>(generation));
    std::vector<Case> cases;
    cases.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        Case item;
        item.schema = generator.schema(3);
        if (item.schema.is_object()) {
            item.schema["$schema"] = dialect.uri;
        }
        for (int j = 0; j < 8; ++j) {
            item.values.push_back(generator.value());
        }
        cases.push_back(std::move(item));
    }
    return cases;
}

void for_each_mutant(const json& schema, const std::function<void(const json&)>& visit) {
    std::vector<std::pair<json::json_pointer, std::string>> sites;
    collect_mutation_sites(schema, json::json_pointer(), sites);
    for (const auto& [parent, key] : sites) {
        const auto& original = schema.at(parent).at(key);
        json mutant = schema;
        mutant.at(parent).erase(key);
        visit(mutant);
        for (const auto& replacement : replacements()) {
            mutant = schema;
            mutant.at(parent)[key] = replacement;
            visit(mutant);
        }
        mutant = schema;
        mutant.at(parent)[key] = json::array({original});
        visit(mutant);
    }
}

std::size_t mutant_count(const json& schema) {
    std::vector<std::pair<json::json_pointer, std::string>> sites;
    collect_mutation_sites(schema, json::json_pointer(), sites);
    return sites.size() * g_MUTATIONS_PER_SITE;
}

char compile_verdict(const json& schema) {
    try {
        (void)CompiledSchema::compile(schema);
        return 'o';
    } catch (const SchemaError& error) {
        switch (error.category()) {
            case SchemaError::Category::eInvalidSchema:
                return 'i';
            case SchemaError::Category::eUnsupportedDialect:
                return 'd';
            case SchemaError::Category::eUnresolvedRef:
                return 'r';
            case SchemaError::Category::eUnsupportedPattern:
                return 'p';
        }
        return 'x';
    } catch (const std::exception&) {
        return 'x';
    }
}

std::string case_verdicts(const Case& item) {
    std::string verdicts;
    std::optional<CompiledSchema> compiled;
    try {
        compiled = CompiledSchema::compile(item.schema);
        verdicts.push_back('o');
    } catch (const std::exception&) {
        verdicts.push_back(compile_verdict(item.schema));
        return verdicts;
    }
    for (const auto& value : item.values) {
        verdicts.push_back(compiled->validate(value).valid ? '1' : '0');
    }
    return verdicts;
}

}  // namespace mcp::test::schema_corpus

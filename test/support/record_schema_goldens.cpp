// Records the verdicts of the jsoncons evaluation on the schema corpora (schema_corpus.hpp) into
// test/data/schema-goldens.json, which json_schema_goldens_test.cpp compares every build with.
// Run it only to record the verdicts again, for instance after the corpora change:
//
//   mcp-schema-goldens [output path]
//
// The output path defaults to test/data/schema-goldens.json in the source tree.

#include "../../src/protocol/schema_cache.hpp"
#include "schema_corpus.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <exception>
#include <fstream>
#include <iostream>
#include <string>

namespace {

namespace schema_corpus = mcp::test::schema_corpus;
using nlohmann::json;

/// Generated schemas per dialect.
constexpr std::size_t g_GENERATED_PER_DIALECT = 400;

json record() {
    const mcp::detail::ScopedSchemaEngine jsoncons(mcp::detail::SchemaEngine::eJsoncons);
    json goldens = {
        {"about",
         "Verdicts of the jsoncons evaluation on the corpora of test/support/schema_corpus.hpp, "
         "recorded by mcp-schema-goldens (test/support/record_schema_goldens.cpp). A verdict "
         "string is the compile outcome ('o' compiles; 'i', 'd', 'r', 'p' a SchemaError "
         "category) followed, when the schema compiles, by '1' or '0' per value."},
        {"generated_per_dialect", g_GENERATED_PER_DIALECT},
        {"suite", json::object()},
        {"generated", json::object()},
        {"mutants", json::object()},
    };
    for (const auto& dialect : schema_corpus::dialects()) {
        auto& suite = goldens["suite"][dialect.directory];
        auto& mutants = goldens["mutants"][dialect.directory];
        for (const auto& file : schema_corpus::suite_files(dialect)) {
            json groups = json::array();
            std::string mutant_verdicts;
            for (const auto& group : file.groups) {
                groups.push_back(schema_corpus::case_verdicts(group));
                schema_corpus::for_each_mutant(group.schema, [&](const json& mutant) {
                    mutant_verdicts.push_back(schema_corpus::compile_verdict(mutant));
                });
            }
            suite[file.name] = std::move(groups);
            mutants[file.name] = std::move(mutant_verdicts);
        }
        json generated = json::array();
        for (const auto& item : schema_corpus::generated_cases(dialect, g_GENERATED_PER_DIALECT)) {
            generated.push_back(schema_corpus::case_verdicts(item));
        }
        goldens["generated"][dialect.directory] = std::move(generated);
        std::cerr << "recorded " << dialect.directory << "\n";
    }
    return goldens;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string path =
        argc > 1 ? argv[1] : std::string(MCP_TEST_DATA_DIR) + "/schema-goldens.json";
    try {
        const auto goldens = record();
        std::ofstream out(path, std::ios::binary);
        out << goldens.dump(1) << "\n";
        if (!out) {
            std::cerr << "cannot write " << path << "\n";
            return 1;
        }
    } catch (const std::exception& error) {
        std::cerr << "recording failed: " << error.what() << "\n";
        return 1;
    }
    std::cerr << "wrote " << path << "\n";
    return 0;
}

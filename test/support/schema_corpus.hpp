/**
 * @file schema_corpus.hpp
 * @brief The schemas and values the schema tests compare the SDK's evaluation on, and the
 * verdicts recorded for them.
 *
 * Three corpora, each per dialect: the JSON-Schema-Test-Suite groups (test/data/json-schema),
 * schemas generated from a fixed seed, and mutants of the suite schemas. Every corpus is built the
 * same way on every platform, so verdicts recorded once (test/data/schema-goldens.json) can be
 * compared with the evaluation of any build.
 */
#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace mcp::test::schema_corpus {

/// A dialect the SDK accepts, by its suite directory and its `$schema` URI.
struct Dialect {
    const char* directory;
    const char* uri;
};

/// The five dialects, newest first.
const std::vector<Dialect>& dialects();

/// A schema and the values checked against it.
struct Case {
    nlohmann::json schema;
    std::vector<nlohmann::json> values;
};

/// The suite files of @p dialect, sorted by name: each file's groups in file order. A group's schema
/// carries @p dialect's `$schema` when it has none, as a schema without one is read as 2020-12.
struct SuiteFile {
    std::string name;
    std::vector<Case> groups;
};
std::vector<SuiteFile> suite_files(const Dialect& dialect);

/// @p count schemas over @p dialect's keywords, each with eight values, from a fixed seed.
std::vector<Case> generated_cases(const Dialect& dialect, std::size_t count);

/// Calls @p visit with every mutant of @p schema, in a fixed order. A mutant changes one member of
/// one object in the document: the member is removed, its value replaced by one of nine values
/// (null, true, 0, -1, 1.5, "", "x", [], {}), or wrapped in a one-element array.
void for_each_mutant(const nlohmann::json& schema,
                     const std::function<void(const nlohmann::json&)>& visit);

/// The number of mutants for_each_mutant() visits for @p schema.
std::size_t mutant_count(const nlohmann::json& schema);

/// The outcome of compiling @p schema: 'o' when it compiles, otherwise a letter for the
/// SchemaError category ('i' invalid, 'd' unsupported dialect, 'r' unresolved reference,
/// 'p' unsupported pattern), or 'x' for any other exception.
char compile_verdict(const nlohmann::json& schema);

/// compile_verdict() of the case's schema, then, when it compiles, '1' or '0' for each value by
/// whether it is valid.
std::string case_verdicts(const Case& item);

}  // namespace mcp::test::schema_corpus

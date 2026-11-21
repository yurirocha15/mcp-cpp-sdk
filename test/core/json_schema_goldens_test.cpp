// The SDK's schema evaluation against verdicts recorded from the jsoncons evaluation
// (test/data/schema-goldens.json, written by mcp-schema-goldens). A verdict may differ only where
// test/data/schema-engine-differences.json lists the position with a reason, so a change of verdict
// is always deliberate, also once jsoncons is no longer part of the SDK.

#include <mcp/protocol/schema.hpp>

#include "../support/schema_corpus.hpp"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <cstddef>
#include <fstream>
#include <map>
#include <string>
#include <tuple>

namespace json_schema_goldens_test {

namespace {

namespace schema_corpus = mcp::test::schema_corpus;
using nlohmann::json;

json load(const char* name) {
    std::ifstream stream(std::string(MCP_TEST_DATA_DIR) + "/" + name);
    return json::parse(stream);
}

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define MCP_SCHEMA_GOLDENS_SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) ||                             \
    __has_feature(memory_sanitizer)
#define MCP_SCHEMA_GOLDENS_SANITIZED 1
#endif
#endif

/// Every how many generated schemas and mutants this build checks. Sanitized and unoptimized
/// builds check every fiftieth: all of them take sanitized builds longer than a test may run.
/// Optimized builds check all of them.
constexpr std::size_t stride() {
#if defined(MCP_SCHEMA_GOLDENS_SANITIZED) || !defined(NDEBUG)
    return 50;
#else
    return 1;
#endif
}

/// One listed difference: corpus, dialect, file ("" for generated schemas) and position.
using Position = std::tuple<std::string, std::string, std::string, std::size_t>;

/// The listed differences, each with whether a check reached it.
class Differences {
   public:
    Differences() {
        const auto table = load("schema-engine-differences.json");
        for (const auto& row : table.at("differences")) {
            const Position position{
                row.at("corpus").get<std::string>(), row.at("dialect").get<std::string>(),
                row.at("file").get<std::string>(), row.at("index").get<std::size_t>()};
            EXPECT_FALSE(row.at("reason").get<std::string>().empty());
            EXPECT_TRUE(rows_.emplace(position, false).second) << "listed twice";
        }
    }

    /// Compares @p actual with @p expected at @p position; a difference must be listed. Returns
    /// whether the check passed.
    bool check(const Position& position, const std::string& expected, const std::string& actual) {
        const auto listed = rows_.find(position);
        if (listed != rows_.end()) {
            listed->second = true;
            EXPECT_NE(expected, actual) << describe(position) << ": listed, but the verdicts agree";
            return expected != actual;
        }
        EXPECT_EQ(expected, actual) << describe(position);
        return expected == actual;
    }

    /// Fails for a listed difference of @p corpus that no check reached.
    void expect_all_reached(const std::string& corpus) const {
        for (const auto& [position, reached] : rows_) {
            if (std::get<0>(position) == corpus) {
                EXPECT_TRUE(reached) << describe(position) << ": listed, but no such position";
            }
        }
    }

   private:
    static std::string describe(const Position& position) {
        return std::get<0>(position) + " " + std::get<1>(position) + " " + std::get<2>(position) +
               " #" + std::to_string(std::get<3>(position));
    }

    std::map<Position, bool> rows_;
};

std::string at(const std::string& verdicts, std::size_t index) {
    return index < verdicts.size() ? std::string(1, verdicts[index]) : std::string("?");
}

TEST(SchemaGoldens, SuiteVerdicts) {
    const auto goldens = load("schema-goldens.json");
    Differences differences;
    for (const auto& dialect : schema_corpus::dialects()) {
        const auto& recorded = goldens.at("suite").at(dialect.directory);
        const auto files = schema_corpus::suite_files(dialect);
        ASSERT_EQ(recorded.size(), files.size()) << dialect.directory;
        for (const auto& file : files) {
            const auto& groups = recorded.at(file.name);
            ASSERT_EQ(groups.size(), file.groups.size()) << file.name;
            for (std::size_t index = 0; index < file.groups.size(); ++index) {
                differences.check({"suite", dialect.directory, file.name, index},
                                  groups[index].get<std::string>(),
                                  schema_corpus::case_verdicts(file.groups[index]));
            }
        }
    }
    differences.expect_all_reached("suite");
}

TEST(SchemaGoldens, GeneratedVerdicts) {
    const auto goldens = load("schema-goldens.json");
    const auto count = goldens.at("generated_per_dialect").get<std::size_t>();
    Differences differences;
    for (const auto& dialect : schema_corpus::dialects()) {
        const auto& recorded = goldens.at("generated").at(dialect.directory);
        const auto cases = schema_corpus::generated_cases(dialect, count);
        ASSERT_EQ(recorded.size(), cases.size()) << dialect.directory;
        for (std::size_t index = 0; index < cases.size(); index += stride()) {
            differences.check({"generated", dialect.directory, "", index},
                              recorded[index].get<std::string>(),
                              schema_corpus::case_verdicts(cases[index]));
        }
    }
    if (stride() == 1) {
        differences.expect_all_reached("generated");
    }
}

// Every mutant compiles or is refused with a SchemaError, as jsoncons decided: never another
// exception or a crash.
TEST(SchemaGoldens, MutantVerdicts) {
    const auto goldens = load("schema-goldens.json");
    Differences differences;
    for (const auto& dialect : schema_corpus::dialects()) {
        const auto& recorded = goldens.at("mutants").at(dialect.directory);
        for (const auto& file : schema_corpus::suite_files(dialect)) {
            const auto expected = recorded.at(file.name).get<std::string>();
            std::size_t count = 0;
            for (const auto& group : file.groups) {
                count += schema_corpus::mutant_count(group.schema);
            }
            ASSERT_EQ(expected.size(), count) << dialect.directory << " " << file.name;
            std::size_t index = 0;
            for (const auto& group : file.groups) {
                schema_corpus::for_each_mutant(group.schema, [&](const json& mutant) {
                    if (index % stride() == 0) {
                        const char verdict = schema_corpus::compile_verdict(mutant);
                        EXPECT_NE(verdict, 'x') << dialect.directory << " " << file.name << " #"
                                                << index << ": " << mutant.dump();
                        if (!differences.check({"mutants", dialect.directory, file.name, index},
                                               at(expected, index), std::string(1, verdict))) {
                            ADD_FAILURE() << "mutant: " << mutant.dump();
                        }
                    }
                    ++index;
                });
            }
        }
    }
    if (stride() == 1) {
        differences.expect_all_reached("mutants");
    }
}

}  // namespace

}  // namespace json_schema_goldens_test

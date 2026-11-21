#pragma once

#include <mcp/protocol/schema.hpp>

#include <nlohmann/json.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <string_view>

namespace mcp::detail {

/**
 * @brief A JSON Schema 2020-12 document, evaluated directly on nlohmann::json.
 *
 * CompiledSchema evaluates a schema this way when it can and with jsoncons otherwise. jsoncons
 * copies its evaluation path for every keyword it evaluates, which makes checking typical tool
 * arguments several times slower than walking them once.
 *
 * Implemented: every 2020-12 assertion and applicator keyword, `unevaluatedProperties` and
 * `unevaluatedItems`, and the reference keywords `$ref`, `$dynamicRef`, `$defs`, `$id`, `$anchor`
 * and `$dynamicAnchor`. `additionalItems` and `dependencies`, which jsoncons also implements for
 * 2020-12, leave the schema to jsoncons. The schemas evaluated in place at a value's location
 * record which of its members and items they evaluated only when an `unevaluated*` keyword reads
 * it. Annotations and keywords jsoncons ignores are ignored too (`format`, `examples`,
 * `contentEncoding`, `contentMediaType`, `$comment`, `x-...`). `$schema` may appear only at the
 * root of a document and only naming 2020-12.
 *
 * A reference resolves within the document or to a registered document, compiled when a reference
 * first reaches it; a reference to anything else, a metaschema included, leaves the schema to
 * jsoncons. Each location of a document compiles once, however many references reach it, and
 * references are resolved after the walk, so a chain of references does not deepen compilation.
 *
 * Every subschema evaluation is one frame, charged one unit of work; frames nest at most
 * constants::schema::g_MAX_EVAL_DEPTH deep, which also stops a reference cycle. An `anyOf` or
 * `oneOf` branch is evaluated once, keeping a few of its violations to report when no branch
 * matches; `not`, `if` and `contains` keep none.
 */
class NativeSchema {
   public:
    /// @p schema as a NativeSchema, or nothing when it uses a keyword or dialect this does not
    /// implement, holds a value of the wrong shape or has a reference it cannot resolve within
    /// itself and @p documents. Never throws for a malformed schema.
    static std::optional<NativeSchema> compile(const nlohmann::json& schema,
                                               const SchemaDocuments& documents);

    /// Whether the document uses `$ref` or `$dynamicRef`.
    [[nodiscard]] bool has_references() const;

    /// Records in @p result how @p instance fails the schema, stopping after @p max_violations.
    /// Pattern matching may take @p pattern_steps steps (see Regex::search) and the evaluation
    /// @p work units; a validation that needs more, or nests deeper than the frame bound, reports
    /// the value invalid, as jsoncons does when it reaches a limit.
    void validate(const nlohmann::json& instance, std::size_t max_violations, std::size_t pattern_steps,
                  std::size_t work, ValidationResult& result) const;

    /// Why evaluating @p instance stops at a bound with these allowances, or nothing when it does
    /// not stop.
    [[nodiscard]] std::optional<std::string_view> stop_reason(const nlohmann::json& instance,
                                                              std::size_t pattern_steps,
                                                              std::size_t work) const;

    struct Node;
    struct Resource;
    struct Document;

   private:
    explicit NativeSchema(std::shared_ptr<const Document> document);

    std::optional<std::string_view> evaluate(const nlohmann::json& instance, std::size_t max_violations,
                                             std::size_t pattern_steps, std::size_t work,
                                             ValidationResult& result) const;

    std::shared_ptr<const Document> document_;
};

}  // namespace mcp::detail

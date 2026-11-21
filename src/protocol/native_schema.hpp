#pragma once

#include <mcp/protocol/schema.hpp>

#include <nlohmann/json.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <string_view>

namespace mcp::detail {

/**
 * @brief A JSON Schema document of any supported dialect, evaluated directly on nlohmann::json.
 *
 * Implemented: every assertion and applicator keyword of draft-04, draft-06, draft-07, 2019-09 and
 * 2020-12, `unevaluatedProperties` and `unevaluatedItems`, and the reference keywords `$ref`,
 * `$dynamicRef`, `$recursiveRef`, `$defs`, `definitions`, `$id` (`id` in draft-04), `$anchor`,
 * `$dynamicAnchor` and `$recursiveAnchor`. `format`, `examples`, `$comment` and unknown keywords
 * are annotations. `contentEncoding` and `contentMediaType` are asserted in draft-07 and earlier
 * only. In draft-07 and earlier a schema with `$ref` is that reference alone. `$schema` changes the
 * dialect at the root of a schema resource.
 *
 * A reference resolves within the document, to a registered document, compiled when a reference
 * first reaches it, or to the whole metaschema of a supported dialect, which check_schema()
 * stands in for. Nothing else is ever looked for. Each location of a document compiles once,
 * however many references reach it, and references are resolved after the walk, so a chain of
 * references does not deepen compilation.
 *
 * Every subschema evaluation is one frame, charged one unit of work more than the frames it is
 * nested in; frames nest at most constants::schema::g_MAX_EVAL_DEPTH deep, which also stops a
 * reference cycle. An `anyOf` or `oneOf` branch is evaluated once, keeping a few of its violations
 * to report when no branch matches; `not`, `if` and `contains` keep none. The schemas evaluated in
 * place at a value's location record which of its members and items they evaluated only when an
 * `unevaluated*` keyword reads it.
 */
class NativeSchema {
   public:
    /// @p schema, already checked against its dialect's rules, as a NativeSchema. Patterns are
    /// compiled with PatternBudget::compile(), so a budget the caller holds collects the ones that
    /// cannot be compiled.
    /// @throws SchemaError when a `$schema` names an unsupported dialect, a reference cannot be
    /// resolved within @p schema and @p documents, or the schema cannot be compiled otherwise.
    static NativeSchema compile(const nlohmann::json& schema, const SchemaDocuments& documents);

    /// Whether the document uses `$ref` or `$dynamicRef`.
    [[nodiscard]] bool has_references() const;

    /// Records in @p result how @p instance fails the schema, stopping after @p max_violations.
    /// Pattern matching may take @p pattern_steps steps (see Regex::search) and the evaluation
    /// @p work units; a validation that needs more, or nests deeper than the frame bound, reports
    /// the value invalid.
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

#pragma once

#include <mcp/protocol/schema.hpp>

#include <nlohmann/json.hpp>

#include <cstddef>
#include <memory>
#include <optional>

namespace mcp::detail {

/**
 * @brief A JSON Schema 2020-12 document without references, evaluated directly on nlohmann::json.
 *
 * CompiledSchema evaluates a schema this way when it can and with jsoncons otherwise. jsoncons
 * copies its evaluation path for every keyword it evaluates, which makes checking typical tool
 * arguments several times slower than walking them once.
 *
 * Implemented: every 2020-12 assertion and applicator keyword except the reference keywords
 * (`$ref`, `$defs`, `$anchor`, `$dynamicRef`, `$dynamicAnchor`) and `unevaluatedProperties` and
 * `unevaluatedItems`, which leave the schema to jsoncons, as do `additionalItems` and
 * `dependencies`. Annotations and keywords jsoncons ignores are ignored too (`format`, `examples`,
 * `contentEncoding`, `contentMediaType`, `$comment`, `x-...`). `$schema` may appear only at the
 * root and only naming 2020-12.
 *
 * Every subschema evaluation is one frame, charged one unit of work; frames nest at most
 * constants::schema::g_MAX_EVAL_DEPTH deep. An `anyOf` or `oneOf` branch is evaluated once, keeping
 * a few of its violations to report when no branch matches; `not`, `if` and `contains` keep none.
 */
class NativeSchema {
   public:
    /// @p schema as a NativeSchema, or nothing when it uses a keyword or dialect this does not
    /// implement or holds a value of the wrong shape. Never throws for a malformed schema.
    static std::optional<NativeSchema> compile(const nlohmann::json& schema);

    /// Records in @p result how @p instance fails the schema, stopping after @p max_violations.
    /// Pattern matching may take @p pattern_steps steps (see Regex::search) and the evaluation
    /// @p work units; a validation that needs more, or nests deeper than the frame bound, reports
    /// the value invalid, as jsoncons does when it reaches a limit.
    void validate(const nlohmann::json& instance, std::size_t max_violations, std::size_t pattern_steps,
                  std::size_t work, ValidationResult& result) const;

    struct Node;

   private:
    explicit NativeSchema(std::shared_ptr<const Node> root);

    std::shared_ptr<const Node> root_;
};

}  // namespace mcp::detail

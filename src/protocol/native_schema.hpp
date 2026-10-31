#pragma once

#include <mcp/protocol/schema.hpp>

#include <nlohmann/json.hpp>

#include <cstddef>
#include <memory>
#include <optional>

namespace mcp::detail {

/**
 * @brief A JSON Schema 2020-12 document that uses only the keywords tool schemas use most,
 * evaluated directly on nlohmann::json.
 *
 * CompiledSchema evaluates a schema this way when it can and with jsoncons otherwise. jsoncons
 * copies its evaluation path for every keyword it evaluates, which makes checking typical tool
 * arguments several times slower than walking them once.
 *
 * Supported: `type`, `enum`, `const`, `minimum`, `maximum`, `exclusiveMinimum`,
 * `exclusiveMaximum`, `minLength`, `maxLength`, `pattern`, `items` (a schema), `minItems`,
 * `maxItems`, `properties`, `required`, `additionalProperties`, `minProperties`, `maxProperties`,
 * the boolean schemas, the annotations `title`, `description`, `default`, `readOnly`,
 * `writeOnly` and `$id`, and keywords jsoncons does not know, which it ignores too (`format`,
 * `examples`, `$comment`, `x-...`). `$schema` may appear only at the root and only naming
 * 2020-12. Any other keyword jsoncons implements (`$ref`, `anyOf`, `patternProperties`, ...)
 * leaves the schema to jsoncons.
 *
 * Evaluation recurses only where the schema descends, so the schema's nesting bounds it; no
 * evaluation limits are needed.
 */
class NativeSchema {
   public:
    /// @p schema as a NativeSchema, or nothing when it uses a keyword or dialect this does not
    /// implement. @p schema must already be valid under its metaschema.
    static std::optional<NativeSchema> compile(const nlohmann::json& schema);

    /// Records in @p result how @p instance fails the schema, stopping after @p max_violations.
    /// Pattern matching may take @p pattern_steps steps (see Regex::search); a validation that
    /// needs more reports the value invalid, as jsoncons does when it reaches a limit.
    void validate(const nlohmann::json& instance, std::size_t max_violations, std::size_t pattern_steps,
                  ValidationResult& result) const;

    struct Node;

   private:
    explicit NativeSchema(std::shared_ptr<const Node> root);

    std::shared_ptr<const Node> root_;
};

}  // namespace mcp::detail

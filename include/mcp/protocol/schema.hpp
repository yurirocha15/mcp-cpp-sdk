#pragma once

#include <mcp/core/export.hpp>

#include <nlohmann/json.hpp>

#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace mcp {

/**
 * @brief Limits applied when compiling a JSON Schema and validating a value against it.
 *
 * The defaults accept every schema and value a tool realistically uses; they exist so that a schema
 * or value from a peer cannot make compilation or validation exhaust the stack.
 */
struct SchemaOptions {
    /// Deepest nesting of objects and arrays accepted in a value checked against a schema.
    /// Validation recurses with the value wherever the schema descends into it, so this bounds the
    /// stack a validation can use.
    std::size_t max_instance_depth = 64;
    /// Most violations reported by one `validate()` call; validation stops after this many.
    std::size_t max_reported_violations = 8;
    /// Pattern matching steps one `validate()` call may take, before the allowance for the value's
    /// text below. A step takes a few nanoseconds.
    std::size_t base_pattern_steps = 16'000'000;
    /// Further pattern matching steps allowed for every byte of string and property name in the
    /// value. Typical patterns take a handful of steps per byte.
    std::size_t pattern_steps_per_text_byte = 64;
};

/**
 * @brief Thrown when a JSON Schema cannot be compiled.
 *
 * `what()` names the problem; text taken from the schema is flattened and bounded first.
 */
class MCP_API SchemaError : public std::invalid_argument {
   public:
    /// Why the schema was refused.
    enum class Category {
        eInvalidSchema,       ///< The schema is not valid under its dialect's metaschema, holds
                              ///< NaN or Infinity, or evaluating it exceeds the evaluation limits
                              ///< on any value; or a tool input schema holds an invalid
                              ///< `x-mcp-header` annotation.
        eUnsupportedDialect,  ///< `$schema` names a dialect this SDK does not implement.
        eUnresolvedRef,       ///< A `$ref` target is neither in the document nor a supported
                              ///< dialect's metaschema. Nothing is fetched.
    };

    /// Construct with the reason @p category and the description @p message.
    SchemaError(Category category, const std::string& message);

    /// Why the schema was refused.
    [[nodiscard]] Category category() const noexcept { return category_; }

   private:
    Category category_;
};

/**
 * @brief One way a value fails a schema.
 */
struct SchemaViolation {
    std::string instance_path;  ///< JSON Pointer to the failing part of the value ("" is the root).
    std::string message;        ///< What failed, with value text flattened and bounded.
};

/**
 * @brief Outcome of validating a value against a compiled schema.
 */
struct ValidationResult {
    bool valid = true;                        ///< Whether the value conforms.
    std::vector<SchemaViolation> violations;  ///< Up to `SchemaOptions::max_reported_violations`.
};

struct CompiledSchemaImpl;

/**
 * @brief A JSON Schema compiled for repeated validation.
 *
 * A schema without `$schema` is read as JSON Schema 2020-12, the default the MCP specification
 * sets. `$schema` may name 2020-12, 2019-09, draft-07, draft-06 or draft-04; any other value is
 * refused with `SchemaError::Category::eUnsupportedDialect`. The schema itself is checked against
 * its dialect's metaschema. `pattern` and `patternProperties` use `mcp::detail::Regex`, which
 * matches in linear time.
 *
 * References resolve within the document only: JSON Pointer fragments, anchors, and subschemas
 * identified by `$id`, plus the metaschemas of the supported dialects. A `$ref` to anything else
 * (an `http:` or `file:` URI, a relative file name, a location missing from the document) is
 * refused with `SchemaError::Category::eUnresolvedRef`; no network or file access is ever
 * attempted.
 *
 * Validation is bounded: the value's nesting (`SchemaOptions::max_instance_depth`), the length
 * of the evaluation path through the schema (192 keywords and array indexes), and the evaluation
 * work, which may grow with the size and nesting of the value but not exponentially. Pattern
 * matching (`pattern`, `patternProperties`) has its own budget, by default 16 million matcher
 * steps plus 64 for every byte of string and property name in the value (`SchemaOptions`). A
 * value whose validation exceeds a bound is reported as invalid. The evaluation path grows by several
 * keywords per level of the value when a recursive schema passes through `anyOf` and `$ref` at every
 * level; such a schema reaches the bound at about 48 levels. The same bounds apply when the schema
 * is checked against its metaschema, which a 2020-12 schema nesting `properties` 27 levels deep
 * exceeds. A schema that exceeds a bound even for a trivial value, such as `{"$ref": "#"}`, is
 * refused when it is compiled.
 *
 * Violation messages are written for people and their wording may change; `valid` and
 * `instance_path` are the stable parts of a result.
 *
 * A compiled schema is immutable: copies share it, and `validate()` may be called from several
 * threads at once.
 */
class MCP_API CompiledSchema {
   public:
    /**
     * @brief Compile @p schema (an object or a boolean).
     * @throws SchemaError when the schema cannot be compiled.
     */
    static CompiledSchema compile(const nlohmann::json& schema, const SchemaOptions& options = {});

    /// Validate @p instance against the schema.
    [[nodiscard]] ValidationResult validate(const nlohmann::json& instance) const;

    /// The schema this was compiled from.
    [[nodiscard]] const nlohmann::json& schema() const noexcept;

   private:
    explicit CompiledSchema(std::shared_ptr<const CompiledSchemaImpl> impl);

    std::shared_ptr<const CompiledSchemaImpl> impl_;
};

}  // namespace mcp

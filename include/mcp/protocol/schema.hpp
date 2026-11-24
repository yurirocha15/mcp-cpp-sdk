#pragma once

#include <mcp/core/export.hpp>

#include <nlohmann/json.hpp>

#include <cstddef>
#include <map>
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
    /// Most evaluation work one `validate()` call may do, and checking a schema against its
    /// dialect's rules when it is compiled. The work allowed grows with the size of the value times the
    /// size of the schema, up to this; a validation that needs more is stopped and reported as a
    /// violation.
    std::size_t max_evaluation_work = 256'000'000;
    /// Most instructions, in all, that the `pattern` and `patternProperties` regular expressions of
    /// one schema may compile to. An instruction takes about 12 bytes.
    std::size_t max_pattern_instructions = 1'000'000;
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
                              ///< NaN or Infinity, has a pattern that is not a regular
                              ///< expression, or evaluating it exceeds the evaluation limits on
                              ///< any value; or a tool input schema holds an invalid
                              ///< `x-mcp-header` annotation.
        eUnsupportedDialect,  ///< `$schema` names a dialect this SDK does not implement.
        eUnresolvedRef,       ///< A `$ref` target is neither in the document, nor in a given
                              ///< document, nor a supported dialect's whole metaschema.
                              ///< Nothing is fetched.
        eUnsupportedPattern,  ///< A `pattern` or `patternProperties` key is one the linear-time
                              ///< matcher cannot run, such as a lookahead, or the patterns exceed
                              ///< its size limits. Patterns are checked last, so the schema has no
                              ///< other problem.
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
 * @brief Schema documents the references of a schema may reach besides the schema itself, keyed
 * by absolute URI without a fragment, such as `https://example.com/address.json`.
 *
 * A reference whose target, resolved against the referring schema's base URI, is one of these
 * URIs reads that document; nothing is fetched. At most 64 documents may be given, and each is
 * checked as a schema of its own.
 */
using SchemaDocuments = std::map<std::string, nlohmann::json>;

/**
 * @brief A JSON Schema compiled for repeated validation.
 *
 * A schema without `$schema` is read as JSON Schema 2020-12, the default the MCP specification
 * sets. `$schema` may name 2020-12, 2019-09, draft-07, draft-06 or draft-04; any other value is
 * refused with `SchemaError::Category::eUnsupportedDialect`. The schema itself is checked against
 * the rules its dialect's published metaschema states. `pattern` and `patternProperties` use
 * the SDK's own regular expression engine, which matches in linear time.
 *
 * References resolve within the document and within the documents given as `SchemaDocuments`:
 * JSON Pointer fragments, anchors, and subschemas identified by `$id`. A reference to the whole
 * metaschema of a supported dialect checks the value as a schema of that dialect. A `$ref` to
 * anything else (an `http:` or `file:` URI that was not given, a relative file name, a location
 * missing from the document, a location inside a metaschema) is refused with
 * `SchemaError::Category::eUnresolvedRef`; no network or file access is ever attempted.
 *
 * Validation is bounded: the value's nesting (`SchemaOptions::max_instance_depth`), the nesting
 * of the evaluation (192 subschemas evaluated one inside another, `$ref` targets included), and the
 * evaluation work, which may grow with the size and nesting of the value but not exponentially.
 * Pattern matching (`pattern`, `patternProperties`) has its own budget, by default 16 million
 * matcher steps plus 64 for every byte of string and property name in the value (`SchemaOptions`).
 * A value whose validation exceeds a bound is reported as invalid. A recursive schema takes one
 * level of nesting for each subschema on its way down one level of the value: one that passes
 * through `anyOf`, `items`, `allOf` and `$ref` at every level reaches the bound at 48 levels of the
 * value. A schema document may nest 64 levels, and is checked and compiled within that depth. A
 * schema that exceeds a bound even for a trivial value, such as `{"$ref": "#"}`, is refused when it
 * is compiled.
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
     * @brief Compile @p schema (an object or a boolean), whose references may also reach
     * @p documents.
     * @throws SchemaError when the schema or one of @p documents cannot be compiled, or when more
     * than 64 documents or a key that is not an absolute URI without a fragment is given.
     */
    static CompiledSchema compile(
        const nlohmann::json& schema,
        const SchemaOptions& options = {} const SchemaDocuments& documents = {});

    /// Validate @p instance against the schema.
    [[nodiscard]] ValidationResult validate(const nlohmann::json& instance) const;

    /// The schema this was compiled from.
    [[nodiscard]] const nlohmann::json& schema() const noexcept;

   private:
    explicit CompiledSchema(std::shared_ptr<const CompiledSchemaImpl> impl);

    std::shared_ptr<const CompiledSchemaImpl> impl_;
};

}  // namespace mcp

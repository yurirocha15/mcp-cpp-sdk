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
        eInvalidSchema,       ///< The schema is not valid under its dialect's metaschema.
        eUnsupportedDialect,  ///< `$schema` names a dialect this SDK does not implement.
    };

    SchemaError(Category category, const std::string& message);

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

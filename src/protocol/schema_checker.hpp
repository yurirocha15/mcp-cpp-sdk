#pragma once

#include "schema_dialect.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <optional>
#include <string>

namespace mcp::detail {

/// Where a schema breaks its dialect's rules, and how.
struct SchemaProblem {
    std::string path;  ///< JSON Pointer into the schema.
    std::string message;
};

/**
 * @brief The first place where @p schema breaks the rules its dialect's metaschema states, or
 * nothing.
 *
 * Checks what the metaschema of @p dialect checks, keyword by keyword, without evaluating the
 * metaschema: the shape of every keyword the dialect defines, at every place the dialect reads a
 * subschema. Keywords the dialect does not define are left alone, and `format` is not asserted,
 * as the metaschemas leave it. Subschemas are checked against the same dialect, as a metaschema
 * checks them, whatever `$schema` they declare.
 *
 * Each value visited costs one unit of @p work, and comparing values for uniqueness costs what
 * json_equal() charges. Throws WorkBudgetExceeded when @p work runs out, and EvaluationTooDeep
 * when subschemas nest more than @p max_depth levels.
 */
std::optional<SchemaProblem> check_schema(const nlohmann::json& schema, SchemaDialect dialect,
                                          std::size_t& work, std::size_t max_depth);

}  // namespace mcp::detail

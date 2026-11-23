#pragma once

#include <mcp/core/export.hpp>

#include <initializer_list>
#include <nlohmann/json.hpp>
#include <string>

namespace mcp {

/// @brief One argument of a tool, for tool_params().
struct ParamSpec {
    std::string name;
    /// The JSON Schema type: "string", "number", "integer", "boolean", "array" or "object".
    std::string type;
    std::string description;
    bool required = true;
};

/**
 * @brief The input schema of a tool whose arguments are @p params, such as
 *        `mcp::tool_params({{"name", "string", "Who to greet"}})`.
 *
 * @return An object schema with a property of the given type and description per argument, and
 *         the required arguments listed under `required`. Any other schema is written as JSON.
 * @throws std::invalid_argument For an argument without a name, a name given twice, or a type
 *         other than those ParamSpec::type lists.
 */
MCP_API nlohmann::json tool_params(std::initializer_list<ParamSpec> params);

}  // namespace mcp

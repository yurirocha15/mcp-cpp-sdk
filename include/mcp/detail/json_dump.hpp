#pragma once

#include <mcp/core/export.hpp>

#include <nlohmann/json.hpp>

#include <string>

namespace mcp::detail {

/// Appends the compact text of @p value to @p out. The text is byte for byte what
/// `value.dump()` returns, and a string that is not valid UTF-8 throws the same
/// `nlohmann::json::type_error` that `dump()` throws, leaving @p out unspecified.
MCP_API void append_json(std::string& out, const nlohmann::json& value);

/// The compact text of @p value: the same bytes as `value.dump()`, written faster.
MCP_API std::string dump_json(const nlohmann::json& value);

}  // namespace mcp::detail

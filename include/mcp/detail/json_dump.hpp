#pragma once

#include <mcp/core/export.hpp>

#include <nlohmann/json.hpp>

#include <span>
#include <string>
#include <string_view>

namespace mcp::detail {

/// Appends the compact text of @p value to @p out. The text is byte for byte what
/// `value.dump()` returns, and a string that is not valid UTF-8 throws the same
/// `nlohmann::json::type_error` that `dump()` throws, leaving @p out unspecified.
MCP_API void append_json(std::string& out, const nlohmann::json& value);

/// A member written into an object that does not hold it: its key and the JSON text of its value.
struct JsonMemberText {
    std::string_view key;
    std::string_view value_text;
};

/// Appends what `dump()` writes for @p object, which must be a JSON object, with the members of
/// @p extra added, without copying @p object. @p extra is sorted by key; a key @p object already
/// holds keeps the value it has there, as `emplace()` would. Each value text is written as it is.
MCP_API void append_json_object_with(std::string& out, const nlohmann::json& object,
                                     std::span<const JsonMemberText> extra);

/// The compact text of @p value: the same bytes as `value.dump()`, written faster.
MCP_API std::string dump_json(const nlohmann::json& value);

}  // namespace mcp::detail

#pragma once

#include <mcp/core/export.hpp>

#include <nlohmann/json.hpp>

#include <cstddef>
#include <string_view>

namespace mcp::detail {

/// The deepest nesting of arrays and objects parse_json_fast() reads itself, one recursion per
/// level; a document nested this deep parses on a thread with a 256 KiB stack, also when built
/// with AddressSanitizer. Deeper text is left to nlohmann::json, whose parser keeps its own stack.
inline constexpr std::size_t g_MAX_FAST_PARSE_DEPTH = 128;

/// Parses @p text, a whole JSON document, into @p out. The value is the one
/// `nlohmann::json::parse(text)` returns. Returns false, leaving @p out unspecified, for text
/// nlohmann::json would reject and also for some it accepts: a byte order mark, text after a NUL
/// byte that ends the document, a number that is out of range for its type or that would be read
/// in a locale whose decimal point is not `.`, and nesting deeper than g_MAX_FAST_PARSE_DEPTH.
/// A caller that needs nlohmann's answer for those parses again with nlohmann::json.
MCP_API bool parse_json_fast(std::string_view text, nlohmann::json& out);

}  // namespace mcp::detail

#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>
#include <string_view>

namespace mcp::detail {

/// Deepest nesting of arrays and objects accepted in a document received from a peer.
///
/// nlohmann::json parses iteratively, but copying a parsed value recurses once per level, and the
/// SDK copies request params and results. Without a bound, a message of a few hundred kilobytes
/// made of nothing but brackets exhausts the stack of whichever thread copies it. Real MCP payloads
/// stay far below this.
inline constexpr std::size_t g_MAX_PEER_JSON_DEPTH = 128;

/// Whether `text` nests arrays and objects deeper than `max_depth`. Brackets inside strings are not
/// counted. The result is only meaningful for valid JSON, which is all the parser accepts anyway.
inline bool exceeds_json_depth(std::string_view text, std::size_t max_depth = g_MAX_PEER_JSON_DEPTH) {
    std::size_t depth = 0;
    bool in_string = false;
    bool escaped = false;
    for (const char character : text) {
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (character == '\\') {
                escaped = true;
            } else if (character == '"') {
                in_string = false;
            }
            continue;
        }
        switch (character) {
            case '"':
                in_string = true;
                break;
            case '[':
            case '{':
                if (++depth > max_depth) {
                    return true;
                }
                break;
            case ']':
            case '}':
                if (depth > 0) {
                    --depth;
                }
                break;
            default:
                break;
        }
    }
    return false;
}

/// Parse a document received from a peer without throwing. The result is discarded when the text
/// is not valid JSON or nests deeper than g_MAX_PEER_JSON_DEPTH.
inline nlohmann::json parse_peer_json(std::string_view text) {
    if (exceeds_json_depth(text)) {
        return nlohmann::json(nlohmann::json::value_t::discarded);
    }
    return nlohmann::json::parse(text, nullptr, false);
}

}  // namespace mcp::detail

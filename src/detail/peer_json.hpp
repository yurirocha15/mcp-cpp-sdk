#pragma once

#include <mcp/detail/json_parse.hpp>

#include <nlohmann/json.hpp>

#include <cstddef>
#include <string>
#include <string_view>

namespace mcp::detail {

/// Deepest nesting of arrays and objects accepted in a document received from a peer.
///
/// nlohmann::json parses iteratively, but copying a parsed value recurses once per level, and the
/// SDK copies request params and results. Without a bound, a message of a few hundred kilobytes
/// made of nothing but brackets exhausts the stack of whichever thread copies it. Real MCP payloads
/// stay far below this.
inline constexpr std::size_t g_MAX_PEER_JSON_DEPTH = 128;
static_assert(g_MAX_FAST_PARSE_DEPTH >= g_MAX_PEER_JSON_DEPTH,
              "every document parse_peer_json() accepts is within the fast parser's nesting");

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
    nlohmann::json value;
    if (parse_json_fast(text, value)) {
        return value;
    }
    // Invalid text, and the few valid inputs the fast parser leaves to nlohmann::json.
    return nlohmann::json::parse(text, nullptr, false);
}

/// About how many bytes @p value takes in memory once parsed. The limits on work a peer can start
/// charge this rather than the length of the text, since a parsed container is far larger than its
/// text: `{}` is two bytes of text and about eighty in memory. Recursion is bounded by
/// g_MAX_PEER_JSON_DEPTH for anything parse_peer_json() returned.
inline std::size_t json_memory_cost(const nlohmann::json& value) {
    // A value on the heap: the allocation itself and the allocator's own bookkeeping.
    constexpr std::size_t heap_block = 64;
    // A member of an object: a tree node holding the key and the value.
    constexpr std::size_t member_node = 96;
    std::size_t cost = sizeof(nlohmann::json);
    switch (value.type()) {
        case nlohmann::json::value_t::object:
            cost += heap_block;
            for (auto member = value.begin(); member != value.end(); ++member) {
                cost += member_node + member.key().size() + json_memory_cost(member.value());
            }
            break;
        case nlohmann::json::value_t::array:
            cost += heap_block;
            for (const auto& element : value) {
                cost += json_memory_cost(element);
            }
            break;
        case nlohmann::json::value_t::string:
            cost += heap_block + value.get_ref<const std::string&>().size();
            break;
        case nlohmann::json::value_t::binary:
            cost += heap_block + value.get_binary().size();
            break;
        default:
            break;
    }
    return cost;
}

}  // namespace mcp::detail

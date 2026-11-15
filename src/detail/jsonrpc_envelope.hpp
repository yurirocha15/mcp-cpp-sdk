#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string_view>

namespace mcp::detail {

/// The top-level members of a JSON-RPC message that response routing reads.
struct JsonRpcEnvelope {
    /// Raw text of the `id` value, such as `7` or `"req-1"`; empty when the member is absent.
    std::string_view id;
    bool has_result_or_error{false};
    /// Whether the message has a `result` member, which only a successful response has.
    bool has_result{false};
};

inline std::size_t skip_json_whitespace(std::string_view text, std::size_t pos) {
    while (pos < text.size() &&
           (text[pos] == ' ' || text[pos] == '\t' || text[pos] == '\n' || text[pos] == '\r')) {
        ++pos;
    }
    return pos;
}

/// The index of the first quote or backslash among the eight bytes of @p word, loaded from memory
/// on a little-endian machine, or 8 when there is none.
inline std::size_t first_quote_or_backslash(std::uint64_t word) {
    constexpr std::uint64_t g_ones = 0x0101010101010101ULL;
    constexpr std::uint64_t g_highs = 0x8080808080808080ULL;
    constexpr int g_bits_per_byte = 8;
    // Flags the zero bytes of @p value. The lowest flag is exact; a borrow can set false flags
    // only above it, and taking the lowest flag of both masks never reads those.
    const auto zero_bytes = [](std::uint64_t value) { return (value - g_ones) & ~value & g_highs; };
    const auto flags = zero_bytes(word ^ (g_ones * '"')) | zero_bytes(word ^ (g_ones * '\\'));
    return flags == 0 ? sizeof(word)
                      : static_cast<std::size_t>(std::countr_zero(flags) / g_bits_per_byte);
}

/// @return The position after the string that opens at @p pos, or npos when it is unterminated.
inline std::size_t skip_json_string(std::string_view text, std::size_t pos) {
    // The text values of tool results run to kilobytes, mostly escaped JSON, so the scan jumps
    // eight bytes at a time to the next quote or backslash instead of testing every byte.
    const auto size = text.size();
    ++pos;
    while (pos < size) {
        if constexpr (std::endian::native == std::endian::little) {
            if (pos + sizeof(std::uint64_t) <= size) {
                std::uint64_t word = 0;
                std::memcpy(&word, text.data() + pos, sizeof(word));
                const auto offset = first_quote_or_backslash(word);
                pos += offset;
                if (offset == sizeof(word)) {
                    continue;
                }
            }
        }
        if (text[pos] == '\\') {
            pos += 2;
        } else if (text[pos] == '"') {
            return pos + 1;
        } else {
            ++pos;
        }
    }
    return std::string_view::npos;
}

/// @return The position after the value that starts at @p pos, or npos when it is unterminated.
///         A scalar ends at the next delimiter; its own syntax is not checked.
inline std::size_t skip_json_value(std::string_view text, std::size_t pos) {
    if (pos >= text.size()) {
        return std::string_view::npos;
    }
    if (text[pos] == '"') {
        return skip_json_string(text, pos);
    }
    if (text[pos] != '{' && text[pos] != '[') {
        const auto end = text.find_first_of(",}] \t\r\n", pos);
        return end == std::string_view::npos ? text.size() : end;
    }
    std::size_t depth = 0;
    while (pos < text.size()) {
        const char current = text[pos];
        if (current == '"') {
            pos = skip_json_string(text, pos);
            if (pos == std::string_view::npos) {
                return pos;
            }
            continue;
        }
        if (current == '{' || current == '[') {
            ++depth;
        } else if ((current == '}' || current == ']') && --depth == 0) {
            return pos + 1;
        }
        ++pos;
    }
    return std::string_view::npos;
}

/// Find a message's id and whether it carries a result or an error, without building its JSON tree.
///
/// Meant for messages this SDK serialized itself, which are valid JSON: the scan delimits values
/// but does not validate them. A later duplicate member wins, as it does in nlohmann::json.
///
/// @return std::nullopt when @p text is not an object the scan can delimit, or when a member name
///         contains an escape; the caller then parses the message in full.
inline std::optional<JsonRpcEnvelope> scan_jsonrpc_envelope(std::string_view text) {
    auto pos = skip_json_whitespace(text, 0);
    if (pos >= text.size() || text[pos] != '{') {
        return std::nullopt;
    }
    JsonRpcEnvelope envelope;
    pos = skip_json_whitespace(text, pos + 1);
    if (pos < text.size() && text[pos] == '}') {
        return envelope;
    }
    for (;;) {
        if (pos >= text.size() || text[pos] != '"') {
            return std::nullopt;
        }
        const auto key_end = skip_json_string(text, pos);
        if (key_end == std::string_view::npos) {
            return std::nullopt;
        }
        const auto key = text.substr(pos + 1, key_end - pos - 2);
        if (key.find('\\') != std::string_view::npos) {
            return std::nullopt;
        }
        pos = skip_json_whitespace(text, key_end);
        if (pos >= text.size() || text[pos] != ':') {
            return std::nullopt;
        }
        pos = skip_json_whitespace(text, pos + 1);
        const auto value_end = skip_json_value(text, pos);
        if (value_end == std::string_view::npos || value_end == pos) {
            return std::nullopt;
        }
        if (key == "id") {
            envelope.id = text.substr(pos, value_end - pos);
        } else if (key == "result") {
            envelope.has_result_or_error = true;
            envelope.has_result = true;
        } else if (key == "error") {
            envelope.has_result_or_error = true;
            envelope.has_result = key == "result";
        }
        pos = skip_json_whitespace(text, value_end);
        if (pos >= text.size()) {
            return std::nullopt;
        }
        if (text[pos] == '}') {
            return envelope;
        }
        if (text[pos] != ',') {
            return std::nullopt;
        }
        pos = skip_json_whitespace(text, pos + 1);
    }
}

}  // namespace mcp::detail

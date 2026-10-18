#pragma once

#include <cstddef>
#include <optional>
#include <string_view>

namespace mcp::detail {

/// The top-level members of a JSON-RPC message that response routing reads.
struct JsonRpcEnvelope {
    /// Raw text of the `id` value, such as `7` or `"req-1"`; empty when the member is absent.
    std::string_view id;
    bool has_result_or_error{false};
};

inline std::size_t skip_json_whitespace(std::string_view text, std::size_t pos) {
    while (pos < text.size() &&
           (text[pos] == ' ' || text[pos] == '\t' || text[pos] == '\n' || text[pos] == '\r')) {
        ++pos;
    }
    return pos;
}

/// @return The position after the string that opens at @p pos, or npos when it is unterminated.
inline std::size_t skip_json_string(std::string_view text, std::size_t pos) {
    for (++pos; pos < text.size(); ++pos) {
        if (text[pos] == '\\') {
            ++pos;
        } else if (text[pos] == '"') {
            return pos + 1;
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
        } else if (key == "result" || key == "error") {
            envelope.has_result_or_error = true;
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

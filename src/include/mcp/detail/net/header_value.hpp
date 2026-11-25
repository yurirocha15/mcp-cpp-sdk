#pragma once

#include "mcp/detail/constants.hpp"
#include "mcp/detail/text/encoding.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace mcp::detail {

/// RFC 9110 field-value characters: visible ASCII, space and horizontal tab.
inline bool is_valid_header_value(std::string_view value) {
    return std::ranges::all_of(value, [](char raw) {
        const auto byte = static_cast<unsigned char>(raw);
        return byte == '\t' ||
               (byte >= constants::unicode::g_C0_CONTROLS_END && byte < constants::unicode::g_DELETE);
    });
}

/// The header text that carries @p value as an Mcp-Name or Mcp-Param value: the value itself when
/// it is plain header text, otherwise its UTF-8 bytes in a `=?base64?...?=` sentinel. A value needs
/// the sentinel when it has a character a header may not carry (non-ASCII, control), leading or
/// trailing space or tab, or already looks like a sentinel. The inverse of decode_header_value().
inline std::string encode_header_value(std::string_view value) {
    constexpr std::string_view prefix = "=?base64?";
    constexpr std::string_view suffix = "?=";
    const bool padded = !value.empty() && (value.front() == ' ' || value.front() == '\t' ||
                                           value.back() == ' ' || value.back() == '\t');
    const bool sentinel_like = value.size() >= prefix.size() + suffix.size() &&
                               value.starts_with(prefix) &&
                               value.substr(value.size() - suffix.size()) == suffix;
    if (is_valid_header_value(value) && !padded && !sentinel_like) {
        return std::string(value);
    }
    std::string encoded(prefix);
    encoded += base64_encode(value);
    encoded += suffix;
    return encoded;
}

/// The value a client meant by an Mcp-Name or Mcp-Param header: the header text itself, or the
/// UTF-8 text of a `=?base64?...?=` sentinel. Nullopt when the value has characters a header may not
/// carry or its sentinel does not hold Base64.
inline std::optional<std::string> decode_header_value(std::string_view value) {
    if (!is_valid_header_value(value)) {
        return std::nullopt;
    }
    constexpr std::string_view prefix = "=?base64?";
    constexpr std::string_view suffix = "?=";
    if (value.size() >= prefix.size() + suffix.size() && value.starts_with(prefix) &&
        value.substr(value.size() - suffix.size()) == suffix) {
        return base64_decode(value.substr(prefix.size(), value.size() - prefix.size() - suffix.size()));
    }
    return std::string(value);
}

}  // namespace mcp::detail

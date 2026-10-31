#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace mcp::detail {

/// RFC 9110 field-value characters: visible ASCII, space and horizontal tab.
inline bool is_valid_header_value(std::string_view value) {
    for (const char raw : value) {
        const auto byte = static_cast<unsigned char>(raw);
        if (byte != '\t' && (byte < 0x20 || byte > 0x7E)) {
            return false;
        }
    }
    return true;
}

/// Decodes standard padded Base64, or returns nullopt when @p text is not that.
inline std::optional<std::string> decode_base64(std::string_view text) {
    if (text.size() % 4 != 0) {
        return std::nullopt;
    }
    std::string decoded;
    decoded.reserve(text.size() / 4 * 3);
    std::uint32_t accumulator = 0;
    int bits = 0;
    std::size_t padding = 0;
    for (std::size_t index = 0; index < text.size(); ++index) {
        const char symbol = text[index];
        int value = -1;
        if (symbol >= 'A' && symbol <= 'Z') {
            value = symbol - 'A';
        } else if (symbol >= 'a' && symbol <= 'z') {
            value = symbol - 'a' + 26;
        } else if (symbol >= '0' && symbol <= '9') {
            value = symbol - '0' + 52;
        } else if (symbol == '+') {
            value = 62;
        } else if (symbol == '/') {
            value = 63;
        } else if (symbol == '=' && index + 2 >= text.size()) {
            ++padding;
            continue;
        } else {
            return std::nullopt;
        }
        if (padding != 0) {
            return std::nullopt;  // data after padding
        }
        accumulator = (accumulator << 6U) | static_cast<std::uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            decoded.push_back(static_cast<char>((accumulator >> static_cast<unsigned>(bits)) & 0xFFU));
        }
    }
    return decoded;
}

/// Encodes @p data as standard padded Base64.
inline std::string encode_base64(std::string_view data) {
    constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string encoded;
    encoded.reserve((data.size() + 2) / 3 * 4);
    std::size_t index = 0;
    for (; index + 2 < data.size(); index += 3) {
        const std::uint32_t group =
            (static_cast<std::uint32_t>(static_cast<unsigned char>(data[index])) << 16U) |
            (static_cast<std::uint32_t>(static_cast<unsigned char>(data[index + 1])) << 8U) |
            static_cast<std::uint32_t>(static_cast<unsigned char>(data[index + 2]));
        encoded.push_back(alphabet[(group >> 18U) & 0x3FU]);
        encoded.push_back(alphabet[(group >> 12U) & 0x3FU]);
        encoded.push_back(alphabet[(group >> 6U) & 0x3FU]);
        encoded.push_back(alphabet[group & 0x3FU]);
    }
    const std::size_t rest = data.size() - index;
    if (rest != 0) {
        std::uint32_t group = static_cast<std::uint32_t>(static_cast<unsigned char>(data[index]))
                              << 16U;
        if (rest == 2) {
            group |= static_cast<std::uint32_t>(static_cast<unsigned char>(data[index + 1])) << 8U;
        }
        encoded.push_back(alphabet[(group >> 18U) & 0x3FU]);
        encoded.push_back(alphabet[(group >> 12U) & 0x3FU]);
        encoded.push_back(rest == 2 ? alphabet[(group >> 6U) & 0x3FU] : '=');
        encoded.push_back('=');
    }
    return encoded;
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
                               value.substr(0, prefix.size()) == prefix &&
                               value.substr(value.size() - suffix.size()) == suffix;
    if (is_valid_header_value(value) && !padded && !sentinel_like) {
        return std::string(value);
    }
    std::string encoded(prefix);
    encoded += encode_base64(value);
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
    if (value.size() >= prefix.size() + suffix.size() && value.substr(0, prefix.size()) == prefix &&
        value.substr(value.size() - suffix.size()) == suffix) {
        return decode_base64(value.substr(prefix.size(), value.size() - prefix.size() - suffix.size()));
    }
    return std::string(value);
}

}  // namespace mcp::detail

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace mcp::detail {

/// Decodes standard base64 (RFC 4648 section 4) with `=` padding and no line breaks.
///
/// @return The decoded bytes, or nothing when @p text holds a character outside the alphabet,
///         misplaced padding or a length that is not a multiple of four.
inline std::optional<std::string> base64_decode(std::string_view text) {
    if (text.size() % 4 != 0) {
        return std::nullopt;
    }
    const auto value_of = [](char character) -> int {
        if (character >= 'A' && character <= 'Z') {
            return character - 'A';
        }
        if (character >= 'a' && character <= 'z') {
            return character - 'a' + 26;
        }
        if (character >= '0' && character <= '9') {
            return character - '0' + 52;
        }
        if (character == '+') {
            return 62;
        }
        if (character == '/') {
            return 63;
        }
        return -1;
    };
    std::string bytes;
    bytes.reserve(text.size() / 4 * 3);
    for (std::size_t index = 0; index < text.size(); index += 4) {
        std::array<int, 4> quad{};
        std::size_t padding = 0;
        for (std::size_t offset = 0; offset < 4; ++offset) {
            const char character = text[index + offset];
            if (character == '=') {
                // Padding is only the last one or two characters of the final quad.
                if (index + 4 != text.size() || offset < 2) {
                    return std::nullopt;
                }
                ++padding;
                quad[offset] = 0;
            } else {
                if (padding != 0) {
                    return std::nullopt;
                }
                quad[offset] = value_of(character);
                if (quad[offset] < 0) {
                    return std::nullopt;
                }
            }
        }
        const auto word =
            static_cast<std::uint32_t>(quad[0]) << 18 | static_cast<std::uint32_t>(quad[1]) << 12 |
            static_cast<std::uint32_t>(quad[2]) << 6 | static_cast<std::uint32_t>(quad[3]);
        bytes.push_back(static_cast<char>((word >> 16) & 0xFF));
        if (padding < 2) {
            bytes.push_back(static_cast<char>((word >> 8) & 0xFF));
        }
        if (padding < 1) {
            bytes.push_back(static_cast<char>(word & 0xFF));
        }
    }
    return bytes;
}

}  // namespace mcp::detail

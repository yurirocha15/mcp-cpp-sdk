#pragma once

#include "mcp/detail/constants.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace mcp::detail::unicode {

/// Whether @p code is a UTF-16 high surrogate, U+D800 to U+DBFF.
constexpr bool is_high_surrogate(std::uint32_t code) {
    return code >= constants::unicode::g_HIGH_SURROGATE_FIRST &&
           code <= constants::unicode::g_HIGH_SURROGATE_LAST;
}

/// Whether @p code is a UTF-16 low surrogate, U+DC00 to U+DFFF.
constexpr bool is_low_surrogate(std::uint32_t code) {
    return code >= constants::unicode::g_LOW_SURROGATE_FIRST &&
           code <= constants::unicode::g_LOW_SURROGATE_LAST;
}

/// Whether @p code is a UTF-16 surrogate of either kind, which no well-formed text holds alone.
constexpr bool is_surrogate(std::uint32_t code) {
    return code >= constants::unicode::g_HIGH_SURROGATE_FIRST &&
           code <= constants::unicode::g_LOW_SURROGATE_LAST;
}

/// The code point the UTF-16 surrogate pair @p high, @p low stands for.
constexpr std::uint32_t combine_surrogates(std::uint32_t high, std::uint32_t low) {
    return constants::unicode::g_SUPPLEMENTARY_FIRST +
           ((high - constants::unicode::g_HIGH_SURROGATE_FIRST)
            << constants::unicode::g_SURROGATE_BITS) +
           (low - constants::unicode::g_LOW_SURROGATE_FIRST);
}

/// Whether @p byte is a UTF-8 continuation byte, 10xxxxxx.
constexpr bool is_utf8_continuation(std::uint32_t byte) {
    return (byte & constants::unicode::g_UTF8_TAG_MASK) == constants::unicode::g_UTF8_CONTINUATION_TAG;
}

/// The length of the well-formed UTF-8 sequence that starts at @p at, a byte of 0x80 or more, or 0
/// when it is not one. Accepts exactly the sequences of Unicode's table 3-7, as nlohmann::json does.
inline std::size_t utf8_sequence_length(const unsigned char* at, const unsigned char* end) {
    const auto& lead = constants::unicode::g_UTF8_LEADS[at[0]];
    const auto left = static_cast<std::size_t>(end - at);
    if (lead.length == 0 || left < lead.length || at[1] < lead.second_first ||
        at[1] > lead.second_last) {
        return 0;
    }
    for (std::size_t i = 2; i < lead.length; ++i) {
        if (!is_utf8_continuation(at[i])) {
            return 0;
        }
    }
    return lead.length;
}

/// A decoded UTF-8 sequence. A length of 0 means the bytes were not well-formed UTF-8.
struct Utf8Sequence {
    std::size_t length{0};
    std::uint32_t code_point{0};
};

/// Decodes the UTF-8 sequence at @p index of @p text. Every ill-formed sequence is refused: a
/// truncated one, a bad continuation byte, an overlong form, a surrogate, and a value past
/// U+10FFFF.
inline Utf8Sequence decode_utf8(std::string_view text, std::size_t index) {
    const auto* at = reinterpret_cast<const unsigned char*>(text.data()) + index;
    if (*at < constants::unicode::g_ASCII_END) {
        return {.length = 1, .code_point = *at};
    }
    const std::size_t length =
        utf8_sequence_length(at, reinterpret_cast<const unsigned char*>(text.data()) + text.size());
    if (length == 0) {
        return {};
    }
    std::uint32_t code_point = at[0] & (constants::unicode::g_ASCII_MASK >> length);
    for (std::size_t i = 1; i < length; ++i) {
        code_point = (code_point << constants::unicode::g_UTF8_PAYLOAD_BITS) |
                     (at[i] & constants::unicode::g_UTF8_PAYLOAD_MASK);
    }
    return {.length = length, .code_point = code_point};
}

/// Appends the UTF-8 form of @p code, a code point that is not a surrogate, to @p out.
inline void append_utf8(std::string& out, std::uint32_t code) {
    if (code < constants::unicode::g_ASCII_END) {
        out.push_back(static_cast<char>(code));
        return;
    }
    std::size_t length = constants::unicode::g_UTF8_MAX_LENGTH;
    if (code < constants::unicode::g_THREE_BYTE_FIRST) {
        length = 2;
    } else if (code < constants::unicode::g_SUPPLEMENTARY_FIRST) {
        length = 3;
    }
    std::array<char, constants::unicode::g_UTF8_MAX_LENGTH> bytes{};
    for (std::size_t i = length - 1; i > 0; --i) {
        bytes[i] = static_cast<char>(constants::unicode::g_UTF8_CONTINUATION_TAG |
                                     (code & constants::unicode::g_UTF8_PAYLOAD_MASK));
        code >>= constants::unicode::g_UTF8_PAYLOAD_BITS;
    }
    bytes[0] = static_cast<char>(constants::unicode::g_UTF8_LEAD_TAGS[length] | code);
    out.append(bytes.data(), length);
}

}  // namespace mcp::detail::unicode

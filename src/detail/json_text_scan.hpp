#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace mcp::detail::json_text {

inline constexpr std::uint64_t g_ONES = 0x0101010101010101ULL;
inline constexpr std::uint64_t g_HIGH_BITS = 0x8080808080808080ULL;
/// The bytes needs_attention() looks at in one step.
inline constexpr std::size_t g_BLOCK_BYTES = sizeof(std::uint64_t);

/// Whether any of the eight bytes of @p word is a control character, `"`, `\` or not ASCII.
/// It can also flag a plain byte that follows a flagged one, which only costs a closer look.
constexpr bool needs_attention(std::uint64_t word) {
    const std::uint64_t control = (word - g_ONES * 0x20U) & ~word;
    const std::uint64_t quote = word ^ (g_ONES * 0x22U);
    const std::uint64_t backslash = word ^ (g_ONES * 0x5CU);
    const std::uint64_t quote_found = (quote - g_ONES) & ~quote;
    const std::uint64_t backslash_found = (backslash - g_ONES) & ~backslash;
    return ((control | quote_found | backslash_found | word) & g_HIGH_BITS) != 0;
}

/// Advances @p at past the bytes, eight at a time, that are none of those needs_attention()
/// looks for, stopping on the block that holds the first that may be one.
inline const unsigned char* skip_plain_blocks(const unsigned char* at, const unsigned char* end) {
    while (static_cast<std::size_t>(end - at) >= g_BLOCK_BYTES) {
        std::uint64_t word = 0;
        std::memcpy(&word, at, sizeof word);
        if (needs_attention(word)) {
            break;
        }
        at += g_BLOCK_BYTES;
    }
    return at;
}

/// The length of the well-formed UTF-8 sequence that starts at @p at, a byte of 0x80 or more, or 0
/// when it is not one. Accepts exactly the sequences of Unicode's table 3-7, as nlohmann::json does.
inline std::size_t utf8_sequence_length(const unsigned char* at, const unsigned char* end) {
    const auto left = static_cast<std::size_t>(end - at);
    const unsigned char lead = at[0];
    const auto continuation = [](unsigned char byte) { return (byte & 0xC0U) == 0x80U; };
    std::size_t length = 0;
    unsigned char low = 0x80U;  // The range of the second byte, narrower after some leads.
    unsigned char high = 0xBFU;
    if (lead >= 0xC2U && lead <= 0xDFU) {
        length = 2;
    } else if (lead >= 0xE0U && lead <= 0xEFU) {
        length = 3;
        low = lead == 0xE0U ? 0xA0U : low;
        high = lead == 0xEDU ? 0x9FU : high;
    } else if (lead >= 0xF0U && lead <= 0xF4U) {
        length = 4;
        low = lead == 0xF0U ? 0x90U : low;
        high = lead == 0xF4U ? 0x8FU : high;
    } else {
        return 0;
    }
    if (left < length || at[1] < low || at[1] > high) {
        return 0;
    }
    for (std::size_t i = 2; i < length; ++i) {
        if (!continuation(at[i])) {
            return 0;
        }
    }
    return length;
}

}  // namespace mcp::detail::json_text

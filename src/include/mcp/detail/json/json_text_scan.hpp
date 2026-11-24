#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include "mcp/detail/constants.hpp"

namespace mcp::detail::json_text {

/// Whether any of the eight bytes of @p word is a control character, `"`, `\` or not ASCII.
/// It can also flag a plain byte that follows a flagged one, which only costs a closer look.
constexpr bool needs_attention(std::uint64_t word) {
    const std::uint64_t control = (word - mcp::detail::constants::json_text::g_ONES * 0x20U) & ~word;
    const std::uint64_t quote = word ^ (mcp::detail::constants::json_text::g_ONES * 0x22U);
    const std::uint64_t backslash = word ^ (mcp::detail::constants::json_text::g_ONES * 0x5CU);
    const std::uint64_t quote_found = (quote - mcp::detail::constants::json_text::g_ONES) & ~quote;
    const std::uint64_t backslash_found =
        (backslash - mcp::detail::constants::json_text::g_ONES) & ~backslash;
    return ((control | quote_found | backslash_found | word) &
            mcp::detail::constants::json_text::g_HIGH_BITS) != 0;
}

/// Advances @p at past the bytes, eight at a time, that are none of those needs_attention()
/// looks for, stopping on the block that holds the first that may be one.
inline const unsigned char* skip_plain_blocks(const unsigned char* at, const unsigned char* end) {
    while (static_cast<std::size_t>(end - at) >= mcp::detail::constants::json_text::g_BLOCK_BYTES) {
        std::uint64_t word = 0;
        std::memcpy(&word, at, sizeof word);
        if (needs_attention(word)) {
            break;
        }
        at += mcp::detail::constants::json_text::g_BLOCK_BYTES;
    }
    return at;
}

}  // namespace mcp::detail::json_text

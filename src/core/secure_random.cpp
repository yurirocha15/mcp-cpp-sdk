#include "mcp/detail/text/secure_random.hpp"
#include <mcp/core/constants.hpp>

#include <openssl/rand.h>

#include <cstddef>
#include <stdexcept>
#include <vector>
#include "mcp/detail/constants.hpp"

namespace encoding = mcp::detail::constants::encoding;

namespace mcp::detail {

std::string generate_secure_session_id() {
    const std::size_t byte_count = (mcp::detail::constants::session::g_LENGTH + 1) / 2;
    std::vector<unsigned char> random_bytes(byte_count);
    if (RAND_bytes(random_bytes.data(), static_cast<int>(random_bytes.size())) != 1) {
        throw std::runtime_error("Failed to generate a cryptographically secure session id");
    }

    std::string session_id;
    session_id.reserve(mcp::detail::constants::session::g_LENGTH);
    for (const unsigned char value : random_bytes) {
        session_id.push_back(
            encoding::g_HEX_DIGITS[(value >> encoding::g_HEX_DIGIT_BITS) & encoding::g_HEX_DIGIT_MASK]);
        if (session_id.size() == mcp::detail::constants::session::g_LENGTH) {
            break;
        }
        session_id.push_back(encoding::g_HEX_DIGITS[value & encoding::g_HEX_DIGIT_MASK]);
        if (session_id.size() == mcp::detail::constants::session::g_LENGTH) {
            break;
        }
    }
    return session_id;
}

std::string generate_random_string(std::size_t length) {
    // Bytes at or above the largest multiple of the alphabet's size are drawn again, so that every
    // character is equally likely.
    static const std::size_t s_charset_size = encoding::g_UNRESERVED_CHARS.size();
    static const auto s_bias_limit =
        static_cast<unsigned char>((256 / s_charset_size) * s_charset_size);

    std::string result;
    result.reserve(length);
    while (result.size() < length) {
        unsigned char byte = 0;
        if (RAND_bytes(&byte, 1) != 1) {
            throw std::runtime_error("RAND_bytes failed");
        }
        if (byte < s_bias_limit) {
            result.push_back(encoding::g_UNRESERVED_CHARS[byte % s_charset_size]);
        }
    }
    return result;
}

}  // namespace mcp::detail

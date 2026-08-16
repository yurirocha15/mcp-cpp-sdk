#include <mcp/core/constants.hpp>
#include <mcp/detail/secure_random.hpp>

#include <openssl/rand.h>

#include <cstddef>
#include <stdexcept>
#include <vector>

namespace mcp::detail {

std::string generate_secure_session_id() {
    const std::size_t byte_count = (constants::g_session_id_length + 1) / 2;
    std::vector<unsigned char> random_bytes(byte_count);
    if (RAND_bytes(random_bytes.data(), static_cast<int>(random_bytes.size())) != 1) {
        throw std::runtime_error("Failed to generate a cryptographically secure session id");
    }

    std::string session_id;
    session_id.reserve(constants::g_session_id_length);
    for (const unsigned char value : random_bytes) {
        session_id.push_back(constants::g_hex_digits[(value >> 4U) & 0x0FU]);
        if (session_id.size() == constants::g_session_id_length) {
            break;
        }
        session_id.push_back(constants::g_hex_digits[value & 0x0FU]);
        if (session_id.size() == constants::g_session_id_length) {
            break;
        }
    }
    return session_id;
}

}  // namespace mcp::detail

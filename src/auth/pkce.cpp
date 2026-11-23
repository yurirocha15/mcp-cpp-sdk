#include <mcp/auth/pkce.hpp>
#include <mcp/core/constants.hpp>

#include "mcp/detail/text/encoding.hpp"
#include "mcp/detail/text/secure_random.hpp"

#include <cstddef>
#include <stdexcept>
#include <string>

namespace mcp::auth {

PkcePair generate_pkce_pair(std::size_t verifier_length) {
    if (verifier_length < mcp::constants::oauth::g_MIN_VERIFIER_LENGTH ||
        verifier_length > mcp::constants::oauth::g_MAX_VERIFIER_LENGTH) {
        throw std::invalid_argument("PKCE verifier length must be 43-128 characters");
    }

    PkcePair pair;
    pair.code_verifier = mcp::detail::generate_random_string(verifier_length);
    pair.challenge_method = "S256";

    pair.code_challenge = mcp::detail::base64url_encode(
        mcp::detail::digest_bytes(mcp::detail::sha256(pair.code_verifier)));
    return pair;
}

}  // namespace mcp::auth

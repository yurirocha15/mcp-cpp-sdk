#pragma once

#include <mcp/core/constants.hpp>
#include <mcp/core/export.hpp>

#include <cstddef>
#include <string>

namespace mcp::auth {

/**
 * @brief PKCE verifier and challenge values for OAuth authorization flows.
 */
struct PkcePair {
    std::string code_verifier;     ///< High-entropy verifier sent to the token endpoint.
    std::string code_challenge;    ///< Derived challenge sent to the authorization endpoint.
    std::string challenge_method;  ///< Challenge method name, typically S256.
};

/**
 * @brief Generate a PKCE verifier/challenge pair.
 *
 * @param verifier_length Length of the verifier string, between 43 and 128 characters.
 * @return A PKCE pair suitable for OAuth 2.1 authorization code flows.
 */
MCP_API PkcePair
generate_pkce_pair(std::size_t verifier_length = mcp::constants::oauth::g_DEFAULT_VERIFIER_LENGTH);

}  // namespace mcp::auth

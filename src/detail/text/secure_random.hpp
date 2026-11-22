#pragma once

#include <cstddef>
#include <string>

namespace mcp::detail {

/// A new session identifier: mcp::detail::constants::session::g_LENGTH lowercase hexadecimal
/// digits from OpenSSL's cryptographically secure generator.
std::string generate_secure_session_id();

/// @p length characters drawn uniformly from RFC 3986's unreserved set by OpenSSL's
/// cryptographically secure generator, as a PKCE verifier or a JWT identifier needs.
std::string generate_random_string(std::size_t length);

}  // namespace mcp::detail

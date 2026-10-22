#pragma once

#include <mcp/auth/oauth.hpp>

#include <optional>
#include <string>
#include <string_view>

namespace mcp::detail {

/// The principal an HTTP request is attributed to: the id the bearer token validator named, else
/// `h:` and the hex SHA-256 of the Authorization header, else empty. The digest keeps a token out of
/// every place a principal reaches, and still tells two clients' tokens apart.
inline std::string request_principal(const std::optional<std::string>& validator_id,
                                     std::string_view authorization) {
    if (validator_id) {
        return *validator_id;
    }
    if (authorization.empty()) {
        return {};
    }
    constexpr std::string_view hex_digits = "0123456789abcdef";
    const auto digest = auth::detail::sha256(std::string(authorization));
    std::string principal = "h:";
    principal.reserve(2 + digest.size() * 2);
    for (const unsigned char byte : digest) {
        principal += hex_digits[byte >> 4U];
        principal += hex_digits[byte & 0x0FU];
    }
    return principal;
}

}  // namespace mcp::detail

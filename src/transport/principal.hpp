#pragma once

#include <mcp/transport/http_types.hpp>

#include "../detail/text/encoding.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>

namespace mcp::detail {

/// The principal an HTTP request is attributed to: the id the bearer token validator named, else
/// `h:` and the hex SHA-256 of the bearer token, or of the whole Authorization header when it holds
/// no bearer token, else empty. The digest keeps a token out of every place a principal reaches,
/// and still tells two clients' tokens apart. The token alone counts because the scheme's name is
/// case-insensitive: one token must not make a principal for each way of writing it.
inline std::string request_principal(const std::optional<std::string>& validator_id,
                                     std::string_view authorization) {
    if (validator_id) {
        return *validator_id;
    }
    if (authorization.empty()) {
        return {};
    }
    auto token = http_bearer_token(authorization);
    token.remove_prefix(std::min(token.find_first_not_of(' '), token.size()));
    return "h:" + hex_encode(digest_bytes(sha256(token.empty() ? authorization : token)));
}

}  // namespace mcp::detail

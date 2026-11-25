#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mcp::auth::detail {

/// RFC 9728 section 3.3: does a protected-resource metadata `resource` value identify the server this
/// client is configured for? Accepted when the two are byte-exact, or when `resource` is a proper URI
/// prefix of `server_url` under RFC 8707 audience semantics: same scheme and authority, compared
/// byte-exact with no normalization (an explicit default port differs from an implicit one), and a
/// path that is empty/`"/"` (matches any path) or a prefix of the server URL's path on a `/` segment
/// boundary. A single trailing `/` on `resource`'s path is ignored. `resource` may not carry a query
/// or fragment.
///
/// An origin-root resource is accepted for any path by design: the root-PRM layout (conformance
/// `auth/metadata-var2`). This only decides whether the value may be sent as the `resource`
/// parameter; the authorization server remains the arbiter of the audience it issues.
bool resource_identifies_server(const std::string& resource, const std::string& server_url);

/// The scopes of a space-delimited scope string, leaving out empty ones.
std::vector<std::string> split_scopes(std::string_view scopes);

/// Join scopes into the space-delimited form an authorization request carries.
std::string join_scopes(const std::vector<std::string>& scopes);

/// Union of two scope strings: the scopes of @p primary in order, then whatever only @p secondary
/// carries; none when neither holds a scope. Step-up authorization re-authorizes on a challenge
/// that names only what the refused operation needed, so without the union an earlier grant's
/// scopes would be dropped.
std::optional<std::string> union_scopes(const std::optional<std::string>& primary,
                                        const std::optional<std::string>& secondary);

}  // namespace mcp::auth::detail

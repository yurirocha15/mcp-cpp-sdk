#pragma once

#include <mcp/core/core.hpp>
#include <mcp/core/export.hpp>

#include <boost/beast/http.hpp>
#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mcp {

namespace constants {

/**
 * @brief Default cap on an HTTP request body accepted by the server transports, in bytes.
 *
 * Binary content reaches an MCP server as base64 inside the JSON body — `ImageContent::data`,
 * `BlobResourceContents::blob` and `AudioContent` have no out-of-band or streaming path — and
 * base64 inflates by 4/3. Beast's own 1 MB default therefore admits only about 750 KB of raw
 * bytes, which ordinary phone photos and screenshots at 1-3 MB already exceed. Eight mebibytes
 * carries roughly 6 MB of raw content, so those payloads keep working, while still bounding what
 * one unauthenticated connection can make a server allocate.
 */
constexpr std::size_t g_default_max_request_body_bytes = 8 * 1024 * 1024;

}  // namespace constants

/**
 * @brief A generic list of string pairs, often used for query params or form fields.
 */
using KeyValuePairList = std::vector<std::pair<std::string, std::string>>;

/**
 * @brief A list of SSE events as (id, data) pairs.
 */
using SseEventList = KeyValuePairList;

/**
 * @brief Alias for a Boost.Beast HTTP request with a string body.
 */
using StringRequest = boost::beast::http::request<boost::beast::http::string_body>;

/**
 * @brief Alias for a Boost.Beast HTTP response with a string body.
 */
using StringResponse = boost::beast::http::response<boost::beast::http::string_body>;

/// @brief Synchronous validator invoked at the HTTP boundary for a bearer token.
using BearerTokenValidator = std::function<bool(std::string_view)>;

/**
 * @brief Asynchronous validator invoked at the HTTP boundary for a bearer token.
 *
 * Use this when deciding on a token requires I/O — token introspection, a JWKS fetch — so the
 * decision suspends instead of blocking the executor that is concurrently serving MCP traffic.
 *
 * The token is passed by value on purpose. A view would point into the Beast request buffer,
 * which is not guaranteed to outlive a suspension; one allocation is immaterial on a path that
 * is about to make a network call.
 */
using AsyncBearerTokenValidator = std::function<Task<bool>(std::string)>;

/**
 * @brief Extract a bearer token from an HTTP Authorization header.
 * @param authorization_header Complete Authorization header value.
 * @return The characters after the Bearer scheme, or an empty view when the
 *         scheme is absent or no characters follow it.
 */
MCP_API std::string_view http_bearer_token(std::string_view authorization_header);

/**
 * @brief Extract the path component of an HTTP request target.
 * @param target Request target, such as `/mcp?stream=1`.
 * @return The characters before the first `?` or `#`, or the whole target when neither appears.
 */
MCP_API std::string_view http_request_path(std::string_view target);

/**
 * @brief Parameters advertised in the `WWW-Authenticate` challenge on a 401.
 *
 * Every field is optional. A default-constructed config renders the bare `Bearer` challenge,
 * which is what the server transports send when no challenge is configured.
 */
struct BearerChallengeConfig {
    std::string resource_metadata;  ///< RFC 9728 metadata URL, sent as `resource_metadata="..."`.
    std::string scope;              ///< Space-delimited scopes, sent as `scope="..."`.
    std::string realm;              ///< RFC 7235 realm, sent as `realm="..."`.
    std::string error;              ///< RFC 6750 §3.1 code such as `invalid_token`.
};

/**
 * @brief Render a challenge config as a `WWW-Authenticate` header value.
 *
 * Set parameters are emitted in the order realm, error, scope, resource_metadata, each as an
 * RFC 7235 quoted-string with `\` and `"` backslash-escaped.
 *
 * @param config Challenge parameters to render.
 * @return `Bearer` when no field is set, otherwise `Bearer key="value", ...`.
 * @throws std::invalid_argument If a value holds a byte that cannot appear in a quoted-string,
 *         that is, anything outside horizontal tab and printable US-ASCII. The transports render
 *         the challenge once when it is configured, so an invalid value is reported to the caller
 *         that supplied it rather than while a request is being served.
 */
MCP_API std::string format_www_authenticate(const BearerChallengeConfig& config);

/**
 * @brief RFC 9728 protected-resource metadata served by a server transport.
 */
struct ProtectedResourceMetadataConfig {
    std::string resource;                            ///< Required. Canonical resource URL.
    std::vector<std::string> authorization_servers;  ///< Issuer URLs able to mint tokens.
    std::vector<std::string> scopes_supported;       ///< Scopes the resource recognizes.
    /// Path the document is served at. Empty means derive it from `resource` per RFC 9728 3.1;
    /// set it only to override that derivation.
    std::string path;
};

/**
 * @brief Path the metadata document is served at.
 *
 * Returns `path` when it is set. Otherwise derives it the way RFC 9728 3.1 requires, by inserting
 * the well-known segment between the authority and the resource's own path: a resource at
 * `https://host/mcp` is described at `/.well-known/oauth-protected-resource/mcp`, and one at
 * `https://host` at `/.well-known/oauth-protected-resource`. Defaulting to the bare well-known
 * path instead is correct only for a resource sitting at the origin root, and would leave every
 * other server publishing where clients do not look.
 *
 * @param metadata Metadata whose `resource` supplies the path component.
 * @return The path, always beginning with `/`.
 * @throws std::invalid_argument If `resource` is not an absolute URL with an authority.
 */
MCP_API std::string protected_resource_metadata_path(const ProtectedResourceMetadataConfig& metadata);

/**
 * @brief Render protected-resource metadata as the JSON document clients fetch.
 *
 * Empty list members are omitted rather than written as empty arrays.
 *
 * @param metadata Metadata to render.
 * @return A JSON object with `resource` and any populated list members.
 */
MCP_API std::string format_protected_resource_metadata(const ProtectedResourceMetadataConfig& metadata);

/**
 * @brief Build the absolute URL the metadata document is reachable at.
 *
 * The URL is the origin of `resource` — its scheme and authority — followed by
 * protected_resource_metadata_path(). It is built entirely from `resource`, never from the
 * address a transport happens to be bound to: behind a TLS terminator, a reverse proxy or a
 * container port mapping the listener's own origin is not the one clients can reach, so inferring
 * it would publish a URL that cannot be fetched.
 *
 * @param metadata Metadata whose `resource` supplies the origin and path.
 * @return The absolute metadata URL.
 * @throws std::invalid_argument If `resource` is not an absolute URL with an authority.
 */
MCP_API std::string protected_resource_metadata_url(const ProtectedResourceMetadataConfig& metadata);

}  // namespace mcp

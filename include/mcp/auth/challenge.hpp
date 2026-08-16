#pragma once

#include <mcp/core/export.hpp>
#include <mcp/transport/http_types.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mcp::auth {

/**
 * @brief A single authentication challenge parsed from a `WWW-Authenticate` header.
 *
 * @details Parameter names are matched case-insensitively and stored lower-cased. Quoted values are
 * unescaped. The named accessors below expose the parameters the MCP authorization flow consumes;
 * `parameters` retains every parameter in the order it appeared.
 */
struct BearerChallenge {
    std::string scheme;                ///< Authentication scheme exactly as written, e.g. `Bearer`.
    std::optional<std::string> realm;  ///< Optional `realm` parameter.
    std::optional<std::string> resource_metadata;  ///< Optional RFC 9728 `resource_metadata` URL.
    std::optional<std::string> scope;              ///< Optional space-delimited `scope` parameter.
    std::optional<std::string> error;              ///< Optional OAuth `error` code.
    std::optional<std::string> error_description;  ///< Optional human-readable error description.
    std::optional<std::string> error_uri;          ///< Optional URL describing the error.
    KeyValuePairList parameters;  ///< Every parameter, lower-cased names, source order preserved.

    /// @return True when the scheme is `Bearer`, compared case-insensitively.
    [[nodiscard]] MCP_API bool is_bearer() const;
};

/**
 * @brief Parse one `WWW-Authenticate` header value into its constituent challenges.
 *
 * @param header_value Raw header value.
 * @return Every challenge found, in source order.
 *
 * @details Handles quoted values with backslash escapes, case-insensitive parameter names,
 * arbitrary parameter ordering, and several comma-separated challenges in one header. Malformed
 * trailing input is discarded rather than throwing, so a partially understood header still yields
 * the challenges that precede the damage.
 */
[[nodiscard]] MCP_API std::vector<BearerChallenge> parse_www_authenticate(
    std::string_view header_value);

/**
 * @brief Parse several `WWW-Authenticate` header values into one ordered challenge list.
 *
 * @param header_values Raw header values, in the order the response carried them.
 * @return Every challenge found across all headers, in source order.
 */
[[nodiscard]] MCP_API std::vector<BearerChallenge> parse_www_authenticate(
    const std::vector<std::string>& header_values);

/**
 * @brief Select the challenge the MCP authorization flow should act on.
 *
 * @param challenges Challenges parsed from the response.
 * @return The first `Bearer` challenge, or `std::nullopt` when none is present.
 */
[[nodiscard]] MCP_API std::optional<BearerChallenge> select_bearer_challenge(
    const std::vector<BearerChallenge>& challenges);

/**
 * @brief Per-attempt record of an authorization request.
 *
 * @details One record is created per authorization attempt and holds every value the response must
 * be validated against: the CSRF `state`, the PKCE code verifier, and the issuer recorded from the
 * selected authorization server's metadata document. The recorded issuer is authentic only because
 * it comes from metadata the SDK itself fetched and validated; validation provides no protection
 * against an issuer taken from an unvalidated source.
 */
struct AuthorizationRequest {
    std::string authorization_url;  ///< Complete authorization endpoint URL including query.
    std::string state;              ///< CSRF state generated from a cryptographic random source.
    std::string code_verifier;      ///< PKCE verifier retained for the token request.
    std::string code_challenge;     ///< PKCE challenge sent to the authorization endpoint.
    std::string issuer;             ///< Issuer recorded from the selected authorization server.
    /// Value of `authorization_response_iss_parameter_supported` in that same metadata document.
    bool issuer_parameter_supported{false};
    std::string client_id;                ///< Client identifier used for this attempt.
    std::string redirect_uri;             ///< Redirect URI registered for this attempt.
    std::optional<std::string> scope;     ///< Requested scope, omitted when no scope was selected.
    std::optional<std::string> resource;  ///< RFC 8707 resource indicator for this attempt.
};

/**
 * @brief Authorization response returned to the application's redirect handler.
 */
struct AuthorizationResponse {
    std::optional<std::string> code;               ///< Authorization code on success.
    std::optional<std::string> state;              ///< State echoed by the authorization server.
    std::optional<std::string> iss;                ///< RFC 9207 issuer identifier, when supplied.
    std::optional<std::string> error;              ///< OAuth error code on failure.
    std::optional<std::string> error_description;  ///< Human-readable error description.
    std::optional<std::string> error_uri;          ///< URL describing the error.
};

/**
 * @brief Parse an authorization response from a redirect URL or a bare query string.
 *
 * @param redirect_url Redirect URL the authorization server sent the user agent to, or the query
 *        component on its own.
 * @return The decoded response parameters.
 *
 * @details Values are decoded as `application/x-www-form-urlencoded`, so `+` becomes a space and
 * percent-escapes are expanded. Decoding happens before any comparison, exactly as RFC 9207
 * Section 2.4 requires.
 */
[[nodiscard]] MCP_API AuthorizationResponse
parse_authorization_response(const std::string& redirect_url);

/**
 * @brief Outcome of validating an authorization response against its request record.
 */
enum class AuthorizationResponseStatus {
    accepted,         ///< Response is authentic and carries a code.
    state_missing,    ///< Response omitted `state` although the request sent one.
    state_mismatch,   ///< Returned `state` did not match the recorded value.
    issuer_missing,   ///< Server advertised `iss` support but omitted the parameter.
    issuer_mismatch,  ///< Returned `iss` did not match the recorded issuer.
    server_error,     ///< Authentic response carrying an OAuth `error`.
    code_missing,     ///< Authentic success response with no authorization code.
};

/**
 * @brief Result of validating an authorization response.
 */
struct AuthorizationResponseValidation {
    AuthorizationResponseStatus status{AuthorizationResponseStatus::accepted};  ///< Outcome.
    std::string message;  ///< Diagnostic description of the outcome.

    /// @return True when the authorization code may be sent to a token endpoint.
    [[nodiscard]] bool accepted() const { return status == AuthorizationResponseStatus::accepted; }
};

/**
 * @brief Validate an authorization response against the record of the request that produced it.
 *
 * @param request Per-attempt record holding the expected state and recorded issuer.
 * @param response Response returned to the application's redirect handler.
 * @return The validation outcome; only `accepted` permits transmitting the code.
 *
 * @details Applies RFC 9207 Section 2.4 as adopted by MCP, before any other interpretation of the
 * response:
 *
 * | `authorization_response_iss_parameter_supported` | `iss` present | Action                     |
 * |--------------------------------------------------|---------------|----------------------------|
 * | `true`                                           | yes           | Compare to recorded issuer |
 * | `true`                                           | no            | Reject                     |
 * | `false` or absent                                | yes           | Compare to recorded issuer |
 * | `false` or absent                                | no            | Proceed                    |
 *
 * Comparison is a plain byte-for-byte string equality test. No scheme or host case folding, no
 * default-port elision, no trailing-slash normalization and no percent-encoding normalization is
 * applied, and no canonicalizing URL helper is consulted.
 *
 * The issuer check runs ahead of the `error` path, so a response carrying `error`,
 * `error_description` or `error_uri` with a mismatched issuer is rejected as `issuer_mismatch` and
 * the caller must neither act on nor display those values.
 */
[[nodiscard]] MCP_API AuthorizationResponseValidation validate_authorization_response(
    const AuthorizationRequest& request, const AuthorizationResponse& response);

}  // namespace mcp::auth

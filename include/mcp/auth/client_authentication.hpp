#pragma once

#include <mcp/auth/oauth.hpp>
#include <mcp/core/constants.hpp>
#include <mcp/core/export.hpp>
#include <mcp/transport/http_types.hpp>

#include <chrono>
#include <optional>
#include <string>
#include <vector>

/**
 * @file client_authentication.hpp
 * @brief How a confidential client proves its identity at a token endpoint: a client secret or a
 *        signed JWT assertion (`private_key_jwt`, RFC 7523 section 2.2).
 */

namespace mcp::auth {

/// A private key and the JWT assertion settings built on it.
struct PrivateKeyJwtConfig {
    /// The private key as PEM: an RSA key (RS256, PS256) or an EC key on P-256 (ES256).
    std::string private_key_pem;
    /// Put in the JWT header as `kid`, so the authorization server finds the matching public key.
    std::optional<std::string> key_id;
    /// `RS256`, `PS256` or `ES256`. When unset the algorithm follows from the key and from the
    /// algorithms the authorization server lists in `token_endpoint_auth_signing_alg_values_supported`.
    std::optional<std::string> algorithm;
    /// How long each assertion is valid, from the moment it is signed.
    std::chrono::seconds lifetime{mcp::constants::oauth::g_DEFAULT_CLIENT_ASSERTION_LIFETIME};
    /// The `aud` claim. Defaults to the authorization server's issuer identifier, or to the token
    /// endpoint URL when its metadata names no issuer.
    std::optional<std::string> audience;
};

/// The credentials a client presents at a token endpoint.
struct ClientAuthentication {
    std::string client_id;                     ///< Client identifier; the JWT `iss` and `sub`.
    std::optional<std::string> client_secret;  ///< For `client_secret_basic` or `client_secret_post`.
    std::optional<PrivateKeyJwtConfig> private_key_jwt;  ///< For `private_key_jwt`.
    /// `private_key_jwt`, `client_secret_basic`, `client_secret_post` or `none`. When unset the
    /// method is chosen from the credentials and the methods the authorization server lists:
    /// `private_key_jwt` is preferred over a secret, and `client_secret_basic` over
    /// `client_secret_post`.
    std::optional<std::string> method;
};

/**
 * @brief Pick the signing algorithm for a key.
 *
 * @param config The key settings; an `algorithm` set there is used as given.
 * @param supported The authorization server's `token_endpoint_auth_signing_alg_values_supported`,
 *        when it published them.
 * @return `RS256`, `PS256` or `ES256`.
 * @throws std::invalid_argument When the key cannot be read, is not an RSA or P-256 EC key, or
 *         @p config names an algorithm the key cannot sign with.
 * @throws std::runtime_error When the authorization server lists no algorithm the key can sign with.
 */
[[nodiscard]] MCP_API std::string choose_signing_algorithm(
    const PrivateKeyJwtConfig& config, const std::optional<std::vector<std::string>>& supported);

/**
 * @brief Build and sign a client assertion.
 *
 * @details The JWT has the claims `iss` and `sub` (the client identifier), `aud`, `exp`, `iat` and a
 * random `jti`, and a header with `alg` and, when set, `kid`.
 *
 * @param config The key settings.
 * @param client_id The client identifier.
 * @param audience The `aud` claim, used unless `config.audience` is set.
 * @param algorithm `RS256`, `PS256` or `ES256`.
 * @return The compact JWS.
 * @throws std::invalid_argument When the key cannot be read or does not fit @p algorithm.
 */
[[nodiscard]] MCP_API std::string sign_client_assertion(const PrivateKeyJwtConfig& config,
                                                        const std::string& client_id,
                                                        const std::string& audience,
                                                        const std::string& algorithm);

/**
 * @brief Add client authentication to the form of a token request.
 *
 * @details `client_secret_basic` yields the `Authorization` header value and adds nothing to the
 * form. `client_secret_post` adds `client_id` and `client_secret`. `private_key_jwt` adds
 * `client_assertion_type` and `client_assertion`, and `client_id` only when @p send_client_id is
 * set, since RFC 7523 section 3 identifies the client by the assertion's `sub`. `none` adds
 * `client_id`.
 *
 * @param credentials What the client holds.
 * @param server The authorization server's metadata; its `issuer` is the default assertion
 *        audience (the `token_endpoint` when there is none), and the authentication methods and
 *        signing algorithms it lists constrain the choice.
 * @param params The form to add to.
 * @param send_client_id Also send `client_id` next to an assertion.
 * @return The `Authorization` header value, empty when the method uses none.
 * @throws std::invalid_argument When the credentials cannot satisfy the method.
 * @throws std::runtime_error When the authorization server lists no method the credentials fit.
 */
MCP_API std::string authenticate_token_request(const ClientAuthentication& credentials,
                                               const AuthServerMetadata& server,
                                               KeyValuePairList& params, bool send_client_id = false);

}  // namespace mcp::auth

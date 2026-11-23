#pragma once

#include <mcp/auth/authenticator.hpp>
#include <mcp/auth/challenge.hpp>
#include <mcp/auth/client_identity.hpp>
#include <mcp/auth/metadata_policy.hpp>
#include <mcp/auth/oauth_http_client.hpp>
#include <mcp/auth/token_store.hpp>
#include <mcp/core/async_fwd.hpp>
#include <mcp/core/executor.hpp>
#include <mcp/core/export.hpp>
#include <mcp/transport/proxy.hpp>
#include <mcp/transport/tls.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace mcp::auth {

/**
 * @brief Application-controlled consent step for an authorization attempt.
 *
 * @details Receives the per-attempt request record and returns the response the authorization
 * server delivered to the redirect URI. The SDK never launches a browser and never binds an
 * unsolicited listener; carrying the user agent to the authorization endpoint and collecting the
 * redirect is entirely the application's responsibility.
 */
using AuthorizationCallback = std::function<Task<AuthorizationResponse>(const AuthorizationRequest&)>;

/**
 * @brief Configuration for challenge-driven OAuth authorization.
 */
struct OAuthAuthorizationConfig {
    std::string server_url;  ///< MCP server URL that issued the challenge; also the token store key.
    /// Client identifier presented to the authorization server. Setting it is shorthand for
    /// injecting pre-registered credentials: it is treated exactly like
    /// `client_identity.pre_registered` and therefore never falls back to registration.
    std::string client_id;
    std::optional<std::string> client_secret;  ///< Optional confidential-client secret.
    /// Issuer the shorthand credentials above are bound to. Required whenever `client_secret` is
    /// set: the authorization server is named by the protected-resource document, so a secret that
    /// names no issuer is refused rather than presented. "Bound to no issuer" must never be read as
    /// "bound to every issuer". Leave empty for a public client, whose `client_id` is not a secret
    /// and may be presented to any authorization server.
    std::string client_issuer;
    std::string redirect_uri;  ///< Redirect URI the authorization response returns to.
    /// Scope override. When set it wins over both the challenge scope and the resource metadata;
    /// when unset the challenge scope is preferred, then `scopes_supported`, then no scope at all.
    std::optional<std::string> scope;
    /// Add `offline_access` to the scope of the authorization request when the authorization
    /// server lists it in `scopes_supported`, so it may issue a refresh token. Applies on top of
    /// the challenge and resource metadata scope sources; an explicit `scope` is used as it is. A
    /// server that does not list it is never sent it. Set to false to request exactly the
    /// selected scope.
    bool request_offline_access{true};
    /// Client identity inputs consulted when no `client_id` was supplied: a published client ID
    /// metadata document URL, injected credentials, and the metadata used for registration.
    ClientIdentityConfig client_identity;
    /// Issuer-keyed storage for credentials obtained by dynamic registration. When unset, a
    /// registration is performed per authorization attempt rather than reused.
    std::shared_ptr<ClientCredentialStore> credential_store;
    MetadataFetchPolicy policy;  ///< Outbound-request policy for every discovery and token request.
    HostResolver host_resolver;  ///< Optional custom resolver; the system resolver is used when unset.
    /// TLS settings for authorization servers; null uses the system's certificate authorities.
    std::shared_ptr<const TlsClientContext> tls;
    ProxyOptions proxy;  ///< Proxy for discovery and token requests; none by default.
};

/**
 * @brief Authenticator that performs challenge-driven OAuth authorization.
 *
 * @details Composes the pieces a `WWW-Authenticate` response requires: challenge parsing,
 * protected-resource and authorization-server discovery under the configured fetch policy, an
 * authorization request carrying S256 PKCE and cryptographic `state` bound to the issuer recorded
 * from the selected metadata document, RFC 9207 response validation, and an authorization-code
 * exchange carrying the RFC 8707 `resource` indicator.
 */
class MCP_API OAuthAuthorizationManager : public Authenticator {
   public:
    /**
     * @brief Construct a challenge-driven authorization manager.
     *
     * @param executor Executor used for asynchronous operations.
     * @param token_store Storage for the acquired access token.
     * @param config Client identity, redirect URI and outbound-request policy.
     * @param callback Application consent step invoked once per authorization attempt.
     * @throws std::invalid_argument When @p token_store or @p callback is empty, or when @p config
     *         sets `client_id`, `client_secret` or `client_issuer` together with
     *         `client_identity.pre_registered`.
     */
    OAuthAuthorizationManager(const Executor& executor, std::shared_ptr<TokenStore> token_store,
                              OAuthAuthorizationConfig config, AuthorizationCallback callback);

    /**
     * @brief Return the stored access token without network I/O.
     *
     * @return The stored access token, or an empty string when none has been acquired.
     */
    [[nodiscard]] std::string get_access_token() const override;

    /**
     * @brief Renew the stored token using its refresh token, when one is present.
     *
     * @details Renewal needs the token endpoint and client identity of an authorization this
     * manager completed in the same process. Without one, for instance with a refresh token loaded
     * from a persistent TokenStore after a restart, this returns false without network I/O.
     * Overlapping calls share one refresh grant, and the refresh token is read from the TokenStore
     * only by the call that leads it, so no call sends a token that another refresh has already
     * replaced in the store.
     *
     * @return true when a renewed token was persisted.
     */
    Task<bool> try_refresh_token() override;

    /**
     * @brief Run a full authorization exchange for a challenge response.
     *
     * @param www_authenticate Raw `WWW-Authenticate` header value.
     * @return true when authorization completed and an access token was persisted; false when the
     *         response carried no `Bearer` challenge to act on.
     *
     * @throws MetadataPolicyError If any discovery or token target is refused by the fetch policy.
     * @throws std::runtime_error If discovery fails or the authorization response is rejected.
     */
    Task<bool> try_handle_challenge(const std::string& www_authenticate) override;

    /**
     * @brief Return the record of the most recent authorization attempt.
     *
     * @return The request record, or `std::nullopt` when no attempt has been made.
     *
     * @details Exposes the state, PKCE verifier, recorded issuer and resource indicator that were
     * actually used, so applications can audit the binding an attempt was validated against.
     */
    [[nodiscard]] std::optional<AuthorizationRequest> last_authorization_request() const;

    /**
     * @brief Return the client identity used by the most recent authorization attempt.
     *
     * @return The identity, or `std::nullopt` when no attempt has resolved one.
     *
     * @details `source` records which path produced it, so an application can tell an injected
     * credential from a metadata-document identifier from a dynamic registration.
     */
    [[nodiscard]] std::optional<OAuthClientInformation> last_client_identity() const;

    /**
     * @brief Abort the authorization attempt and refresh in flight and release their followers with
     *        an error, leaving the manager usable.
     *
     * @details Aborts the in-progress discovery, registration, or token-exchange HTTP exchange and
     * wakes every follower coalesced onto it, which then fails. The next challenge or refresh starts
     * a new attempt on a fresh HTTP client, with its discovery cache empty. Called by
     * OAuthClientTransport::close(). Safe to call from any thread. A no-op once the manager is
     * closed.
     *
     * @note As with close(), a leader parked inside the application's own authorization callback is
     * not reachable from here. It stays parked until that callback returns, then fails at its next
     * network call. A leader that has not yet recorded its authorization request when this runs
     * fails without calling the callback.
     */
    void cancel_pending() override;

    /**
     * @brief Abort this manager's HTTP work, release parked followers with an error, and refuse
     *        further authorization attempts.
     *
     * @details Aborts the in-progress discovery or token-exchange HTTP exchange, wakes every
     * coalesced follower, and refuses new authorization attempts from this point on -- including one
     * racing this very call. Safe to call from any thread. Idempotent.
     *
     * @note A leader parked inside the application's own authorization callback is not reachable from
     * here. Followers coalesced onto that flow are still released, but the flow itself ends only when
     * the application's callback returns or its executor stops.
     */
    void close() override;

   private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

}  // namespace mcp::auth

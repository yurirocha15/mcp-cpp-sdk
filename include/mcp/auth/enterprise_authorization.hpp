#pragma once

#include <mcp/auth/client_authentication.hpp>
#include <mcp/auth/oauth.hpp>
#include <mcp/core/constants.hpp>
#include <mcp/core/export.hpp>

#include <functional>
#include <mcp/core/executor.hpp>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

/**
 * @file enterprise_authorization.hpp
 * @brief Enterprise-managed authorization
 *        (`io.modelcontextprotocol/enterprise-managed-authorization`).
 *
 * @details A client that signed a user in through an enterprise identity provider (IdP) exchanges the
 * identity assertion it holds for an Identity Assertion JWT Authorization Grant (ID-JAG) at the IdP
 * (RFC 8693 token exchange), then presents the ID-JAG to the MCP server's authorization server as a
 * JWT bearer grant (RFC 7523) and receives an access token with no user interaction. The extension is
 * specified in github.com/modelcontextprotocol/ext-auth,
 * specification/stable/enterprise-managed-authorization. Signing the user in, and obtaining the ID
 * token or other assertion, are the application's work.
 */

namespace mcp {
class Server;
}  // namespace mcp

namespace mcp::auth {

/// The identity assertion the IdP issued when the user signed in.
struct IdentityAssertion {
    std::string token;  ///< The ID token, refresh token or assertion.
    std::string token_type{
        std::string(mcp::constants::oauth::g_ID_TOKEN_TYPE)};  ///< Its token type URN.
};

/// Supplies the user's current identity assertion. Called for every token exchange, so an
/// application that refreshes its ID token returns the fresh one.
using IdentityAssertionProvider = std::function<Task<IdentityAssertion>()>;

/// Configuration of an EnterpriseAuthorizationAuthenticator.
struct EnterpriseAuthorizationConfig {
    /// URL of the MCP server; also the key the token is stored under, and what the protected
    /// resource metadata must identify.
    std::string server_url;
    /// Token endpoint of the enterprise IdP.
    std::string idp_token_endpoint;
    /// How the client authenticates at the IdP token endpoint; required when the IdP requires client
    /// authentication for the token exchange. With a secret and no `method`, the secret is sent in
    /// the request body, as in the extension's example. Without credentials only `client_id` is sent.
    ClientAuthentication idp_client;
    /// How the client authenticates at the MCP server's authorization server. Without a secret or a
    /// private key the client is public, or identified by a client ID metadata document URL used as
    /// `client_id`, and only `client_id` is sent.
    ClientAuthentication client;
    /// Scope to request. When unset the challenge's scope is used, then the resource's
    /// `scopes_supported`, then none.
    std::optional<std::string> scope;
    MetadataFetchPolicy policy;  ///< Outbound-request policy; it must allow the IdP's origin too.
    HostResolver host_resolver;  ///< Optional custom resolver; the system resolver when unset.
    /// TLS settings for authorization servers and the IdP; null uses the system's certificate
    /// authorities.
    std::shared_ptr<const TlsClientContext> tls;
    ProxyOptions proxy;  ///< Proxy for discovery and token requests; none by default.
};

/**
 * @brief Authenticator that obtains tokens with the enterprise-managed authorization flow.
 *
 * @details On a challenge it discovers the protected resource and its authorization server, refuses
 * a server whose `authorization_grant_profiles_supported` does not list
 * `urn:ietf:params:oauth:grant-profile:id-jag` (a server that publishes no such list is tried),
 * asks the provider for the identity assertion, exchanges
 * it at the IdP (`audience` is the authorization server's issuer, `resource` the MCP server's
 * resource identifier), checks that the IdP issued an `id-jag` token, and presents that as the
 * `assertion` of a `urn:ietf:params:oauth:grant-type:jwt-bearer` request at the authorization
 * server's token endpoint. The token is stored under `server_url`.
 *
 * No refresh token is involved: try_refresh_token() runs the exchange again, which calls the
 * provider again. Concurrent callers each run their own exchange.
 */
class MCP_API EnterpriseAuthorizationAuthenticator : public Authenticator {
   public:
    /**
     * @throws std::invalid_argument When @p token_store or @p provider is empty, or `server_url`,
     *         `idp_token_endpoint` or a client identifier is empty.
     */
    EnterpriseAuthorizationAuthenticator(const Executor& executor,
                                         std::shared_ptr<TokenStore> token_store,
                                         EnterpriseAuthorizationConfig config,
                                         IdentityAssertionProvider provider);
    ~EnterpriseAuthorizationAuthenticator() override;

    [[nodiscard]] std::string get_access_token() const override;

    /// Runs the exchange again against the server of the last grant. Returns false when there was
    /// none or it failed.
    Task<bool> try_refresh_token() override;

    /// @return false when @p www_authenticate holds no Bearer challenge.
    /// @throws std::runtime_error When discovery, the provider or either token request fails, or the
    ///         authorization server's profile list omits the ID-JAG grant profile.
    /// @throws MetadataPolicyError When the fetch policy refuses a target.
    Task<bool> try_handle_challenge(const std::string& www_authenticate) override;

    /// Obtains a token before any request, from the well-known discovery location. Reuses a token
    /// cached for the authorization server that is still valid. @return true when a token is stored.
    Task<bool> authorize();

    void cancel_pending() override;
    void close() override;

   private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

/// Declares support for the extension in the capabilities a client sends.
MCP_API void declare_enterprise_authorization_support(ClientCapabilities& capabilities);

/// Declares support for the extension in the capabilities a server advertises.
MCP_API void declare_enterprise_authorization_support(Server& server);

}  // namespace mcp::auth

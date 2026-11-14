#pragma once

#include <mcp/auth/client_authentication.hpp>
#include <mcp/auth/oauth.hpp>
#include <mcp/core/export.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

/**
 * @file client_credentials.hpp
 * @brief OAuth client credentials flow for machine-to-machine access
 *        (`io.modelcontextprotocol/oauth-client-credentials`).
 *
 * @details The client authenticates with pre-registered credentials and receives an access token
 * without any user interaction. The extension is specified in
 * github.com/modelcontextprotocol/ext-auth, specification/stable/oauth-client-credentials. It names
 * two ways to authenticate at the token endpoint: a signed JWT assertion (`private_key_jwt`,
 * recommended) and a client secret.
 */

namespace mcp::auth {

/// Identifier of the extension in `capabilities.extensions`.
inline constexpr std::string_view g_CLIENT_CREDENTIALS_EXTENSION_ID =
    "io.modelcontextprotocol/oauth-client-credentials";

/// Configuration of a ClientCredentialsAuthenticator.
struct ClientCredentialsConfig {
    /// URL of the MCP server; also the key the token is stored under, and what the protected
    /// resource metadata must identify.
    std::string server_url;
    /// The pre-registered credentials. Needs a `client_secret` or a `private_key_jwt`; the
    /// extension does not use dynamic client registration.
    ClientAuthentication client;
    /// Scope to request. When unset the challenge's scope is used, then the resource's
    /// `scopes_supported`, then none.
    std::optional<std::string> scope;
    MetadataFetchPolicy policy;  ///< Outbound-request policy for discovery and token requests.
    HostResolver host_resolver;  ///< Optional custom resolver; the system resolver when unset.
};

/**
 * @brief Authenticator that obtains tokens with the client credentials grant.
 *
 * @details On a `WWW-Authenticate` challenge it reads the protected resource metadata the challenge
 * names (or the well-known location), discovers the authorization server it lists, authenticates
 * with the configured credentials, and posts `grant_type=client_credentials` with the RFC 8707
 * `resource` indicator and the selected scope. The token is stored under `server_url`. The
 * authorization server's metadata must be fetched from its issuer, and the resource metadata must
 * identify the configured server; discovery refuses anything else before a credential is sent.
 *
 * The client credentials grant issues no refresh token, so try_refresh_token() asks for a new token
 * from the server the last grant used. Concurrent callers each request their own token.
 *
 * Declare the extension to the server with declare_client_credentials_support() on the client
 * capabilities.
 */
class MCP_API ClientCredentialsAuthenticator : public Authenticator {
   public:
    /**
     * @throws std::invalid_argument When @p token_store is null, `server_url` is empty, or the
     *         client has neither a secret nor a private key.
     */
    ClientCredentialsAuthenticator(const boost::asio::any_io_executor& executor,
                                   std::shared_ptr<TokenStore> token_store,
                                   ClientCredentialsConfig config);
    ~ClientCredentialsAuthenticator() override;

    [[nodiscard]] std::string get_access_token() const override;

    /// Requests a new token from the authorization server of the last grant. Returns false when
    /// there was none or the request failed.
    Task<bool> try_refresh_token() override;

    /// @return false when @p www_authenticate holds no Bearer challenge.
    /// @throws std::runtime_error When discovery or the grant fails.
    /// @throws MetadataPolicyError When the fetch policy refuses a target.
    Task<bool> try_handle_challenge(const std::string& www_authenticate) override;

    /// Obtains a token before any request: discovers the server from its well-known location and
    /// runs the grant, or reuses a token cached for the authorization server that is still valid.
    /// @return true when a token is stored.
    Task<bool> authorize();

    void cancel_pending() override;
    void close() override;

   private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

/// Declares support for the extension in the capabilities a client sends.
MCP_API void declare_client_credentials_support(ClientCapabilities& capabilities);

/// Declares support for the extension in the capabilities a server advertises.
MCP_API void declare_client_credentials_support(Server& server);

}  // namespace mcp::auth

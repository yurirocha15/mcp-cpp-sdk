#pragma once

#include <mcp/auth/authenticator.hpp>
#include <mcp/auth/oauth_http_client.hpp>
#include <mcp/auth/token_store.hpp>
#include <mcp/core/async_fwd.hpp>
#include <mcp/core/export.hpp>

#include <memory>
#include <string>

namespace mcp::auth {

/**
 * @brief OAuth 2.0 implementation of the Authenticator interface.
 */
class MCP_API OAuthAuthenticator : public Authenticator {
   public:
    /**
     * @brief Construct an authenticator that refreshes tokens through `oauth_client`.
     *
     * @throws std::invalid_argument if `oauth_client` is null.
     */
    OAuthAuthenticator(std::shared_ptr<TokenStore> token_store,
                       std::shared_ptr<OAuthHttpClient> oauth_client, OAuthConfig config,
                       std::string server_url);

    [[nodiscard]] std::string get_access_token() const override;

    Task<bool> try_refresh_token() override;

    /// @brief Stores an initial token obtained from an explicit OAuth exchange (e.g., authorization
    /// code flow).
    /// @param token The token response to persist via the configured TokenStore.
    void store_token(TokenResponse token);

    /// @brief Aborts this authenticator's in-flight token-refresh HTTP exchanges. Later refreshes
    /// run as usual. Other holders of the same OAuthHttpClient are unaffected.
    void cancel_pending() override;

    /// @brief Aborts this authenticator's in-flight token-refresh HTTP exchanges and refuses later
    /// ones. Other holders of the same OAuthHttpClient are unaffected.
    void close() override;

   private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

}  // namespace mcp::auth

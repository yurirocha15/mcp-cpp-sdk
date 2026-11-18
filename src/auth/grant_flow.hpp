#pragma once

#include <mcp/auth/oauth.hpp>
#include <mcp/core/async.hpp>

#include <algorithm>
#include <any>
#include <boost/asio/any_io_executor.hpp>
#include <cctype>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>

namespace mcp::auth::detail {

namespace net = boost::asio;

/// Throws unless @p token is a Bearer token, the only type these flows can present.
inline void require_bearer(const TokenResponse& token) {
    const auto& type = token.token_type;
    const bool bearer =
        type.size() == 6 && std::equal(type.begin(), type.end(), "bearer", [](char left, char right) {
            return std::tolower(static_cast<unsigned char>(left)) == right;
        });
    if (token.access_token.empty() || !bearer) {
        throw std::runtime_error("The token response did not hold a Bearer access token");
    }
}

/// Whom a grant is for.
struct GrantIdentity {
    std::string key;  ///< Cached tokens are reused only for an equal key; empty when not bound.
    std::any state;   ///< What the flow read while naming the identity, handed to request_token.
};

/// What discovery established about the server a grant is requested from.
struct GrantTarget {
    ProtectedResourceMetadata resource;  ///< Verified to identify the MCP server.
    AuthServerMetadata server;           ///< Fetched from the issuer the resource names.
    std::optional<std::string> scope;    ///< Scope to request; none when nothing selects one.
    GrantIdentity identity;              ///< Whom the grant is for, see GrantFlow::identity.
};

/// The part the authenticators of the machine-to-machine grants share: challenge handling,
/// discovery of the protected resource and its authorization server, the token cache and the
/// cancel and close behaviour of an Authenticator. A derived class supplies the token request.
///
/// Concurrent callers each run their own discovery and token request: nothing coalesces them. Each
/// request is a complete grant, so the extra requests cost round trips and nothing else.
class GrantFlow : public std::enable_shared_from_this<GrantFlow> {
   public:
    GrantFlow(const net::any_io_executor& executor, std::shared_ptr<TokenStore> token_store,
              std::string server_url, MetadataFetchPolicy policy, HostResolver host_resolver,
              std::optional<std::string> scope);
    virtual ~GrantFlow() = default;

    GrantFlow(const GrantFlow&) = delete;
    GrantFlow& operator=(const GrantFlow&) = delete;

    /// The stored access token, empty when there is none.
    [[nodiscard]] std::string access_token() const;

    /// Obtain a token for a `WWW-Authenticate` challenge. @return false when it holds no Bearer
    /// challenge. Always requests a new token: the challenge says the stored one was refused.
    Task<bool> handle_challenge(std::string www_authenticate);

    /// Obtain a token without a challenge, using one cached for the authorization server when it is
    /// still valid and covers the scope.
    Task<bool> authorize();

    /// Request a new token from the server the last grant used. @return false when there is none
    /// or the request fails.
    Task<bool> renew();

    void cancel_pending();
    void close();

   protected:
    /// Performs the grant against @p target and returns the token the authorization server issued.
    virtual Task<TokenResponse> request_token(std::shared_ptr<OAuthHttpClient> http_client,
                                              GrantTarget target) = 0;

    /// Names the party the next grant is for; cached tokens are reused only for the same key. The
    /// default is empty, for flows whose credentials do not change.
    virtual Task<GrantIdentity> identity();

    /// Throws unless @p server may issue the grant; the default accepts any server.
    virtual void check_server(const AuthServerMetadata& server) const;

   private:
    struct Operation;
    static Task<bool> run(std::shared_ptr<Operation> operation);
    static Task<bool> run_renewal(std::shared_ptr<GrantFlow> self);
    std::shared_ptr<OAuthHttpClient> make_client() const;
    void record(const GrantTarget& target, const TokenResponse& token);

    net::any_io_executor executor_;
    std::shared_ptr<TokenStore> token_store_;
    std::string server_url_;
    MetadataFetchPolicy policy_;
    HostResolver host_resolver_;
    std::optional<std::string> scope_;

    mutable std::mutex mutex_;
    bool closed_{false};
    std::shared_ptr<OAuthHttpClient> http_client_;
    std::shared_ptr<OAuthDiscoveryClient> discovery_;
    std::optional<GrantTarget> last_target_;
    std::optional<std::string> granted_scope_;
    /// Tokens by authorization server issuer and identity, with the scope each was requested for.
    std::map<std::pair<std::string, std::string>, std::pair<TokenResponse, std::optional<std::string>>>
        cache_;
};

}  // namespace mcp::auth::detail

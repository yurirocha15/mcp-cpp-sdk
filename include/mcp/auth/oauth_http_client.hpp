#pragma once

#include <mcp/auth/metadata_policy.hpp>
#include <mcp/auth/token_store.hpp>
#include <mcp/core/async_fwd.hpp>
#include <mcp/core/executor.hpp>
#include <mcp/core/export.hpp>
#include <mcp/transport/http_types.hpp>
#include <mcp/transport/proxy.hpp>
#include <mcp/transport/tls.hpp>

#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace mcp::auth {

/**
 * @brief A discovery document request answered with an HTTP error status.
 *
 * @details Thrown by OAuthHttpClient::get_json() for a status of 400 or more. Discovery reads it to
 * tell a document that does not exist (404 or 410 at the address asked for) from one it could not
 * get.
 */
class MCP_API MetadataStatusError : public std::runtime_error {
   public:
    /**
     * @brief Construct the error for one answered request.
     *
     * @param status HTTP status of the answer.
     * @param url URL that answered, the last one when redirects were followed.
     * @param after_redirect Whether the answer came from a redirect target.
     */
    MetadataStatusError(unsigned int status, std::string url, bool after_redirect);

    /** @return HTTP status of the answer. */
    [[nodiscard]] unsigned int status() const noexcept { return status_; }

    /** @return URL that answered. */
    [[nodiscard]] const std::string& url() const noexcept { return url_; }

    /** @return Whether the answer came from a redirect target rather than the URL asked for. */
    [[nodiscard]] bool after_redirect() const noexcept { return after_redirect_; }

   private:
    unsigned int status_;
    std::string url_;
    bool after_redirect_;
};

/**
 * @brief Configuration for OAuth token exchange and refresh operations.
 */
struct OAuthConfig {
    std::string client_id;                              ///< OAuth client identifier.
    std::optional<std::string> client_secret;           ///< Optional confidential-client secret.
    std::string token_endpoint;                         ///< Token endpoint URL.
    std::optional<std::string> authorization_endpoint;  ///< Optional authorization endpoint URL.
    std::optional<std::string> revocation_endpoint;     ///< Optional revocation endpoint URL.
    std::string redirect_uri;             ///< Redirect URI used during authorization code flow.
    std::optional<std::string> scope;     ///< Optional requested scope string.
    std::optional<std::string> resource;  ///< Optional resource or audience hint.
    /// Client authentication method for the token endpoint, spelled as the authorization server
    /// spells it in `token_endpoint_auth_methods_supported`. `client_secret_basic` puts the
    /// credentials in the HTTP Basic header and nowhere else; `none` sends no secret at all;
    /// anything else, including leaving this unset, puts the secret in the request body.
    std::optional<std::string> token_endpoint_auth_method;
};

/**
 * @brief Resolves a host and port to candidate address literals.
 *
 * @details When the fetch policy refuses a target the resolver is never invoked. When none is
 * installed the executor's system resolver is used.
 *
 * The resolver is called synchronously on the client's strand, so it must return promptly: while
 * it runs, every other request on the client, in every scope, waits behind it, and so does the
 * socket close that abort_pending() and a scope's abort() rely on.
 */
using HostResolver =
    std::function<std::vector<std::string>(const std::string& host, const std::string& port)>;

class OAuthHttpClientScope;

namespace detail {

/// One scope's abort latch. Opaque here and defined in the implementation: a scope holds its own
/// latch rather than a name for one the client keeps, which is what lets the latch die with the
/// scope instead of accumulating on the client for the life of the process.
struct OAuthScopeState;

}  // namespace detail

/**
 * @brief Minimal HTTP client for OAuth token exchange and metadata retrieval.
 */
class MCP_API OAuthHttpClient {
   public:
    /**
     * @brief Construct an OAuth HTTP client.
     *
     * @param executor Executor used for asynchronous operations.
     *
     * @note Without an explicit fetch policy the client holds a default-constructed
     * `MetadataFetchPolicy` (empty origin allow list, https-only, non-loopback), so every request is
     * refused until a policy is installed via set_metadata_policy(). A client that follows
     * `WWW-Authenticate` challenges must be given a policy naming the origins it is meant to reach,
     * because the URLs it then visits are influenced by the server it talks to.
     */
    explicit OAuthHttpClient(const Executor& executor);

    /**
     * @brief Apply an outbound-request policy to every request this client issues.
     *
     * @param policy Policy governing schemes, origins, resolved addresses, response size and
     *        redirect depth.
     *
     * @details May be called at any time: an exchange already running keeps the policy it started
     * with, and the next one picks up the new value. Each request is validated before host
     * resolution, every resolved address is classified before connecting and pinned for the
     * connection, response bodies are capped, and redirects are bounded and individually
     * re-validated.
     *
     * @warning Do not call this and set_host_resolver() in sequence to change both: an exchange
     * started between the two calls runs with one new value and one old one. Use configure().
     */
    void set_metadata_policy(MetadataFetchPolicy policy);

    /**
     * @brief Install a custom host resolver.
     *
     * @param resolver Resolver invoked in place of the system resolver.
     *
     * @warning Do not call this and set_metadata_policy() in sequence to change both; use
     * configure().
     */
    void set_host_resolver(HostResolver resolver);

    /**
     * @brief Install an outbound-request policy and a host resolver as one indivisible change.
     *
     * @param policy Policy governing schemes, origins, resolved addresses, response size and
     *        redirect depth.
     * @param resolver Resolver invoked in place of the system resolver; an empty resolver restores
     *        the executor's system resolver.
     *
     * @details An exchange started around this call runs entirely with the configuration before it or
     * entirely with the configuration after it. Use this whenever both are changed on a client that
     * may already be serving requests.
     */
    void configure(MetadataFetchPolicy policy, HostResolver resolver);

    /**
     * @brief Install the policy, the resolver, the TLS settings and the proxy as one indivisible
     * change.
     *
     * @param policy As above.
     * @param resolver As above.
     * @param tls TLS settings for `https` servers; null checks them against the system's
     *        certificate authorities.
     * @param proxy The proxy requests go through. The proxy resolves the names of the servers it
     *        reaches, so the policy's address checks apply only to addresses written in a URL.
     *        Connections to loopback hosts never use it.
     *
     * @details The two-argument form keeps the TLS settings and proxy the client has. Without
     * either, `https` servers are checked against the system's certificate authorities and every
     * request connects directly.
     * @throws std::invalid_argument If a proxy URL is malformed or not supported.
     */
    void configure(MetadataFetchPolicy policy, HostResolver resolver,
                   std::shared_ptr<const TlsClientContext> tls, ProxyOptions proxy = {});

    /**
     * @brief Exchange an authorization code for an access token.
     *
     * @param config OAuth client configuration.
     * @param code Authorization code obtained from the authorization server.
     * @param code_verifier PKCE verifier associated with the original request.
     * @return A task resolving to the parsed token response.
     */
    Task<TokenResponse> exchange_code(const OAuthConfig& config, const std::string& code,
                                      const std::string& code_verifier);

    /**
     * @brief Refresh an access token using a refresh token.
     *
     * @param config OAuth client configuration.
     * @param refresh_token Refresh token issued by the authorization server.
     * @return A task resolving to the parsed token response.
     */
    Task<TokenResponse> refresh_token(const OAuthConfig& config, const std::string& refresh_token);

    /**
     * @brief Fetch a JSON document from an OAuth discovery endpoint.
     *
     * @param url HTTP URL to fetch.
     * @return A task resolving to the parsed JSON body.
     * @throws MetadataStatusError When the server answers with a status of 400 or more.
     * @throws MetadataPolicyError When the fetch policy refuses the URL or a redirect target.
     */
    Task<nlohmann::json> get_json(const std::string& url);

    /**
     * @brief Post a JSON document to an OAuth endpoint and read the JSON reply.
     *
     * @param url HTTP URL to post to.
     * @param body JSON request body.
     * @return A task resolving to the parsed JSON response.
     *
     * @details Used for RFC 7591 dynamic client registration. The target is validated against the
     * fetch policy exactly like every other request this client issues, and redirects are not
     * followed for a POST.
     */
    Task<nlohmann::json> post_json(const std::string& url, const nlohmann::json& body);

    /**
     * @brief Post a form to a token endpoint and read the JSON reply.
     *
     * @param url Token endpoint URL.
     * @param params Form parameters, sent as `application/x-www-form-urlencoded`.
     * @param authorization Value of the `Authorization` header, or empty to send none.
     * @return A task resolving to the parsed JSON reply.
     *
     * @details For grants the client does not build itself, such as client credentials, token
     * exchange and the JWT bearer grant. The target is validated against the fetch policy like every
     * other request, redirects are not followed, and an HTTP error status fails the request.
     */
    Task<nlohmann::json> post_form(const std::string& url, const KeyValuePairList& params,
                                   const std::string& authorization = {});

    /**
     * @brief Abort every HTTP exchange currently in flight on this client, and refuse every one
     *        issued afterward.
     *
     * @details Closes the underlying socket of each active exchange, so a pending resolve, connect,
     * write, or read completes with an error instead of hanging. Safe to call from any thread. Sticky
     * and irreversible: a request issued afterward -- even one that has not yet made its first
     * network call -- fails immediately instead of running to completion. Idempotent.
     */
    void abort_pending();

    /**
     * @brief Open an independently abortable scope on this client.
     *
     * @return A scope that issues requests through this client but can be aborted on its own.
     *
     * @details Requests issued through a scope are tracked against it; aborting it closes those and
     * refuses later ones, leaving every other scope and the client's own unscoped requests untouched.
     * `abort_pending()` still ends everything, scopes included. A scope keeps the underlying client
     * alive, so it stays usable even if the `OAuthHttpClient` object it came from is destroyed.
     */
    [[nodiscard]] OAuthHttpClientScope make_scope();

   private:
    struct Impl;
    std::shared_ptr<Impl> impl_;

    friend class OAuthHttpClientScope;
};

/**
 * @brief An independently abortable view of an OAuthHttpClient.
 *
 * @details Issues requests exactly as the client does, but tracks them separately so `abort()` ends
 * this scope's work alone. Obtained from `OAuthHttpClient::make_scope()`. Use one wherever the client
 * is shared: `OAuthHttpClient::abort_pending()` disables the client permanently for every holder,
 * and since a failed refresh is reported as a plain `false`, the other holders see no error, only
 * requests that stop working.
 *
 * Copyable, and every copy names the same scope, so aborting through any copy aborts them all.
 */
class MCP_API OAuthHttpClientScope {
   public:
    /** @see OAuthHttpClient::exchange_code */
    Task<TokenResponse> exchange_code(const OAuthConfig& config, const std::string& code,
                                      const std::string& code_verifier);

    /** @see OAuthHttpClient::refresh_token */
    Task<TokenResponse> refresh_token(const OAuthConfig& config, const std::string& refresh_token);

    /** @see OAuthHttpClient::get_json */
    Task<nlohmann::json> get_json(const std::string& url);

    /** @see OAuthHttpClient::post_json */
    Task<nlohmann::json> post_json(const std::string& url, const nlohmann::json& body);

    /** @see OAuthHttpClient::post_form */
    Task<nlohmann::json> post_form(const std::string& url, const KeyValuePairList& params,
                                   const std::string& authorization = {});

    /**
     * @brief Abort this scope's in-flight exchanges and refuse every one issued through it after.
     *
     * @details The scoped counterpart of `OAuthHttpClient::abort_pending()`: safe from any thread,
     * sticky, irreversible and idempotent. Requests issued through the client directly, or through
     * any other scope, are unaffected.
     */
    void abort();

   private:
    friend class OAuthHttpClient;

    OAuthHttpClientScope(std::shared_ptr<OAuthHttpClient::Impl> impl,
                         std::shared_ptr<detail::OAuthScopeState> state);

    std::shared_ptr<OAuthHttpClient::Impl> impl_;
    /// The latch itself, not a name for one held elsewhere. Every exchange issued through this
    /// scope holds the same control block, so the flag lives exactly as long as someone can
    /// still consult it and is freed once nobody can.
    std::shared_ptr<detail::OAuthScopeState> state_;
};

}  // namespace mcp::auth

#pragma once

#include <mcp/auth/challenge.hpp>
#include <mcp/auth/metadata_policy.hpp>
#include <mcp/core/constants.hpp>
#include <mcp/core/context.hpp>
#include <mcp/core/core.hpp>
#include <mcp/core/export.hpp>
#include <mcp/server/server.hpp>
#include <mcp/transport/http_types.hpp>
#include <mcp/transport/transport.hpp>

#include <array>
#include <boost/asio/any_io_executor.hpp>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mcp::auth {

namespace net = boost::asio;

namespace constants {

constexpr int g_default_token_lifetime_safety_margin_seconds = 30;
constexpr std::size_t g_buffer_size = 4096;
constexpr int g_shift4 = 4;
constexpr int g_shift6 = 6;
constexpr int g_shift8 = 8;
constexpr int g_shift12 = 12;
constexpr int g_shift16 = 16;
constexpr int g_shift18 = 18;
constexpr unsigned int g_mask0x3F = 0x3F;
constexpr std::size_t g_min_verifier_length = 43;
constexpr std::size_t g_max_verifier_length = 128;
constexpr unsigned int g_mask0x0F = 0x0F;
constexpr std::size_t g_sha256_digest_length = 32;
constexpr std::size_t g_default_verifier_length = 64;
constexpr std::size_t g_default_cache_ttl_seconds = 300;
}  // namespace constants

namespace detail {
MCP_API std::string base64_encode(const unsigned char* data, std::size_t len);
MCP_API std::string base64url_encode(const unsigned char* data, std::size_t len);

/// SHA-256 via OpenSSL EVP interface.
MCP_API std::array<unsigned char, constants::g_sha256_digest_length> sha256(const std::string& input);

MCP_API std::string generate_random_string(std::size_t length);
MCP_API std::string url_encode(const std::string& value);
MCP_API std::string build_form_body(const KeyValuePairList& params);

}  // namespace detail

/**
 * @brief PKCE verifier and challenge values for OAuth authorization flows.
 */
struct PkcePair {
    std::string code_verifier;     ///< High-entropy verifier sent to the token endpoint.
    std::string code_challenge;    ///< Derived challenge sent to the authorization endpoint.
    std::string challenge_method;  ///< Challenge method name, typically S256.
};

/**
 * @brief Generate a PKCE verifier/challenge pair.
 *
 * @param verifier_length Length of the verifier string, between 43 and 128 characters.
 * @return A PKCE pair suitable for OAuth 2.1 authorization code flows.
 */
MCP_API PkcePair generate_pkce_pair(std::size_t verifier_length = constants::g_default_verifier_length);

/**
 * @brief OAuth token response data returned by an authorization server.
 */
struct TokenResponse {
    std::string access_token;                  ///< Access token used for authenticated MCP requests.
    std::string token_type;                    ///< Token type, typically Bearer.
    std::optional<std::string> refresh_token;  ///< Optional refresh token for renewal.
    std::optional<int> expires_in;             ///< Lifetime in seconds reported by the server.
    std::optional<std::string> scope;          ///< Granted OAuth scope string.
    std::chrono::steady_clock::time_point received_at{
        std::chrono::steady_clock::now()};  ///< Local receipt time.

    /**
     * @brief Determine whether the token should be treated as expired.
     *
     * @param margin Safety margin in seconds applied before reported expiry.
     * @return True when the token is expired or within the safety margin.
     */
    [[nodiscard]] MCP_API bool is_expired(
        int margin = constants::g_default_token_lifetime_safety_margin_seconds) const;
};

/**
 * @brief Deserialize a token response from JSON.
 *
 * @param j JSON token payload.
 * @param t Token response to populate.
 */
MCP_API void from_json(const nlohmann::json& j, TokenResponse& t);

/**
 * @brief Serialize a token response to JSON.
 *
 * @param j JSON object to populate.
 * @param t Token response to serialize.
 */
MCP_API void to_json(nlohmann::json& j, const TokenResponse& t);

/**
 * @brief Abstract storage interface for OAuth tokens keyed by server URL.
 */
class TokenStore {
   public:
    virtual ~TokenStore() = default;
    /**
     * @brief Store or replace the token associated with a server URL.
     *
     * @param server_url MCP server URL used as the storage key.
     * @param token Token data to persist.
     */
    virtual void store(const std::string& server_url, TokenResponse token) = 0;
    /**
     * @brief Load the token associated with a server URL.
     *
     * @param server_url MCP server URL used as the storage key.
     * @return The stored token, if present.
     */
    [[nodiscard]] virtual std::optional<TokenResponse> load(const std::string& server_url) const = 0;
    /**
     * @brief Remove the token associated with a server URL.
     *
     * @param server_url MCP server URL used as the storage key.
     */
    virtual void remove(const std::string& server_url) = 0;
};

/**
 * @brief Thread-safe in-memory token store implementation.
 */
class MCP_API InMemoryTokenStore : public TokenStore {
   public:
    InMemoryTokenStore();
    ~InMemoryTokenStore() override;

    InMemoryTokenStore(const InMemoryTokenStore&) = delete;
    InMemoryTokenStore& operator=(const InMemoryTokenStore&) = delete;
    InMemoryTokenStore(InMemoryTokenStore&&) = delete;
    InMemoryTokenStore& operator=(InMemoryTokenStore&&) = delete;

    /**
     * @brief Store or replace a token in the in-memory cache.
     *
     * @param server_url MCP server URL used as the storage key.
     * @param token Token data to persist.
     */
    void store(const std::string& server_url, TokenResponse token) override;

    /**
     * @brief Load a token from the in-memory cache.
     *
     * @param server_url MCP server URL used as the storage key.
     * @return The stored token, if present.
     */
    std::optional<TokenResponse> load(const std::string& server_url) const override;

    /**
     * @brief Remove a token from the in-memory cache.
     *
     * @param server_url MCP server URL used as the storage key.
     */
    void remove(const std::string& server_url) override;

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
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
};

/**
 * @brief Resolves a host and port to candidate address literals.
 *
 * @details This is the single gateway through which the OAuth HTTP client obtains a connectable
 * address. When the fetch policy refuses a target the resolver is never invoked, so no socket can
 * be opened for it. Applications may install one to route lookups through a custom or pinned
 * resolver; when none is installed the executor's system resolver is used.
 */
using HostResolver =
    std::function<std::vector<std::string>(const std::string& host, const std::string& port)>;

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
     * @note Constructed without a fetch policy, the client issues requests to whatever URL the
     * caller supplies. That is appropriate only for URLs the application chose itself. Any client
     * that follows a `WWW-Authenticate` challenge must be given a policy via
     * set_metadata_policy(), because the URLs it visits are then attacker-influenced.
     */
    explicit OAuthHttpClient(const net::any_io_executor& executor);

    /**
     * @brief Apply an outbound-request policy to every request this client issues.
     *
     * @param policy Policy governing schemes, origins, resolved addresses, response size and
     *        redirect depth.
     *
     * @details Must be installed before the first request. Once installed, each request is
     * validated before host resolution, every resolved address is classified before connecting,
     * the addresses from that single resolution are pinned for the connection, response bodies are
     * capped, and redirects are bounded and individually re-validated.
     */
    void set_metadata_policy(MetadataFetchPolicy policy);

    /**
     * @brief Install a custom host resolver.
     *
     * @param resolver Resolver invoked in place of the system resolver.
     */
    void set_host_resolver(HostResolver resolver);

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
     */
    Task<nlohmann::json> get_json(const std::string& url);

   private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

/**
 * @brief Metadata exposed by an OAuth protected resource.
 */
struct ProtectedResourceMetadata {
    std::string resource;                            ///< Protected resource identifier.
    std::vector<std::string> authorization_servers;  ///< Authorization servers that can issue tokens.
    std::optional<std::vector<std::string>> scopes_supported;  ///< Optional supported scopes.
    nlohmann::json raw;                                        ///< Raw source document.
};

/**
 * @brief Deserialize protected-resource metadata from JSON.
 *
 * @param j JSON metadata payload.
 * @param m Metadata structure to populate.
 */
MCP_API void from_json(const nlohmann::json& j, ProtectedResourceMetadata& m);

/**
 * @brief Metadata exposed by an OAuth authorization server.
 */
struct AuthServerMetadata {
    std::string issuer;                                ///< Authorization server issuer URL.
    std::string authorization_endpoint;                ///< Authorization endpoint URL.
    std::string token_endpoint;                        ///< Token endpoint URL.
    std::optional<std::string> revocation_endpoint;    ///< Optional revocation endpoint URL.
    std::optional<std::string> registration_endpoint;  ///< Optional dynamic registration endpoint URL.
    std::optional<std::vector<std::string>> scopes_supported;  ///< Optional supported scopes.
    std::optional<std::vector<std::string>>
        response_types_supported;  ///< Optional supported response types.
    std::optional<std::vector<std::string>> grant_types_supported;  ///< Optional supported grant types.
    std::optional<std::vector<std::string>>
        code_challenge_methods_supported;  ///< Optional PKCE methods.
    std::optional<bool>
        authorization_response_iss_parameter_supported;  ///< Optional RFC 9207 `iss` support flag.
    std::optional<bool>
        client_id_metadata_document_supported;  ///< Optional client ID metadata document support flag.
    std::optional<std::vector<std::string>>
        token_endpoint_auth_methods_supported;  ///< Optional token endpoint auth methods.
    nlohmann::json raw;                         ///< Raw source document.
};

/**
 * @brief Deserialize authorization-server metadata from JSON.
 *
 * @param j JSON metadata payload.
 * @param m Metadata structure to populate.
 */
MCP_API void from_json(const nlohmann::json& j, AuthServerMetadata& m);

template <typename T>
struct CachedEntry {
    T data;
    std::chrono::steady_clock::time_point expires_at;

    [[nodiscard]] bool is_expired() const { return std::chrono::steady_clock::now() >= expires_at; }
};

/**
 * @brief Client for OAuth protected-resource and authorization-server discovery.
 */
class MCP_API OAuthDiscoveryClient {
   public:
    /**
     * @brief Construct an OAuth discovery client.
     *
     * @param http_client HTTP helper used to fetch discovery documents.
     * @param cache_ttl Time-to-live for cached discovery responses.
     */
    explicit OAuthDiscoveryClient(
        std::shared_ptr<OAuthHttpClient> http_client,
        std::chrono::seconds cache_ttl = std::chrono::seconds(constants::g_default_cache_ttl_seconds));

    OAuthDiscoveryClient(const OAuthDiscoveryClient&) = delete;
    OAuthDiscoveryClient& operator=(const OAuthDiscoveryClient&) = delete;
    OAuthDiscoveryClient(OAuthDiscoveryClient&&) = delete;
    OAuthDiscoveryClient& operator=(OAuthDiscoveryClient&&) = delete;

    /**
     * @brief Discover metadata for a protected resource.
     *
     * @param resource_url Resource URL whose metadata should be resolved.
     * @return A task resolving to the discovered protected-resource metadata.
     */
    Task<ProtectedResourceMetadata> discover_protected_resource(const std::string& resource_url);

    /**
     * @brief Discover metadata for a protected resource, honouring a challenge-supplied URL.
     *
     * @param resource_url Resource URL whose metadata should be resolved.
     * @param challenge_metadata_url `resource_metadata` URL taken from a `WWW-Authenticate`
     *        challenge, when the challenge supplied one.
     * @return A task resolving to the discovered protected-resource metadata.
     *
     * @details When the challenge supplied a URL it is fetched and nothing else is tried, so a
     * server that advertises its metadata location is taken at its word. Only when no URL was
     * supplied does the well-known fallback run, trying the path-based location before the root
     * one.
     */
    Task<ProtectedResourceMetadata> discover_protected_resource(
        const std::string& resource_url, const std::optional<std::string>& challenge_metadata_url);

    /**
     * @brief Discover metadata for an authorization server.
     *
     * @param issuer_url Issuer URL or base URL of the authorization server.
     * @return A task resolving to the discovered authorization-server metadata.
     */
    Task<AuthServerMetadata> discover_auth_server(const std::string& issuer_url);

    /**
     * @brief Clear all cached discovery metadata.
     */
    void clear_cache();

   private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

/// @brief Legacy callback used to validate a bearer token embedded in request metadata.
using TokenValidator = std::function<Task<bool>(const std::string& token)>;

/**
 * @brief Create legacy middleware that validates bearer tokens in request metadata.
 *
 * For Streamable HTTP servers, prefer set_bearer_token_validator() on the HTTP transport or
 * session manager so authentication is enforced at the HTTP boundary.
 *
 * @param validator Async callback that returns true when the token is accepted.
 * @return Middleware enforcing presence and validity of `_meta.auth_token`.
 */
MCP_API Middleware make_auth_middleware(TokenValidator validator);

/**
 * @brief Extract a bearer token from an Authorization header value.
 *
 * @param auth_header_value Header value to parse.
 * @return The token value without the `Bearer ` prefix, or an empty string on mismatch.
 */
MCP_API std::string extract_bearer_token(std::string_view auth_header_value);

/**
 * @brief Abstract interface for providing access tokens and handling refresh.
 *
 * @details Implementations must be safe to call from within an asio strand context.
 * `get_access_token()` must be lightweight — no network I/O. Returns the current bearer token or empty
 * string. `try_refresh_token()` is a coroutine that performs network I/O to refresh the token and
 * persists the new token via the configured token store on success.
 */
class Authenticator {
   public:
    virtual ~Authenticator() = default;

    /**
     * @brief Returns the current bearer token without network I/O.
     *
     * @return The stored access token, or an empty string when none is available.
     */
    [[nodiscard]] virtual std::string get_access_token() const = 0;

    /**
     * @brief Performs a token refresh via network I/O, persisting the new token on success.
     *
     * @return true if refresh succeeded and a new token was persisted; false otherwise.
     */
    virtual Task<bool> try_refresh_token() = 0;

    /**
     * @brief Performs challenge-driven authorization for a `WWW-Authenticate` response.
     *
     * @param www_authenticate Raw `WWW-Authenticate` header value from the challenge response.
     * @return true if authorization completed and a new token was persisted; false otherwise.
     *
     * @details Called before try_refresh_token() when a challenge is available, so an
     * implementation can discover metadata and run a full authorization exchange rather than only
     * renewing an existing grant. The default implementation reports that it handled nothing,
     * which keeps existing Authenticator implementations source-compatible.
     */
    virtual Task<bool> try_handle_challenge(const std::string& www_authenticate) {
        (void)www_authenticate;
        co_return false;
    }
};

/**
 * @brief OAuth 2.0 implementation of the Authenticator interface.
 */
class MCP_API OAuthAuthenticator : public Authenticator {
   public:
    OAuthAuthenticator(std::shared_ptr<TokenStore> token_store,
                       std::shared_ptr<OAuthHttpClient> oauth_client, OAuthConfig config,
                       std::string server_url);

    [[nodiscard]] std::string get_access_token() const override;

    Task<bool> try_refresh_token() override;

    /// @brief Stores an initial token obtained from an explicit OAuth exchange (e.g., authorization
    /// code flow).
    /// @param token The token response to persist via the configured TokenStore.
    void store_token(TokenResponse token);

   private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

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
    std::string client_id;   ///< Client identifier presented to the authorization server.
    std::optional<std::string> client_secret;  ///< Optional confidential-client secret.
    std::string redirect_uri;                  ///< Redirect URI the authorization response returns to.
    /// Scope override. When set it wins over both the challenge scope and the resource metadata;
    /// when unset the challenge scope is preferred, then `scopes_supported`, then no scope at all.
    std::optional<std::string> scope;
    MetadataFetchPolicy policy;  ///< Outbound-request policy for every discovery and token request.
    HostResolver host_resolver;  ///< Optional custom resolver; the system resolver is used when unset.
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
     */
    OAuthAuthorizationManager(const net::any_io_executor& executor,
                              std::shared_ptr<TokenStore> token_store, OAuthAuthorizationConfig config,
                              AuthorizationCallback callback);

    /**
     * @brief Return the stored access token without network I/O.
     *
     * @return The stored access token, or an empty string when none has been acquired.
     */
    [[nodiscard]] std::string get_access_token() const override;

    /**
     * @brief Renew the stored token using its refresh token, when one is present.
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

   private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

/**
 * @brief Resource limits for legacy JSON-RPC authentication replay correlation.
 *
 * @details Pending request wires are retained only until a matching response arrives, the entry is
 * evicted to satisfy these limits, the TTL elapses, or the transport closes. A zero limit or a
 * non-positive TTL disables legacy response-driven replay correlation. HTTP status-driven refresh
 * and retry does not depend on this cache.
 */
struct OAuthClientTransportOptions {
    std::size_t max_pending_requests{256};                    ///< Maximum retained request count.
    std::size_t max_pending_request_bytes{16 * 1024 * 1024};  ///< Approximate retained wire/key bytes.
    std::chrono::milliseconds pending_request_ttl{std::chrono::minutes(5)};  ///< Replay eligibility.
};

/**
 * @brief Transport wrapper that supplies and refreshes OAuth bearer tokens.
 *
 * @details For HttpClientTransport, tokens are sent in the HTTP Authorization header. The legacy
 * request-metadata mechanism is retained only for non-HTTP transports. The wrapper also handles token
 * refresh on legacy JSON-RPC authorization failures. When the server returns -32000, the
 * transport calls `Authenticator::try_refresh_token()` and, if successful, re-sends the request whose
 * ID matches the error response and reads the new response. Only one retry attempt is
 * made per `read_message()` call. Callers should ensure messages are idempotent since they may be
 * re-sent after a token refresh. Outstanding request wires are tracked by JSON-RPC request ID in a
 * bounded, expiring cache so an authentication error cannot replay a different concurrent request.
 */
class MCP_API OAuthClientTransport final : public ITransport {
   public:
    /**
     * @brief Construct an authenticated transport wrapper.
     *
     * @param inner Underlying transport used for MCP message exchange.
     * @param authenticator Authenticator used to retrieve and refresh tokens.
     */
    OAuthClientTransport(std::shared_ptr<ITransport> inner,
                         std::shared_ptr<Authenticator> authenticator);

    /**
     * @brief Construct an authenticated transport wrapper with replay-correlation limits.
     *
     * @param inner Underlying transport used for MCP message exchange.
     * @param authenticator Authenticator used to retrieve and refresh tokens.
     * @param options Bounds and TTL for retaining outstanding request wires.
     */
    OAuthClientTransport(std::shared_ptr<ITransport> inner,
                         std::shared_ptr<Authenticator> authenticator,
                         OAuthClientTransportOptions options);

    /**
     * @brief Read a message from the inner transport, retrying once on authentication errors.
     * @details If the received message contains the legacy JSON-RPC authentication error -32000,
     * attempts one token refresh via `Authenticator::try_refresh_token()`. On successful
     * refresh, replays the matching outstanding request while it remains eligible and returns the new
     * response. If refresh fails, correlation has expired or been evicted, or a second auth error is
     * received, returns the error response as-is. If the response cannot be parsed as JSON, the raw
     * string is returned unchanged.
     * @return The (possibly retried) raw message string.
     */
    Task<std::string> read_message() override;

    /**
     * @brief Inject the current bearer token into an outgoing MCP message.
     *
     * @param message Serialized JSON-RPC request or notification.
     * @return A task that completes once the wrapped transport accepts the message.
     */
    Task<void> write_message(std::string_view message) override;

    /**
     * @brief Close the wrapped transport.
     */
    void close() override;

   private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

}  // namespace mcp::auth

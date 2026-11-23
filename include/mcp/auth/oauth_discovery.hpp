#pragma once

#include <mcp/auth/oauth_http_client.hpp>
#include <mcp/core/async_fwd.hpp>
#include <mcp/core/constants.hpp>
#include <mcp/core/export.hpp>

#include <chrono>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace mcp::auth {

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
 * @brief Caller's verdict on a protected-resource document, before it is trusted or cached.
 *
 * @details Invoked with the parsed document on the way out of discovery, whether it was just fetched
 * or served from the cache. Throwing rejects it: the exception reaches the caller, no cache entry is
 * written, and discovery does not fall through to the next candidate URL.
 */
using ProtectedResourceAcceptor = std::function<void(const ProtectedResourceMetadata&)>;

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
    explicit OAuthDiscoveryClient(std::shared_ptr<OAuthHttpClient> http_client,
                                  std::chrono::seconds cache_ttl = std::chrono::seconds(
                                      mcp::constants::oauth::g_DEFAULT_CACHE_TTL_SECONDS));

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
     * @details When the challenge supplied a URL it is fetched and nothing else is tried. Otherwise
     * the well-known fallback runs, trying the path-based location before the root one.
     */
    Task<ProtectedResourceMetadata> discover_protected_resource(
        const std::string& resource_url, const std::optional<std::string>& challenge_metadata_url);

    /**
     * @brief Discover metadata for a protected resource, subject to the caller's acceptance.
     *
     * @param resource_url Resource URL whose metadata should be resolved.
     * @param challenge_metadata_url `resource_metadata` URL taken from a `WWW-Authenticate`
     *        challenge, when the challenge supplied one.
     * @param accept Called with the document before it is returned or cached; throwing rejects it.
     * @return A task resolving to the discovered protected-resource metadata.
     *
     * @details Identical to the two-argument form except that nothing is written to the cache until
     * `accept` has passed on it, and `accept` runs on a cache hit as well.
     */
    Task<ProtectedResourceMetadata> discover_protected_resource(
        const std::string& resource_url, const std::optional<std::string>& challenge_metadata_url,
        ProtectedResourceAcceptor accept);

    /**
     * @brief Drop the cached protected-resource metadata for one resource.
     *
     * @param resource_url Resource URL the metadata was discovered for.
     * @param challenge_metadata_url `resource_metadata` URL of the challenge the metadata was
     *        discovered through, when it supplied one.
     *
     * @details The next discover_protected_resource() call for the same arguments fetches the
     * document again. A client that has just been challenged uses this so the authorization server
     * it authorizes at is the one the resource names now, not the one the cached copy named.
     */
    void forget_protected_resource(const std::string& resource_url,
                                   const std::optional<std::string>& challenge_metadata_url);

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

}  // namespace mcp::auth

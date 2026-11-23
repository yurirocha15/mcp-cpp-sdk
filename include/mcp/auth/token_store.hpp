#pragma once

#include <mcp/core/constants.hpp>
#include <mcp/core/export.hpp>

#include <chrono>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

namespace mcp::auth {

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
        int margin = mcp::constants::oauth::g_DEFAULT_TOKEN_LIFETIME_SAFETY_MARGIN_SECONDS) const;
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

}  // namespace mcp::auth

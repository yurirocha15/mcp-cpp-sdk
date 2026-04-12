#pragma once

#include <mcp/context.hpp>
#include <mcp/core.hpp>
#include <mcp/server.hpp>
#include <mcp/transport.hpp>

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <algorithm>
#include <array>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mcp::auth {

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;

namespace detail {

inline std::string base64_encode(const unsigned char* data, std::size_t len) {
    static constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string result;
    result.reserve(((len + 2) / 3) * 4);

    for (std::size_t i = 0; i < len; i += 3) {
        unsigned int n = static_cast<unsigned int>(data[i]) << 16;
        if (i + 1 < len) {
            n |= static_cast<unsigned int>(data[i + 1]) << 8;
        }
        if (i + 2 < len) {
            n |= static_cast<unsigned int>(data[i + 2]);
        }

        result.push_back(alphabet[(n >> 18) & 0x3F]);
        result.push_back(alphabet[(n >> 12) & 0x3F]);
        result.push_back((i + 1 < len) ? alphabet[(n >> 6) & 0x3F] : '=');
        result.push_back((i + 2 < len) ? alphabet[n & 0x3F] : '=');
    }

    return result;
}

inline std::string base64url_encode(const unsigned char* data, std::size_t len) {
    auto encoded = base64_encode(data, len);

    for (auto& ch : encoded) {
        if (ch == '+') {
            ch = '-';
        } else if (ch == '/') {
            ch = '_';
        }
    }
    encoded.erase(std::remove(encoded.begin(), encoded.end(), '='), encoded.end());
    return encoded;
}

/// SHA-256 via OpenSSL EVP interface.
inline std::array<unsigned char, 32> sha256(const std::string& input) {
    std::array<unsigned char, 32> digest{};
    unsigned int digest_len = 0;

    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!ctx || EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) != 1 ||
        EVP_DigestUpdate(ctx.get(), input.data(), input.size()) != 1 ||
        EVP_DigestFinal_ex(ctx.get(), digest.data(), &digest_len) != 1) {
        throw std::runtime_error("OpenSSL SHA-256 failed");
    }

    return digest;
}

inline std::string generate_random_string(std::size_t length) {
    static constexpr char unreserved_chars[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~";
    static constexpr std::size_t charset_size = sizeof(unreserved_chars) - 1;
    // Largest multiple of charset_size that fits in a byte (avoids modulo bias)
    static constexpr unsigned char bias_limit =
        static_cast<unsigned char>((256 / charset_size) * charset_size);

    std::string result;
    result.reserve(length);
    while (result.size() < length) {
        unsigned char byte = 0;
        if (RAND_bytes(&byte, 1) != 1) {
            throw std::runtime_error("RAND_bytes failed");
        }
        if (byte < bias_limit) {
            result.push_back(unreserved_chars[byte % charset_size]);
        }
    }
    return result;
}

inline std::string url_encode(const std::string& value) {
    static constexpr char hex_chars[] = "0123456789ABCDEF";
    std::string result;
    result.reserve(value.size() * 3);

    for (unsigned char ch : value) {
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
            ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            result.push_back(static_cast<char>(ch));
        } else {
            result.push_back('%');
            result.push_back(hex_chars[ch >> 4]);
            result.push_back(hex_chars[ch & 0x0F]);
        }
    }
    return result;
}

inline std::string build_form_body(const std::vector<std::pair<std::string, std::string>>& params) {
    std::string body;
    for (const auto& [key, value] : params) {
        if (!body.empty()) {
            body.push_back('&');
        }
        body += url_encode(key) + "=" + url_encode(value);
    }
    return body;
}

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
inline PkcePair generate_pkce_pair(std::size_t verifier_length = 64) {
    if (verifier_length < 43 || verifier_length > 128) {
        throw std::invalid_argument("PKCE verifier length must be 43-128 characters");
    }

    PkcePair pair;
    pair.code_verifier = detail::generate_random_string(verifier_length);
    pair.challenge_method = "S256";

    auto hash = detail::sha256(pair.code_verifier);
    pair.code_challenge = detail::base64url_encode(hash.data(), hash.size());

    return pair;
}

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
    [[nodiscard]] bool is_expired(int margin = 30) const {
        if (!expires_in.has_value()) {
            return false;
        }
        auto expiry = received_at + std::chrono::seconds(*expires_in) - std::chrono::seconds(margin);
        return std::chrono::steady_clock::now() >= expiry;
    }
};

/**
 * @brief Deserialize a token response from JSON.
 *
 * @param j JSON token payload.
 * @param t Token response to populate.
 */
inline void from_json(const nlohmann::json& j, TokenResponse& t) {
    j.at("access_token").get_to(t.access_token);
    t.token_type = j.value("token_type", "Bearer");
    if (j.contains("refresh_token")) {
        t.refresh_token = j.at("refresh_token").get<std::string>();
    }
    if (j.contains("expires_in")) {
        t.expires_in = j.at("expires_in").get<int>();
    }
    if (j.contains("scope")) {
        t.scope = j.at("scope").get<std::string>();
    }
    t.received_at = std::chrono::steady_clock::now();
}

/**
 * @brief Serialize a token response to JSON.
 *
 * @param j JSON object to populate.
 * @param t Token response to serialize.
 */
inline void to_json(nlohmann::json& j, const TokenResponse& t) {
    j = nlohmann::json{{"access_token", t.access_token}, {"token_type", t.token_type}};
    if (t.refresh_token) {
        j["refresh_token"] = *t.refresh_token;
    }
    if (t.expires_in) {
        j["expires_in"] = *t.expires_in;
    }
    if (t.scope) {
        j["scope"] = *t.scope;
    }
}

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
    virtual std::optional<TokenResponse> load(const std::string& server_url) const = 0;
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
class InMemoryTokenStore : public TokenStore {
   public:
    /**
     * @brief Store or replace a token in the in-memory cache.
     *
     * @param server_url MCP server URL used as the storage key.
     * @param token Token data to persist.
     */
    void store(const std::string& server_url, TokenResponse token) override {
        std::lock_guard lock(mutex_);
        tokens_[server_url] = std::move(token);
    }

    /**
     * @brief Load a token from the in-memory cache.
     *
     * @param server_url MCP server URL used as the storage key.
     * @return The stored token, if present.
     */
    std::optional<TokenResponse> load(const std::string& server_url) const override {
        std::lock_guard lock(mutex_);
        auto it = tokens_.find(server_url);
        if (it == tokens_.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    /**
     * @brief Remove a token from the in-memory cache.
     *
     * @param server_url MCP server URL used as the storage key.
     */
    void remove(const std::string& server_url) override {
        std::lock_guard lock(mutex_);
        tokens_.erase(server_url);
    }

   private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, TokenResponse> tokens_;
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
 * @brief Minimal HTTP client for OAuth token exchange and metadata retrieval.
 */
class OAuthHttpClient {
   public:
    /**
     * @brief Construct an OAuth HTTP client.
     *
     * @param executor Executor used for asynchronous operations.
     */
    explicit OAuthHttpClient(const net::any_io_executor& executor)
        : strand_(net::make_strand(executor)) {}

    /**
     * @brief Exchange an authorization code for an access token.
     *
     * @param config OAuth client configuration.
     * @param code Authorization code obtained from the authorization server.
     * @param code_verifier PKCE verifier associated with the original request.
     * @return A task resolving to the parsed token response.
     */
    Task<TokenResponse> exchange_code(const OAuthConfig& config, const std::string& code,
                                      const std::string& code_verifier) {
        std::vector<std::pair<std::string, std::string>> params = {
            {"grant_type", "authorization_code"},  {"code", code},
            {"redirect_uri", config.redirect_uri}, {"client_id", config.client_id},
            {"code_verifier", code_verifier},
        };

        if (config.client_secret) {
            params.emplace_back("client_secret", *config.client_secret);
        }
        if (config.resource) {
            params.emplace_back("resource", *config.resource);
        }

        co_return co_await post_token_request(config.token_endpoint, params);
    }

    /**
     * @brief Refresh an access token using a refresh token.
     *
     * @param config OAuth client configuration.
     * @param refresh_token Refresh token issued by the authorization server.
     * @return A task resolving to the parsed token response.
     */
    Task<TokenResponse> refresh_token(const OAuthConfig& config, const std::string& refresh_token) {
        std::vector<std::pair<std::string, std::string>> params = {
            {"grant_type", "refresh_token"},
            {"refresh_token", refresh_token},
            {"client_id", config.client_id},
        };

        if (config.client_secret) {
            params.emplace_back("client_secret", *config.client_secret);
        }
        if (config.resource) {
            params.emplace_back("resource", *config.resource);
        }

        co_return co_await post_token_request(config.token_endpoint, params);
    }

    /**
     * @brief Fetch a JSON document from an OAuth discovery endpoint.
     *
     * @param url HTTP URL to fetch.
     * @return A task resolving to the parsed JSON body.
     */
    Task<nlohmann::json> get_json(const std::string& url) {
        auto parsed = parse_url(url);

        co_await net::post(strand_, net::use_awaitable);

        net::ip::tcp::resolver resolver(strand_);
        auto endpoints = co_await resolver.async_resolve(parsed.host, parsed.port, net::use_awaitable);

        beast::tcp_stream stream(strand_);
        stream.expires_after(std::chrono::seconds(30));
        co_await stream.async_connect(endpoints, net::use_awaitable);

        http::request<http::empty_body> req{http::verb::get, parsed.path, 11};
        req.set(http::field::host, parsed.host);
        req.set(http::field::accept, "application/json");

        stream.expires_after(std::chrono::seconds(30));
        co_await http::async_write(stream, req, net::use_awaitable);

        beast::flat_buffer buffer;
        http::response<http::string_body> res;
        co_await http::async_read(stream, buffer, res, net::use_awaitable);

        beast::error_code ec;
        stream.socket().shutdown(net::ip::tcp::socket::shutdown_both, ec);

        if (res.result_int() >= 400) {
            throw std::runtime_error("HTTP GET " + url + " failed with status " +
                                     std::to_string(res.result_int()));
        }

        auto json = nlohmann::json::parse(res.body(), nullptr, false);
        if (json.is_discarded()) {
            throw std::runtime_error("Failed to parse JSON from " + url);
        }

        co_return json;
    }

   private:
    struct ParsedUrl {
        std::string host;
        std::string port;
        std::string path;
    };

    static ParsedUrl parse_url(const std::string& url) {
        constexpr std::string_view scheme = "http://";
        if (url.rfind(std::string(scheme), 0) != 0) {
            throw std::invalid_argument("OAuth HTTP client URL must start with http://");
        }

        auto authority_and_path = url.substr(scheme.size());
        auto path_sep = authority_and_path.find('/');
        auto authority = authority_and_path.substr(0, path_sep);
        auto path = path_sep == std::string::npos ? "/" : authority_and_path.substr(path_sep);

        std::string host;
        std::string port = "80";
        auto colon = authority.find(':');
        if (colon == std::string::npos) {
            host = std::move(authority);
        } else {
            host = authority.substr(0, colon);
            port = authority.substr(colon + 1);
        }

        return {std::move(host), std::move(port), std::move(path)};
    }

    Task<TokenResponse> post_token_request(
        const std::string& token_endpoint,
        const std::vector<std::pair<std::string, std::string>>& params) {
        auto parsed = parse_url(token_endpoint);
        auto form_body = detail::build_form_body(params);

        co_await net::post(strand_, net::use_awaitable);

        net::ip::tcp::resolver resolver(strand_);
        auto endpoints = co_await resolver.async_resolve(parsed.host, parsed.port, net::use_awaitable);

        beast::tcp_stream stream(strand_);
        stream.expires_after(std::chrono::seconds(30));
        co_await stream.async_connect(endpoints, net::use_awaitable);

        http::request<http::string_body> req{http::verb::post, parsed.path, 11};
        req.set(http::field::host, parsed.host);
        req.set(http::field::content_type, "application/x-www-form-urlencoded");
        req.set(http::field::accept, "application/json");
        req.body() = std::move(form_body);
        req.prepare_payload();

        stream.expires_after(std::chrono::seconds(30));
        co_await http::async_write(stream, req, net::use_awaitable);

        beast::flat_buffer buffer;
        http::response<http::string_body> res;
        co_await http::async_read(stream, buffer, res, net::use_awaitable);

        beast::error_code ec;
        stream.socket().shutdown(net::ip::tcp::socket::shutdown_both, ec);

        if (res.result_int() >= 400) {
            throw std::runtime_error("Token request failed with status " +
                                     std::to_string(res.result_int()) + ": " + res.body());
        }

        auto response_json = nlohmann::json::parse(res.body(), nullptr, false);
        if (response_json.is_discarded()) {
            throw std::runtime_error("Failed to parse token response JSON");
        }

        co_return response_json.get<TokenResponse>();
    }

    net::strand<net::any_io_executor> strand_;
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
inline void from_json(const nlohmann::json& j, ProtectedResourceMetadata& m) {
    m.raw = j;
    if (j.contains("resource")) {
        j.at("resource").get_to(m.resource);
    }
    if (j.contains("authorization_servers")) {
        j.at("authorization_servers").get_to(m.authorization_servers);
    }
    if (j.contains("scopes_supported")) {
        m.scopes_supported = j.at("scopes_supported").get<std::vector<std::string>>();
    }
}

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
    nlohmann::json raw;                    ///< Raw source document.
};

/**
 * @brief Deserialize authorization-server metadata from JSON.
 *
 * @param j JSON metadata payload.
 * @param m Metadata structure to populate.
 */
inline void from_json(const nlohmann::json& j, AuthServerMetadata& m) {
    m.raw = j;
    if (j.contains("issuer")) {
        j.at("issuer").get_to(m.issuer);
    }
    if (j.contains("authorization_endpoint")) {
        j.at("authorization_endpoint").get_to(m.authorization_endpoint);
    }
    if (j.contains("token_endpoint")) {
        j.at("token_endpoint").get_to(m.token_endpoint);
    }
    if (j.contains("revocation_endpoint")) {
        m.revocation_endpoint = j.at("revocation_endpoint").get<std::string>();
    }
    if (j.contains("registration_endpoint")) {
        m.registration_endpoint = j.at("registration_endpoint").get<std::string>();
    }
    if (j.contains("scopes_supported")) {
        m.scopes_supported = j.at("scopes_supported").get<std::vector<std::string>>();
    }
    if (j.contains("response_types_supported")) {
        m.response_types_supported = j.at("response_types_supported").get<std::vector<std::string>>();
    }
    if (j.contains("grant_types_supported")) {
        m.grant_types_supported = j.at("grant_types_supported").get<std::vector<std::string>>();
    }
    if (j.contains("code_challenge_methods_supported")) {
        m.code_challenge_methods_supported =
            j.at("code_challenge_methods_supported").get<std::vector<std::string>>();
    }
}

template <typename T>
struct CachedEntry {
    T data;
    std::chrono::steady_clock::time_point expires_at;

    [[nodiscard]] bool is_expired() const { return std::chrono::steady_clock::now() >= expires_at; }
};

/**
 * @brief Client for OAuth protected-resource and authorization-server discovery.
 */
class OAuthDiscoveryClient {
   public:
    /**
     * @brief Construct an OAuth discovery client.
     *
     * @param http_client HTTP helper used to fetch discovery documents.
     * @param cache_ttl Time-to-live for cached discovery responses.
     */
    explicit OAuthDiscoveryClient(std::shared_ptr<OAuthHttpClient> http_client,
                                  std::chrono::seconds cache_ttl = std::chrono::seconds(300))
        : http_client_(std::move(http_client)), cache_ttl_(cache_ttl) {}

    /**
     * @brief Discover metadata for a protected resource.
     *
     * @param resource_url Resource URL whose metadata should be resolved.
     * @return A task resolving to the discovered protected-resource metadata.
     */
    Task<ProtectedResourceMetadata> discover_protected_resource(const std::string& resource_url) {
        {
            std::lock_guard lock(cache_mutex_);
            auto it = resource_cache_.find(resource_url);
            if (it != resource_cache_.end() && !it->second.is_expired()) {
                co_return it->second.data;
            }
        }

        auto parsed = parse_url_components(resource_url);
        auto base = parsed.scheme + "://" + parsed.authority;

        std::vector<std::string> urls_to_try;
        if (!parsed.path.empty() && parsed.path != "/") {
            auto path_part = parsed.path;
            if (!path_part.empty() && path_part.front() == '/') {
                path_part = path_part.substr(1);
            }
            urls_to_try.push_back(base + "/.well-known/oauth-protected-resource/" + path_part);
        }
        urls_to_try.push_back(base + "/.well-known/oauth-protected-resource");

        for (const auto& url : urls_to_try) {
            try {
                auto json = co_await http_client_->get_json(url);
                auto metadata = json.get<ProtectedResourceMetadata>();

                std::lock_guard lock(cache_mutex_);
                resource_cache_[resource_url] = {
                    metadata,
                    std::chrono::steady_clock::now() + cache_ttl_,
                };
                co_return metadata;
            } catch (...) {
                // Expected: trying next well-known URL fallback
                continue;
            }
        }

        throw std::runtime_error("Failed to discover protected resource metadata for " + resource_url);
    }

    /**
     * @brief Discover metadata for an authorization server.
     *
     * @param issuer_url Issuer URL or base URL of the authorization server.
     * @return A task resolving to the discovered authorization-server metadata.
     */
    Task<AuthServerMetadata> discover_auth_server(const std::string& issuer_url) {
        {
            std::lock_guard lock(cache_mutex_);
            auto it = auth_cache_.find(issuer_url);
            if (it != auth_cache_.end() && !it->second.is_expired()) {
                co_return it->second.data;
            }
        }

        auto parsed = parse_url_components(issuer_url);
        auto base = parsed.scheme + "://" + parsed.authority;

        std::vector<std::string> urls_to_try;
        bool has_path = !parsed.path.empty() && parsed.path != "/";

        if (has_path) {
            auto path_part = parsed.path;
            if (!path_part.empty() && path_part.front() == '/') {
                path_part = path_part.substr(1);
            }
            if (!path_part.empty() && path_part.back() == '/') {
                path_part.pop_back();
            }
            urls_to_try.push_back(base + "/.well-known/oauth-authorization-server/" + path_part);
            urls_to_try.push_back(base + "/.well-known/openid-configuration/" + path_part);
            urls_to_try.push_back(issuer_url + "/.well-known/openid-configuration");
        } else {
            urls_to_try.push_back(base + "/.well-known/oauth-authorization-server");
            urls_to_try.push_back(base + "/.well-known/openid-configuration");
        }

        for (const auto& url : urls_to_try) {
            try {
                auto json = co_await http_client_->get_json(url);
                auto metadata = json.get<AuthServerMetadata>();

                std::lock_guard lock(cache_mutex_);
                auth_cache_[issuer_url] = {
                    metadata,
                    std::chrono::steady_clock::now() + cache_ttl_,
                };
                co_return metadata;
            } catch (...) {
                // Expected: trying next well-known URL fallback
                continue;
            }
        }

        throw std::runtime_error("Failed to discover authorization server metadata for " + issuer_url);
    }

    /**
     * @brief Clear all cached discovery metadata.
     */
    void clear_cache() {
        std::lock_guard lock(cache_mutex_);
        resource_cache_.clear();
        auth_cache_.clear();
    }

   private:
    struct UrlComponents {
        std::string scheme;
        std::string authority;
        std::string path;
    };

    static UrlComponents parse_url_components(const std::string& url) {
        UrlComponents result;
        auto scheme_end = url.find("://");
        if (scheme_end == std::string::npos) {
            throw std::invalid_argument("URL missing scheme: " + url);
        }
        result.scheme = url.substr(0, scheme_end);
        auto rest = url.substr(scheme_end + 3);

        auto path_start = rest.find('/');
        if (path_start == std::string::npos) {
            result.authority = rest;
            result.path = "/";
        } else {
            result.authority = rest.substr(0, path_start);
            result.path = rest.substr(path_start);
        }
        return result;
    }

    std::shared_ptr<OAuthHttpClient> http_client_;
    std::chrono::seconds cache_ttl_;

    mutable std::mutex cache_mutex_;
    std::unordered_map<std::string, CachedEntry<ProtectedResourceMetadata>> resource_cache_;
    std::unordered_map<std::string, CachedEntry<AuthServerMetadata>> auth_cache_;
};

/// @brief Callback used to validate a bearer token extracted from request metadata.
using TokenValidator = std::function<Task<bool>(const std::string& token)>;

/**
 * @brief Create middleware that validates bearer tokens in request metadata.
 *
 * @param validator Async callback that returns true when the token is accepted.
 * @return Middleware enforcing presence and validity of `_meta.auth_token`.
 */
inline Middleware make_auth_middleware(TokenValidator validator) {
    return [validator = std::move(validator)](mcp::Context& ctx, const nlohmann::json& params,
                                              TypeErasedHandler next) -> Task<nlohmann::json> {
        std::string token;
        if (params.contains("_meta") && params["_meta"].contains("auth_token")) {
            token = params["_meta"]["auth_token"].get<std::string>();
        }

        if (token.empty()) {
            co_return nlohmann::json{
                {"content", {{{"type", "text"}, {"text", "Unauthorized: missing Bearer token"}}}},
                {"isError", true}};
        }

        bool valid = co_await validator(token);
        if (!valid) {
            co_return nlohmann::json{
                {"content", {{{"type", "text"}, {"text", "Unauthorized: invalid Bearer token"}}}},
                {"isError", true}};
        }

        co_return co_await next(ctx, params);
    };
}

/**
 * @brief Extract a bearer token from an Authorization header value.
 *
 * @param auth_header_value Header value to parse.
 * @return The token value without the `Bearer ` prefix, or an empty string on mismatch.
 */
inline std::string extract_bearer_token(std::string_view auth_header_value) {
    constexpr std::string_view prefix = "Bearer ";
    if (auth_header_value.size() > prefix.size() &&
        auth_header_value.substr(0, prefix.size()) == prefix) {
        return std::string(auth_header_value.substr(prefix.size()));
    }
    return {};
}

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
};

/**
 * @brief OAuth 2.0 implementation of the Authenticator interface.
 */
class OAuthAuthenticator : public Authenticator {
   public:
    OAuthAuthenticator(std::shared_ptr<TokenStore> token_store,
                       std::shared_ptr<OAuthHttpClient> oauth_client, OAuthConfig config,
                       std::string server_url)
        : token_store_(std::move(token_store)),
          oauth_client_(std::move(oauth_client)),
          config_(std::move(config)),
          server_url_(std::move(server_url)) {}

    [[nodiscard]] std::string get_access_token() const override {
        auto token = token_store_->load(server_url_);
        if (token) {
            return token->access_token;
        }
        return {};
    }

    Task<bool> try_refresh_token() override {
        auto stored = token_store_->load(server_url_);
        if (!stored || !stored->refresh_token) {
            co_return false;
        }

        try {
            auto new_token = co_await oauth_client_->refresh_token(config_, *stored->refresh_token);
            if (!new_token.refresh_token && stored->refresh_token) {
                new_token.refresh_token = stored->refresh_token;
            }
            token_store_->store(server_url_, std::move(new_token));
            co_return true;
        } catch (...) {
            co_return false;
        }
    }

    /// @brief Stores an initial token obtained from an explicit OAuth exchange (e.g., authorization
    /// code flow).
    /// @param token The token response to persist via the configured TokenStore.
    void store_token(TokenResponse token) { token_store_->store(server_url_, std::move(token)); }

   private:
    std::shared_ptr<TokenStore> token_store_;
    std::shared_ptr<OAuthHttpClient> oauth_client_;
    OAuthConfig config_;
    std::string server_url_;
};

/**
 * @brief Transport wrapper that injects and refreshes OAuth bearer tokens.
 *
 * @details Wraps any `ITransport` to automatically inject OAuth bearer tokens on write and handle token
 * refresh on auth failures. When the server returns error codes -32001 or -32000
 * (authentication-related), the transport calls `Authenticator::try_refresh_token()` and, if
 * successful, re-sends the last written message and reads the new response. Only one retry attempt is
 * made per `read_message()` call. Callers should ensure messages are idempotent since they may be
 * re-sent after a token refresh. `last_written_message_` stores the most recently written message for
 * potential replay.
 */
class OAuthClientTransport final : public ITransport {
   public:
    /**
     * @brief Construct an authenticated transport wrapper.
     *
     * @param inner Underlying transport used for MCP message exchange.
     * @param authenticator Authenticator used to retrieve and refresh tokens.
     */
    OAuthClientTransport(std::unique_ptr<ITransport> inner,
                         std::shared_ptr<Authenticator> authenticator)
        : inner_(std::move(inner)), authenticator_(std::move(authenticator)) {}

    /**
     * @brief Read a message from the inner transport, retrying once on authentication errors.
     * @details If the received message contains an authentication error (JSON-RPC error code -32001 or
     * -32000), attempts one token refresh via `Authenticator::try_refresh_token()`. On successful
     * refresh, replays the last written message and returns the new response. If refresh fails or a
     * second auth error is received, returns the error response as-is. If the response cannot be parsed
     * as JSON, the raw string is returned unchanged.
     * @return The (possibly retried) raw message string.
     */
    Task<std::string> read_message() override {
        auto raw = co_await inner_->read_message();
        bool retried = false;  // Retry guard: prevents re-entering the retry branch more than once

        try {
            auto json_msg = nlohmann::json::parse(raw);
            JSONRPCMessage msg = json_msg.get<JSONRPCMessage>();

            if (auto* error_resp = std::get_if<JSONRPCErrorResponse>(&msg)) {
                auto code = error_resp->error.code;
                // Error codes -32001 (authentication required) and -32000 (authentication failed)
                // are MCP standard JSON-RPC error codes for auth-related failures.
                // Attempt to refresh token and retry the original request once.
                if ((code == -32001 || code == -32000) && !retried) {
                    retried = true;
                    bool refreshed = co_await authenticator_->try_refresh_token();
                    if (refreshed && !last_written_message_.empty()) {
                        co_await write_message(last_written_message_);
                        co_return co_await inner_->read_message();
                    }
                }
            }
        } catch (...) {
            // Silently ignore JSON parse errors: returns raw message to caller unchanged.
            // This allows the client to handle non-JSON responses gracefully.
        }

        co_return raw;
    }

    /**
     * @brief Inject the current bearer token into an outgoing MCP message.
     *
     * @param message Serialized JSON-RPC request or notification.
     * @return A task that completes once the wrapped transport accepts the message.
     */
    Task<void> write_message(std::string_view message) override {
        std::string injected = std::string(message);
        try {
            auto json_msg = nlohmann::json::parse(message);
            JSONRPCMessage msg = json_msg.get<JSONRPCMessage>();

            if (std::holds_alternative<JSONRPCRequest>(msg)) {
                injected = inject_token(std::get<JSONRPCRequest>(msg));
            }
        } catch (...) {
            // Ignore parse errors, send original message.
        }

        last_written_message_ = std::string(message);
        co_await inner_->write_message(injected);
    }

    /**
     * @brief Close the wrapped transport.
     */
    void close() override { inner_->close(); }

   private:
    std::string inject_token(JSONRPCRequest request) const {
        auto token = authenticator_->get_access_token();
        if (token.empty()) {
            return nlohmann::json(request).dump();
        }

        if (!request.params) {
            request.params = nlohmann::json::object();
        }

        auto& params = *request.params;
        if (!params.is_object()) {
            params = nlohmann::json::object();
        }

        if (!params.contains("_meta")) {
            params["_meta"] = nlohmann::json::object();
        }
        params["_meta"]["auth_token"] = token;

        return nlohmann::json(request).dump();
    }

    std::unique_ptr<ITransport> inner_;
    std::shared_ptr<Authenticator> authenticator_;
    std::string last_written_message_;
};

}  // namespace mcp::auth

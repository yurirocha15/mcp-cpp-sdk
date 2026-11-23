#include <mcp/auth/oauth_discovery.hpp>
#include <mcp/core/async.hpp>

#include <chrono>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// GCC 11 SSO Coroutine Safety -- see docs/contributing.rst "Known Issues".
// Strings and string-containing protocol values that cross a suspension point
// live in shared operation state rather than directly in coroutine frames.

namespace mcp::auth {

namespace {

/// Flattens peer-controlled text -- control characters, bidi overrides and ill-formed UTF-8 -- and
/// caps it, so a value a peer chose cannot forge or reorder a line of a diagnostic. Every throw
/// site in this file that interpolates a value originating from a peer must route it through here;
/// `MetadataPolicyError` does the same inside its own constructor, so its throw sites do not repeat
/// it. This is a convention, not something the compiler checks: a new throw site that forgets it
/// re-opens the hole silently.
using detail::sanitize_for_diagnostics;

}  // namespace

void from_json(const nlohmann::json& json, ProtectedResourceMetadata& metadata) {
    metadata = ProtectedResourceMetadata{};
    metadata.raw = json;
    if (json.contains("resource")) {
        json.at("resource").get_to(metadata.resource);
    }
    if (json.contains("authorization_servers")) {
        json.at("authorization_servers").get_to(metadata.authorization_servers);
    }
    if (json.contains("scopes_supported")) {
        metadata.scopes_supported = json.at("scopes_supported").get<std::vector<std::string>>();
    }
}

void from_json(const nlohmann::json& json, AuthServerMetadata& metadata) {
    metadata = AuthServerMetadata{};
    metadata.raw = json;
    if (json.contains("issuer")) {
        json.at("issuer").get_to(metadata.issuer);
    }
    if (json.contains("authorization_endpoint")) {
        json.at("authorization_endpoint").get_to(metadata.authorization_endpoint);
    }
    if (json.contains("token_endpoint")) {
        json.at("token_endpoint").get_to(metadata.token_endpoint);
    }
    if (json.contains("revocation_endpoint")) {
        metadata.revocation_endpoint = json.at("revocation_endpoint").get<std::string>();
    }
    if (json.contains("registration_endpoint")) {
        metadata.registration_endpoint = json.at("registration_endpoint").get<std::string>();
    }
    if (json.contains("scopes_supported")) {
        metadata.scopes_supported = json.at("scopes_supported").get<std::vector<std::string>>();
    }
    if (json.contains("response_types_supported")) {
        metadata.response_types_supported =
            json.at("response_types_supported").get<std::vector<std::string>>();
    }
    if (json.contains("grant_types_supported")) {
        metadata.grant_types_supported =
            json.at("grant_types_supported").get<std::vector<std::string>>();
    }
    if (json.contains("code_challenge_methods_supported")) {
        metadata.code_challenge_methods_supported =
            json.at("code_challenge_methods_supported").get<std::vector<std::string>>();
    }
    if (json.contains("authorization_response_iss_parameter_supported")) {
        metadata.authorization_response_iss_parameter_supported =
            json.at("authorization_response_iss_parameter_supported").get<bool>();
    }
    if (json.contains("client_id_metadata_document_supported")) {
        metadata.client_id_metadata_document_supported =
            json.at("client_id_metadata_document_supported").get<bool>();
    }
    if (json.contains("token_endpoint_auth_methods_supported")) {
        metadata.token_endpoint_auth_methods_supported =
            json.at("token_endpoint_auth_methods_supported").get<std::vector<std::string>>();
    }
}

struct OAuthDiscoveryClient::Impl {
    struct UrlComponents {
        std::string scheme;
        std::string authority;
        std::string path;
    };

    template <typename Metadata>
    struct DiscoveryOperation {
        std::shared_ptr<Impl> owner;
        std::string cache_key;
        std::vector<std::string> urls;
        std::size_t next_url{0};
        nlohmann::json response;
        std::optional<Metadata> metadata;
        /// Caller's verdict, consulted before the document is cached or returned. Empty means
        /// accept everything that parsed, which is the contract of the overloads that take no
        /// acceptor.
        std::function<void(const Metadata&)> accept;
        /// Why each URL tried so far gave no document, for the error when none does.
        std::vector<std::string> misses;

        /// Records why @p url gave no document: @p failure, led by the URL unless it names it.
        void miss(const std::string& url, const std::exception& failure) {
            auto reason = sanitize_for_diagnostics(failure.what());
            const auto where = sanitize_for_diagnostics(url);
            misses.push_back(reason.find(where) == std::string::npos ? where + ": " + reason
                                                                     : std::move(reason));
        }

        /// The error for a discovery in which no URL gave a document: @p summary and the misses.
        [[nodiscard]] std::runtime_error failure(std::string summary) const {
            summary += " for " + sanitize_for_diagnostics(cache_key);
            for (std::size_t index = 0; index < misses.size(); ++index) {
                summary += index == 0 ? ": " : "; ";
                summary += misses[index];
            }
            return std::runtime_error(summary);
        }
    };

    Impl(std::shared_ptr<OAuthHttpClient> client, std::chrono::seconds ttl)
        : http_client(std::move(client)), cache_ttl(ttl) {}

    static UrlComponents parse_url_components(const std::string& url) {
        UrlComponents result;
        const auto scheme_end = url.find("://");
        if (scheme_end == std::string::npos) {
            // The authorization server identifier a protected-resource document names arrives here
            // with nothing having validated it, so this is the throw site a peer reaches first.
            throw std::invalid_argument("URL missing scheme: " + sanitize_for_diagnostics(url));
        }
        result.scheme = url.substr(0, scheme_end);
        auto rest = url.substr(scheme_end + 3);

        const auto path_start = rest.find('/');
        if (path_start == std::string::npos) {
            result.authority = std::move(rest);
            result.path = "/";
        } else {
            result.authority = rest.substr(0, path_start);
            result.path = rest.substr(path_start);
        }
        return result;
    }

    template <typename Metadata>
    static Task<Metadata> return_cached(std::shared_ptr<Metadata> metadata) {
        co_return std::move(*metadata);
    }

    /// Serve a cached document, but only if the caller still accepts it.
    ///
    /// A coroutine rather than a plain check at the call site so that a rejection surfaces from the
    /// await like every other discovery failure, instead of throwing out of the call that merely
    /// builds the awaitable.
    static Task<ProtectedResourceMetadata> return_accepted_cached(
        std::shared_ptr<ProtectedResourceMetadata> metadata, ProtectedResourceAcceptor accept) {
        if (accept) {
            accept(*metadata);
        }
        co_return std::move(*metadata);
    }

    static Task<ProtectedResourceMetadata> discover_protected_resource(
        std::shared_ptr<Impl> owner, std::string resource_url,
        const std::optional<std::string>& challenge_metadata_url,
        ProtectedResourceAcceptor accept = {}) {
        // A challenge-supplied URL is its own cache key, so a server that moves its metadata is
        // never served an entry discovered through the well-known fallback.
        auto cache_key = challenge_metadata_url.value_or(resource_url);
        {
            std::lock_guard lock(owner->cache_mutex);
            const auto iter = owner->resource_cache.find(cache_key);
            if (iter != owner->resource_cache.end() && !iter->second.is_expired()) {
                return return_accepted_cached(
                    std::make_shared<ProtectedResourceMetadata>(iter->second.data), std::move(accept));
            }
        }

        auto operation = std::make_shared<DiscoveryOperation<ProtectedResourceMetadata>>();
        operation->owner = std::move(owner);
        operation->cache_key = std::move(cache_key);
        operation->accept = std::move(accept);

        if (challenge_metadata_url) {
            // The challenge named the location; take the server at its word and try nothing else.
            operation->urls.push_back(*challenge_metadata_url);
            return run_protected_discovery(std::move(operation));
        }

        const auto parsed = parse_url_components(resource_url);
        const auto base = parsed.scheme + "://" + parsed.authority;
        if (!parsed.path.empty() && parsed.path != "/") {
            auto path_part = parsed.path;
            if (path_part.front() == '/') {
                path_part.erase(0, 1);
            }
            operation->urls.push_back(base + "/.well-known/oauth-protected-resource/" + path_part);
        }
        operation->urls.push_back(base + "/.well-known/oauth-protected-resource");
        return run_protected_discovery(std::move(operation));
    }

    static Task<AuthServerMetadata> discover_auth_server(std::shared_ptr<Impl> owner,
                                                         std::string issuer_url) {
        {
            std::lock_guard lock(owner->cache_mutex);
            const auto iter = owner->auth_cache.find(issuer_url);
            if (iter != owner->auth_cache.end() && !iter->second.is_expired()) {
                return return_cached(std::make_shared<AuthServerMetadata>(iter->second.data));
            }
        }

        const auto parsed = parse_url_components(issuer_url);
        const auto base = parsed.scheme + "://" + parsed.authority;
        auto operation = std::make_shared<DiscoveryOperation<AuthServerMetadata>>();
        operation->owner = std::move(owner);
        operation->cache_key = std::move(issuer_url);

        if (!parsed.path.empty() && parsed.path != "/") {
            auto path_part = parsed.path;
            if (path_part.front() == '/') {
                path_part.erase(0, 1);
            }
            if (!path_part.empty() && path_part.back() == '/') {
                path_part.pop_back();
            }
            operation->urls.push_back(base + "/.well-known/oauth-authorization-server/" + path_part);
            operation->urls.push_back(base + "/.well-known/openid-configuration/" + path_part);
            operation->urls.push_back(operation->cache_key + "/.well-known/openid-configuration");
        } else {
            operation->urls.push_back(base + "/.well-known/oauth-authorization-server");
            operation->urls.push_back(base + "/.well-known/openid-configuration");
        }
        return run_auth_discovery(std::move(operation));
    }

    static Task<ProtectedResourceMetadata> run_protected_discovery(
        std::shared_ptr<DiscoveryOperation<ProtectedResourceMetadata>> operation) {
        while (operation->next_url < operation->urls.size()) {
            const auto& url = operation->urls[operation->next_url++];
            try {
                operation->response = co_await operation->owner->http_client->get_json(url);
                operation->metadata = operation->response.get<ProtectedResourceMetadata>();
            } catch (const MetadataPolicyError&) {
                // A refused target is a security decision, not a candidate that missed.
                throw;
            } catch (const std::exception& error) {
                operation->miss(url, error);
                continue;
            }

            // Outside the try above, because a caller refusing a fetched and parsed document is a
            // verdict on it, not a candidate that missed: `catch (...)` would move on to the next
            // well-known URL. Before the cache write, because a refused document must never become a
            // cache entry served to a later attempt without reaching the network.
            if (operation->accept) {
                operation->accept(*operation->metadata);
            }

            {
                std::lock_guard lock(operation->owner->cache_mutex);
                operation->owner->resource_cache[operation->cache_key] = {
                    *operation->metadata,
                    std::chrono::steady_clock::now() + operation->owner->cache_ttl,
                };
            }
            co_return *operation->metadata;
        }
        throw operation->failure("Failed to discover protected resource metadata");
    }

    static Task<AuthServerMetadata> run_auth_discovery(
        std::shared_ptr<DiscoveryOperation<AuthServerMetadata>> operation) {
        while (operation->next_url < operation->urls.size()) {
            const auto& url = operation->urls[operation->next_url++];
            try {
                operation->response = co_await operation->owner->http_client->get_json(url);
                operation->metadata = operation->response.get<AuthServerMetadata>();

                std::lock_guard lock(operation->owner->cache_mutex);
                operation->owner->auth_cache[operation->cache_key] = {
                    *operation->metadata,
                    std::chrono::steady_clock::now() + operation->owner->cache_ttl,
                };
                co_return *operation->metadata;
            } catch (const MetadataPolicyError&) {
                // A refused target is a security decision, not a candidate that missed.
                throw;
            } catch (const std::exception& error) {
                operation->miss(url, error);
                continue;
            }
        }
        throw operation->failure("Failed to discover authorization server metadata");
    }

    std::shared_ptr<OAuthHttpClient> http_client;
    std::chrono::seconds cache_ttl;
    std::mutex cache_mutex;
    std::unordered_map<std::string, CachedEntry<ProtectedResourceMetadata>> resource_cache;
    std::unordered_map<std::string, CachedEntry<AuthServerMetadata>> auth_cache;
};

OAuthDiscoveryClient::OAuthDiscoveryClient(std::shared_ptr<OAuthHttpClient> http_client,
                                           std::chrono::seconds cache_ttl)
    : impl_(std::make_shared<Impl>(std::move(http_client), cache_ttl)) {}

Task<ProtectedResourceMetadata> OAuthDiscoveryClient::discover_protected_resource(
    const std::string& resource_url) {
    return Impl::discover_protected_resource(impl_, resource_url, std::nullopt);
}

Task<ProtectedResourceMetadata> OAuthDiscoveryClient::discover_protected_resource(
    const std::string& resource_url, const std::optional<std::string>& challenge_metadata_url) {
    return Impl::discover_protected_resource(impl_, resource_url, challenge_metadata_url);
}

Task<ProtectedResourceMetadata> OAuthDiscoveryClient::discover_protected_resource(
    const std::string& resource_url, const std::optional<std::string>& challenge_metadata_url,
    ProtectedResourceAcceptor accept) {
    return Impl::discover_protected_resource(impl_, resource_url, challenge_metadata_url,
                                             std::move(accept));
}

Task<AuthServerMetadata> OAuthDiscoveryClient::discover_auth_server(const std::string& issuer_url) {
    return Impl::discover_auth_server(impl_, issuer_url);
}

void OAuthDiscoveryClient::forget_protected_resource(
    const std::string& resource_url, const std::optional<std::string>& challenge_metadata_url) {
    std::lock_guard lock(impl_->cache_mutex);
    impl_->resource_cache.erase(challenge_metadata_url.value_or(resource_url));
}

void OAuthDiscoveryClient::clear_cache() {
    std::lock_guard lock(impl_->cache_mutex);
    impl_->resource_cache.clear();
    impl_->auth_cache.clear();
}

}  // namespace mcp::auth

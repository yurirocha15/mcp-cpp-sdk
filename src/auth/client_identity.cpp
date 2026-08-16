#include <mcp/auth/client_identity.hpp>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mcp::auth {

namespace {

constexpr std::string_view g_offline_access = "offline_access";

/// Split a space-delimited scope string into its individual values.
std::vector<std::string> split_scope(const std::string& scope) {
    std::vector<std::string> values;
    std::istringstream stream(scope);
    std::string value;
    while (stream >> value) {
        values.push_back(value);
    }
    return values;
}

std::string join_scope(const std::vector<std::string>& values) {
    std::string joined;
    for (const auto& value : values) {
        if (!joined.empty()) {
            joined.push_back(' ');
        }
        joined += value;
    }
    return joined;
}

bool contains(const std::vector<std::string>& values, std::string_view needle) {
    return std::find(values.begin(), values.end(), needle) != values.end();
}

}  // namespace

std::string_view describe(ClientIdentitySource source) {
    switch (source) {
        case ClientIdentitySource::pre_registered:
            return "pre-registered credentials";
        case ClientIdentitySource::client_id_metadata_document:
            return "client ID metadata document";
        case ClientIdentitySource::dynamic_registration:
            return "dynamic client registration";
    }
    return "unknown client identity source";
}

std::string_view describe(ClientIdentityDecision decision) {
    switch (decision) {
        case ClientIdentityDecision::use_pre_registered:
            return "present the application's pre-registered credentials";
        case ClientIdentityDecision::use_client_id_metadata_document:
            return "present the configured client ID metadata document URL";
        case ClientIdentityDecision::reuse_stored_registration:
            return "present the credentials already registered with this issuer";
        case ClientIdentityDecision::register_dynamically:
            return "register dynamically with this issuer";
        case ClientIdentityDecision::unavailable:
            return "no client identity is available for this issuer";
    }
    return "unknown client identity decision";
}

void to_json(nlohmann::json& json, const OAuthClientMetadata& metadata) {
    json = nlohmann::json::object();
    json["redirect_uris"] = metadata.redirect_uris;
    // SEP-837: the registration body always states what kind of application is registering.
    json["application_type"] = metadata.application_type;
    json["grant_types"] = metadata.grant_types;
    json["response_types"] = metadata.response_types;
    if (metadata.client_name) {
        json["client_name"] = *metadata.client_name;
    }
    if (metadata.client_uri) {
        json["client_uri"] = *metadata.client_uri;
    }
    if (metadata.software_id) {
        json["software_id"] = *metadata.software_id;
    }
    if (metadata.software_version) {
        json["software_version"] = *metadata.software_version;
    }
    if (metadata.scope) {
        json["scope"] = *metadata.scope;
    }
    if (metadata.token_endpoint_auth_method) {
        json["token_endpoint_auth_method"] = *metadata.token_endpoint_auth_method;
    }
}

bool OAuthClientInformation::secret_expired(std::int64_t now_seconds) const {
    // RFC 7591: zero means the secret never expires; absent means the server said nothing.
    if (!client_secret_expires_at || *client_secret_expires_at == 0) {
        return false;
    }
    return *client_secret_expires_at <= now_seconds;
}

void from_json(const nlohmann::json& json, OAuthClientInformation& information) {
    if (json.contains("client_id")) {
        json.at("client_id").get_to(information.client_id);
    }
    if (json.contains("client_secret") && !json.at("client_secret").is_null()) {
        information.client_secret = json.at("client_secret").get<std::string>();
    }
    if (json.contains("client_id_issued_at") && json.at("client_id_issued_at").is_number()) {
        information.client_id_issued_at = json.at("client_id_issued_at").get<std::int64_t>();
    }
    if (json.contains("client_secret_expires_at") && json.at("client_secret_expires_at").is_number()) {
        information.client_secret_expires_at = json.at("client_secret_expires_at").get<std::int64_t>();
    }
}

void to_json(nlohmann::json& json, const OAuthClientInformation& information) {
    json = nlohmann::json::object();
    json["client_id"] = information.client_id;
    if (information.client_secret) {
        json["client_secret"] = *information.client_secret;
    }
    if (information.client_id_issued_at) {
        json["client_id_issued_at"] = *information.client_id_issued_at;
    }
    if (information.client_secret_expires_at) {
        json["client_secret_expires_at"] = *information.client_secret_expires_at;
    }
    json["issuer"] = information.issuer;
}

struct InMemoryClientCredentialStore::Impl {
    mutable std::mutex mutex;
    std::unordered_map<std::string, OAuthClientInformation> credentials;
};

InMemoryClientCredentialStore::InMemoryClientCredentialStore() : impl_(std::make_unique<Impl>()) {}

InMemoryClientCredentialStore::~InMemoryClientCredentialStore() = default;

void InMemoryClientCredentialStore::store(const std::string& issuer,
                                          OAuthClientInformation information) {
    std::lock_guard lock(impl_->mutex);
    impl_->credentials[issuer] = std::move(information);
}

std::optional<OAuthClientInformation> InMemoryClientCredentialStore::load(
    const std::string& issuer) const {
    std::lock_guard lock(impl_->mutex);
    const auto iter = impl_->credentials.find(issuer);
    if (iter == impl_->credentials.end()) {
        return std::nullopt;
    }
    return iter->second;
}

void InMemoryClientCredentialStore::remove(const std::string& issuer) {
    std::lock_guard lock(impl_->mutex);
    impl_->credentials.erase(issuer);
}

ClientIdentityDecision select_client_identity(const ClientIdentityConfig& config,
                                              const ClientIdentityServerFacts& server,
                                              const std::optional<OAuthClientInformation>& stored) {
    // Injected credentials are terminal. Falling back to registration here would swap the client
    // the application chose for one the authorization server minted, without telling anybody.
    if (config.pre_registered && !config.pre_registered->client_id.empty()) {
        // Credentials that name their issuer are bound to it: presenting the secret the application
        // holds for one authorization server to a different one is credential misbinding, and the
        // terminal rule above means the answer is "no identity", never a silent registration.
        const auto& expected_issuer = config.pre_registered->issuer;
        if (!expected_issuer.empty() && expected_issuer != server.issuer) {
            return ClientIdentityDecision::unavailable;
        }
        return ClientIdentityDecision::use_pre_registered;
    }

    if (config.client_metadata_url && !config.client_metadata_url->empty() &&
        server.client_id_metadata_document_supported) {
        return ClientIdentityDecision::use_client_id_metadata_document;
    }

    // A stored entry is only usable when its recorded issuer is the issuer being contacted. An
    // authorization server change therefore falls straight through to a fresh registration.
    if (stored && !stored->client_id.empty() && stored->issuer == server.issuer &&
        !server.issuer.empty()) {
        return ClientIdentityDecision::reuse_stored_registration;
    }

    if (server.registration_endpoint && !server.registration_endpoint->empty()) {
        return ClientIdentityDecision::register_dynamically;
    }

    return ClientIdentityDecision::unavailable;
}

nlohmann::json build_registration_request(const OAuthClientMetadata& metadata,
                                          const ClientIdentityServerFacts& server) {
    auto body = nlohmann::json(metadata);

    // Offline access is only requested when the server published it; asking for an unpublished
    // scope invites an outright rejection of the whole registration.
    if (contains(server.scopes_supported, g_offline_access)) {
        auto scopes = metadata.scope ? split_scope(*metadata.scope) : std::vector<std::string>{};
        if (!contains(scopes, g_offline_access)) {
            scopes.emplace_back(g_offline_access);
        }
        body["scope"] = join_scope(scopes);
    }
    return body;
}

}  // namespace mcp::auth

#include <mcp/auth/oauth.hpp>
#include <mcp/core/async.hpp>

#include "mcp/detail/protocol/middleware_rejection.hpp"
#include "mcp/detail/text/encoding.hpp"
#include "mcp/detail/text/secure_random.hpp"

#include <mcp/core/constants.hpp>

#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

// GCC 11 SSO Coroutine Safety -- see docs/contributing.rst "Known Issues".
// Strings and string-containing protocol values that cross a suspension point
// live in shared operation state rather than directly in coroutine frames.

namespace mcp::auth {

PkcePair generate_pkce_pair(std::size_t verifier_length) {
    if (verifier_length < mcp::constants::oauth::g_MIN_VERIFIER_LENGTH ||
        verifier_length > mcp::constants::oauth::g_MAX_VERIFIER_LENGTH) {
        throw std::invalid_argument("PKCE verifier length must be 43-128 characters");
    }

    PkcePair pair;
    pair.code_verifier = mcp::detail::generate_random_string(verifier_length);
    pair.challenge_method = "S256";

    pair.code_challenge = mcp::detail::base64url_encode(
        mcp::detail::digest_bytes(mcp::detail::sha256(pair.code_verifier)));
    return pair;
}

bool TokenResponse::is_expired(int margin) const {
    if (!expires_in.has_value()) {
        return false;
    }
    const auto expiry = received_at + std::chrono::seconds(*expires_in) - std::chrono::seconds(margin);
    return std::chrono::steady_clock::now() >= expiry;
}

void from_json(const nlohmann::json& json, TokenResponse& token) {
    token = TokenResponse{};
    json.at("access_token").get_to(token.access_token);
    token.token_type = json.value("token_type", "Bearer");
    if (json.contains("refresh_token")) {
        token.refresh_token = json.at("refresh_token").get<std::string>();
    }
    if (json.contains("expires_in")) {
        token.expires_in = json.at("expires_in").get<int>();
    }
    if (json.contains("scope")) {
        token.scope = json.at("scope").get<std::string>();
    }
    token.received_at = std::chrono::steady_clock::now();
}

void to_json(nlohmann::json& json, const TokenResponse& token) {
    json = nlohmann::json{{"access_token", token.access_token}, {"token_type", token.token_type}};
    if (token.refresh_token) {
        json["refresh_token"] = *token.refresh_token;
    }
    if (token.expires_in) {
        json["expires_in"] = *token.expires_in;
    }
    if (token.scope) {
        json["scope"] = *token.scope;
    }
}

struct InMemoryTokenStore::Impl {
    mutable std::mutex mutex;
    std::unordered_map<std::string, TokenResponse> tokens;
};

InMemoryTokenStore::InMemoryTokenStore() : impl_(std::make_unique<Impl>()) {}

InMemoryTokenStore::~InMemoryTokenStore() = default;

void InMemoryTokenStore::store(const std::string& server_url, TokenResponse token) {
    std::lock_guard lock(impl_->mutex);
    impl_->tokens[server_url] = std::move(token);
}

std::optional<TokenResponse> InMemoryTokenStore::load(const std::string& server_url) const {
    std::lock_guard lock(impl_->mutex);
    const auto iter = impl_->tokens.find(server_url);
    if (iter == impl_->tokens.end()) {
        return std::nullopt;
    }
    return iter->second;
}

void InMemoryTokenStore::remove(const std::string& server_url) {
    std::lock_guard lock(impl_->mutex);
    impl_->tokens.erase(server_url);
}

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

namespace {

struct MiddlewareInvocation {
    TokenValidator validator;
    mcp::Context* context;
    nlohmann::json params;
    TypeErasedHandler next;
    std::string token;
};

Task<nlohmann::json> invoke_auth_middleware(std::shared_ptr<MiddlewareInvocation> invocation) {
    // A tool call reports the refusal as a tool error result; other requests answer it as a
    // JSON-RPC error. See MiddlewareRejection.
    if (invocation->token.empty()) {
        throw mcp::detail::MiddlewareRejection("Unauthorized: missing Bearer token");
    }
    // Awaited into a local, never in the condition: see "GCC 12 co_await in a condition" in
    // docs/contributing.rst.
    const bool valid = co_await invocation->validator(invocation->token);
    if (!valid) {
        throw mcp::detail::MiddlewareRejection("Unauthorized: invalid Bearer token");
    }
    co_return co_await invocation->next(*invocation->context, invocation->params);
}

}  // namespace

Middleware make_auth_middleware(TokenValidator validator) {
    return [validator = std::move(validator)](mcp::Context& context, const nlohmann::json& params,
                                              TypeErasedHandler next) -> Task<nlohmann::json> {
        auto invocation = std::make_shared<MiddlewareInvocation>();
        invocation->validator = validator;
        invocation->context = &context;
        invocation->params = params;
        invocation->next = std::move(next);
        if (params.contains("_meta") && params["_meta"].contains("auth_token")) {
            invocation->token = params["_meta"]["auth_token"].get<std::string>();
        }
        return invoke_auth_middleware(std::move(invocation));
    };
}

std::string extract_bearer_token(std::string_view auth_header_value) {
    constexpr std::string_view prefix = "Bearer ";
    if (auth_header_value.size() > prefix.size() &&
        auth_header_value.substr(0, prefix.size()) == prefix) {
        return std::string(auth_header_value.substr(prefix.size()));
    }
    return {};
}

Task<bool> Authenticator::try_handle_challenge(const std::string& www_authenticate) {
    static_cast<void>(www_authenticate);
    co_return false;
}

}  // namespace mcp::auth

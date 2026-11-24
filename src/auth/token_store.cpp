#include <mcp/auth/token_store.hpp>

#include <chrono>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

namespace mcp::auth {

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
    std::scoped_lock lock(impl_->mutex);
    impl_->tokens[server_url] = std::move(token);
}

std::optional<TokenResponse> InMemoryTokenStore::load(const std::string& server_url) const {
    std::scoped_lock lock(impl_->mutex);
    const auto iter = impl_->tokens.find(server_url);
    if (iter == impl_->tokens.end()) {
        return std::nullopt;
    }
    return iter->second;
}

void InMemoryTokenStore::remove(const std::string& server_url) {
    std::scoped_lock lock(impl_->mutex);
    impl_->tokens.erase(server_url);
}

}  // namespace mcp::auth

#include <mcp/auth/enterprise_authorization.hpp>
#include <mcp/core/async.hpp>

#include "grant_flow.hpp"
#include "mcp/detail/constants.hpp"
#include "mcp/detail/text/encoding.hpp"

#include <mcp/core/constants.hpp>
#include <mcp/protocol/extensions.hpp>
#include <mcp/server/server.hpp>

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace mcp::auth {

namespace {

/// The metadata the IdP's client authentication is built against: only its token endpoint is known.
AuthServerMetadata idp_metadata(const std::string& token_endpoint) {
    AuthServerMetadata metadata;
    metadata.token_endpoint = token_endpoint;
    metadata.raw = nlohmann::json::object();
    return metadata;
}

class EnterpriseFlow final : public detail::GrantFlow {
   public:
    EnterpriseFlow(const boost::asio::any_io_executor& executor, std::shared_ptr<TokenStore> store,
                   EnterpriseAuthorizationConfig config, IdentityAssertionProvider provider)
        : GrantFlow(executor, std::move(store), config.server_url, config.policy, config.host_resolver,
                    config.tls, config.proxy, config.scope),
          idp_token_endpoint_(std::move(config.idp_token_endpoint)),
          idp_client_(std::move(config.idp_client)),
          client_(std::move(config.client)),
          provider_(std::move(provider)) {
        if (!idp_client_.method) {
            if (idp_client_.private_key_jwt) {
                idp_client_.method = "private_key_jwt";
            } else if (idp_client_.client_secret) {
                idp_client_.method = "client_secret_post";
            } else {
                idp_client_.method = "none";
            }
        }
        if (!client_.method && !client_.private_key_jwt && !client_.client_secret) {
            client_.method = "none";
        }
    }

   protected:
    /// The SHA-256 of the current identity assertion, so a token is reused only for the identity it
    /// was issued for.
    Task<detail::GrantIdentity> identity() override {
        auto assertion = co_await provider_();
        auto text = mcp::detail::hex_encode(mcp::detail::digest_bytes(
            mcp::detail::sha256(assertion.token_type + '\n' + assertion.token)));
        co_return detail::GrantIdentity{.key = std::move(text), .state = std::move(assertion)};
    }

    void check_server(const AuthServerMetadata& server) const override {
        // The profile list is how a server says it supports the grant, but not every server
        // publishes it. A list that is there and omits the profile refuses it.
        const auto profiles = server.raw.is_object()
                                  ? server.raw.find("authorization_grant_profiles_supported")
                                  : server.raw.end();
        if (profiles == server.raw.end() || !profiles->is_array()) {
            return;
        }
        const bool listed =
            std::any_of(profiles->begin(), profiles->end(), [](const nlohmann::json& profile) {
                return profile.is_string() &&
                       profile.get_ref<const std::string&>() == mcp::constants::oauth::g_ID_JAG_PROFILE;
            });
        if (!listed) {
            throw std::runtime_error(
                "The authorization server's authorization_grant_profiles_supported does not list the "
                "ID-JAG profile");
        }
    }

    Task<TokenResponse> request_token(std::shared_ptr<OAuthHttpClient> http_client,
                                      detail::GrantTarget target) override {
        // The assertion read while naming the identity is the one exchanged.
        const auto* held = std::any_cast<IdentityAssertion>(&target.identity.state);
        IdentityAssertion assertion;
        if (held != nullptr) {
            assertion = *held;
        } else {
            assertion = co_await provider_();
        }
        if (assertion.token.empty()) {
            throw std::runtime_error("The identity assertion provider returned no token");
        }

        // Step one, at the IdP: the identity assertion for an ID-JAG naming this authorization
        // server as its audience and the MCP server as its resource.
        KeyValuePairList exchange = {
            {"grant_type", std::string(mcp::constants::oauth::g_TOKEN_EXCHANGE_GRANT)},
            {"requested_token_type", std::string(mcp::constants::oauth::g_ID_JAG_TOKEN_TYPE)},
            {"audience", target.server.issuer},
            {"resource", target.resource.resource},
            {"subject_token", assertion.token},
            {"subject_token_type", assertion.token_type},
        };
        if (target.scope && !target.scope->empty()) {
            exchange.emplace_back("scope", *target.scope);
        }
        const auto idp_authorization =
            authenticate_token_request(idp_client_, idp_metadata(idp_token_endpoint_), exchange, true);
        const auto exchanged =
            co_await http_client->post_form(idp_token_endpoint_, exchange, idp_authorization);
        const auto issued = exchanged.find("issued_token_type");
        const auto grant = exchanged.find("access_token");
        if (issued == exchanged.end() || !issued->is_string() ||
            issued->get_ref<const std::string&>() != mcp::constants::oauth::g_ID_JAG_TOKEN_TYPE ||
            grant == exchanged.end() || !grant->is_string() ||
            grant->get_ref<const std::string&>().empty()) {
            throw std::runtime_error("The identity provider did not issue an ID-JAG");
        }

        // Step two, at the authorization server: the ID-JAG as a JWT bearer assertion.
        KeyValuePairList request = {
            {"grant_type", std::string(mcp::constants::oauth::g_JWT_BEARER_GRANT)},
            {"assertion", grant->get<std::string>()},
        };
        const auto authorization = authenticate_token_request(client_, target.server, request, true);
        const auto reply =
            co_await http_client->post_form(target.server.token_endpoint, request, authorization);
        auto token = reply.get<TokenResponse>();
        detail::require_bearer(token);
        co_return token;
    }

   private:
    std::string idp_token_endpoint_;
    ClientAuthentication idp_client_;
    ClientAuthentication client_;
    IdentityAssertionProvider provider_;
};

}  // namespace

struct EnterpriseAuthorizationAuthenticator::Impl {
    std::shared_ptr<EnterpriseFlow> flow;
};

EnterpriseAuthorizationAuthenticator::EnterpriseAuthorizationAuthenticator(
    const Executor& executor, std::shared_ptr<TokenStore> token_store,
    EnterpriseAuthorizationConfig config, IdentityAssertionProvider provider)
    : impl_(std::make_shared<Impl>()) {
    if (!token_store || !provider) {
        throw std::invalid_argument(
            "EnterpriseAuthorizationAuthenticator requires a token store and an identity assertion "
            "provider");
    }
    if (config.server_url.empty() || config.idp_token_endpoint.empty() ||
        config.client.client_id.empty() || config.idp_client.client_id.empty()) {
        throw std::invalid_argument(
            "EnterpriseAuthorizationConfig needs a server_url, an idp_token_endpoint and client "
            "identifiers for the IdP and the authorization server");
    }
    impl_->flow = std::make_shared<EnterpriseFlow>(executor.get(), std::move(token_store),
                                                   std::move(config), std::move(provider));
}

EnterpriseAuthorizationAuthenticator::~EnterpriseAuthorizationAuthenticator() = default;

std::string EnterpriseAuthorizationAuthenticator::get_access_token() const {
    return impl_->flow->access_token();
}

Task<bool> EnterpriseAuthorizationAuthenticator::try_refresh_token() { return impl_->flow->renew(); }

Task<bool> EnterpriseAuthorizationAuthenticator::try_handle_challenge(
    const std::string& www_authenticate) {
    return impl_->flow->handle_challenge(www_authenticate);
}

Task<bool> EnterpriseAuthorizationAuthenticator::authorize() { return impl_->flow->authorize(); }

void EnterpriseAuthorizationAuthenticator::cancel_pending() { impl_->flow->cancel_pending(); }

void EnterpriseAuthorizationAuthenticator::close() { impl_->flow->close(); }

void declare_enterprise_authorization_support(ClientCapabilities& capabilities) {
    declare_extension(capabilities, mcp::constants::oauth::g_ENTERPRISE_AUTHORIZATION_EXTENSION_ID);
}

void declare_enterprise_authorization_support(Server& server) {
    server.declare_extension(
        std::string(mcp::constants::oauth::g_ENTERPRISE_AUTHORIZATION_EXTENSION_ID));
}

}  // namespace mcp::auth

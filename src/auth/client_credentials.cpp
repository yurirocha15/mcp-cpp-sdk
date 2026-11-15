#include <mcp/auth/client_credentials.hpp>

#include "grant_flow.hpp"

#include <mcp/core/constants.hpp>
#include <mcp/protocol/extensions.hpp>

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace oauth = mcp::constants::oauth;

namespace mcp::auth {

namespace {

class CredentialsFlow final : public detail::GrantFlow {
   public:
    CredentialsFlow(const boost::asio::any_io_executor& executor, std::shared_ptr<TokenStore> store,
                    ClientCredentialsConfig config)
        : GrantFlow(executor, std::move(store), config.server_url, config.policy, config.host_resolver,
                    config.scope),
          client_(std::move(config.client)) {}

   protected:
    void check_server(const AuthServerMetadata& server) const override {
        // RFC 8414 defaults an absent list to the interactive grants, and servers that omit it
        // still issue client credentials tokens, so only a list that omits the grant refuses it.
        if (server.grant_types_supported &&
            std::find(server.grant_types_supported->begin(), server.grant_types_supported->end(),
                      "client_credentials") == server.grant_types_supported->end()) {
            throw std::runtime_error(
                "The authorization server does not list the client_credentials grant");
        }
    }

    Task<TokenResponse> request_token(std::shared_ptr<OAuthHttpClient> http_client,
                                      detail::GrantTarget target) override {
        KeyValuePairList params = {{"grant_type", "client_credentials"}};
        if (target.scope && !target.scope->empty()) {
            params.emplace_back("scope", *target.scope);
        }
        // RFC 8707: the token is for the MCP server the metadata identified.
        params.emplace_back("resource", target.resource.resource);
        const auto authorization = authenticate_token_request(client_, target.server, params);
        const auto reply =
            co_await http_client->post_form(target.server.token_endpoint, params, authorization);
        auto token = reply.get<TokenResponse>();
        detail::require_bearer(token);
        co_return token;
    }

   private:
    ClientAuthentication client_;
};

}  // namespace

struct ClientCredentialsAuthenticator::Impl {
    std::shared_ptr<CredentialsFlow> flow;
};

ClientCredentialsAuthenticator::ClientCredentialsAuthenticator(
    const boost::asio::any_io_executor& executor, std::shared_ptr<TokenStore> token_store,
    ClientCredentialsConfig config)
    : impl_(std::make_shared<Impl>()) {
    if (!token_store) {
        throw std::invalid_argument("ClientCredentialsAuthenticator requires a token store");
    }
    if (config.server_url.empty()) {
        throw std::invalid_argument("ClientCredentialsConfig needs a server_url");
    }
    if (config.client.client_id.empty() ||
        (!config.client.client_secret && !config.client.private_key_jwt)) {
        throw std::invalid_argument(
            "ClientCredentialsConfig needs a client_id and a client secret or a private key");
    }
    if (config.client.method && *config.client.method == "none") {
        throw std::invalid_argument("The client credentials grant needs client authentication");
    }
    impl_->flow =
        std::make_shared<CredentialsFlow>(executor, std::move(token_store), std::move(config));
}

ClientCredentialsAuthenticator::~ClientCredentialsAuthenticator() = default;

std::string ClientCredentialsAuthenticator::get_access_token() const {
    return impl_->flow->access_token();
}

Task<bool> ClientCredentialsAuthenticator::try_refresh_token() { return impl_->flow->renew(); }

Task<bool> ClientCredentialsAuthenticator::try_handle_challenge(const std::string& www_authenticate) {
    return impl_->flow->handle_challenge(www_authenticate);
}

Task<bool> ClientCredentialsAuthenticator::authorize() { return impl_->flow->authorize(); }

void ClientCredentialsAuthenticator::cancel_pending() { impl_->flow->cancel_pending(); }

void ClientCredentialsAuthenticator::close() { impl_->flow->close(); }

void declare_client_credentials_support(ClientCapabilities& capabilities) {
    declare_extension(capabilities, oauth::g_CLIENT_CREDENTIALS_EXTENSION_ID);
}

void declare_client_credentials_support(Server& server) {
    server.declare_extension(std::string(oauth::g_CLIENT_CREDENTIALS_EXTENSION_ID));
}

}  // namespace mcp::auth

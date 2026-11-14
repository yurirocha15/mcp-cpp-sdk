/**
 * @file auth_enterprise_authorization_test.cpp
 * @brief The enterprise-managed authorization flow (token exchange at the IdP, then a JWT bearer
 *        grant at the resource authorization server) against a loopback IdP and authorization server.
 */

#include "../support/jwt_test_keys.hpp"
#include "../support/loopback_server.hpp"

#include <gtest/gtest.h>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <functional>
#include <mcp/auth/enterprise_authorization.hpp>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

namespace asio = boost::asio;
namespace http = boost::beast::http;
using json = nlohmann::json;
using mcp_test::form_has;
using mcp_test::form_value;
using mcp_test::json_response;
using mcp_test::status_response;

namespace {

constexpr const char* g_ID_JAG_TYPE = "urn:ietf:params:oauth:token-type:id-jag";

mcp::auth::MetadataFetchPolicy loopback_policy(const std::string& origin) {
    mcp::auth::MetadataFetchPolicy policy;
    policy.allowed_origins.push_back(origin);
    policy.allow_plain_http_loopback = true;
    return policy;
}

/// One loopback origin that plays the IdP (`/idp/token`), the MCP server's authorization server
/// (`/token`, `/.well-known/oauth-authorization-server`) and its protected resource metadata.
struct Fixture {
    asio::io_context io_ctx;
    mcp_test::LoopbackServer server{io_ctx};
    std::string base = server.base_url();
    json auth_server_extra = {
        {"authorization_grant_profiles_supported",
         json::array({"urn:ietf:params:oauth:grant-profile:id-jag"})},
        {"token_endpoint_auth_methods_supported", json::array({"client_secret_basic", "none"})}};
    json idp_reply = {{"issued_token_type", g_ID_JAG_TYPE},
                      {"access_token", "the-id-jag"},
                      {"token_type", "N_A"},
                      {"scope", "chat.read chat.history"},
                      {"expires_in", 300}};
    json token_reply = {{"token_type", "Bearer"},
                        {"access_token", "2YotnFZFEjr1zCsicMWpAA"},
                        {"expires_in", 86400}};
    int provider_calls = 0;
    /// Replaces the assertion passed to run() while set, as when the host switches users.
    std::optional<mcp::auth::IdentityAssertion> switched_assertion;

    void serve(int budget) {
        server.set_handler([this](const http::request<http::string_body>& request) {
            const std::string target(request.target());
            if (target == "/prm" || target == "/.well-known/oauth-protected-resource/mcp") {
                return json_response(
                    {{"resource", base + "/mcp"},
                     {"authorization_servers", json::array({base})},
                     {"scopes_supported", json::array({"chat.read", "chat.history"})}});
            }
            if (target == "/.well-known/oauth-authorization-server") {
                json metadata = {{"issuer", base},
                                 {"authorization_endpoint", base + "/authorize"},
                                 {"token_endpoint", base + "/token"}};
                metadata.update(auth_server_extra);
                return json_response(metadata);
            }
            if (target == "/idp/token") {
                return json_response(idp_reply);
            }
            if (target == "/token") {
                return json_response(token_reply);
            }
            return status_response(http::status::not_found);
        });
        asio::co_spawn(io_ctx, server.serve(budget), asio::detached);
    }

    [[nodiscard]] std::string challenge() const {
        return R"(Bearer resource_metadata=")" + base + R"(/prm")";
    }

    mcp::auth::EnterpriseAuthorizationConfig config() const {
        mcp::auth::EnterpriseAuthorizationConfig config;
        config.server_url = base + "/mcp";
        config.idp_token_endpoint = base + "/idp/token";
        config.idp_client.client_id = "idp-client";
        config.idp_client.client_secret = "idp-secret";
        config.client.client_id = "mcp-client";
        config.client.client_secret = "as-secret";
        config.policy = loopback_policy(base);
        return config;
    }

    void run(const mcp::auth::EnterpriseAuthorizationConfig& config,
             mcp::auth::IdentityAssertion assertion,
             std::function<mcp::Task<void>(mcp::auth::EnterpriseAuthorizationAuthenticator&)> body) {
        auto store = std::make_shared<mcp::auth::InMemoryTokenStore>();
        asio::co_spawn(
            io_ctx,
            [&, store, assertion]() -> mcp::Task<void> {
                mcp::auth::EnterpriseAuthorizationAuthenticator authenticator(
                    io_ctx.get_executor(), store, config,
                    [this, assertion]() -> mcp::Task<mcp::auth::IdentityAssertion> {
                        ++provider_calls;
                        co_return switched_assertion ? *switched_assertion : assertion;
                    });
                try {
                    co_await body(authenticator);
                } catch (const std::exception& error) {
                    ADD_FAILURE() << "unexpected exception: " << error.what();
                }
                server.close();
            },
            asio::detached);
        io_ctx.run();
    }
};

mcp::auth::IdentityAssertion id_token() {
    return {"the-id-token", std::string(mcp::auth::g_ID_TOKEN_TYPE)};
}

TEST(EnterpriseAuthorizationTest, ExchangesTheIdentityAssertionForAnIdJagAndThenForAnAccessToken) {
    Fixture fixture;
    fixture.serve(4);
    fixture.run(fixture.config(), id_token(), [&](auto& authenticator) -> mcp::Task<void> {
        EXPECT_TRUE(co_await authenticator.try_handle_challenge(fixture.challenge()));
        EXPECT_EQ(authenticator.get_access_token(), "2YotnFZFEjr1zCsicMWpAA");
    });

    ASSERT_EQ(fixture.server.targets().size(), 4U);
    EXPECT_EQ(fixture.server.targets()[0], "/prm");
    EXPECT_EQ(fixture.server.targets()[1], "/.well-known/oauth-authorization-server");
    EXPECT_EQ(fixture.server.targets()[2], "/idp/token");
    EXPECT_EQ(fixture.server.targets()[3], "/token");
    EXPECT_EQ(fixture.provider_calls, 1);

    // Step one, at the IdP: the shape of the extension's token exchange example.
    const auto& exchange = fixture.server.bodies()[2];
    EXPECT_EQ(form_value(exchange, "grant_type"), "urn:ietf:params:oauth:grant-type:token-exchange");
    EXPECT_EQ(form_value(exchange, "requested_token_type"), g_ID_JAG_TYPE);
    EXPECT_EQ(form_value(exchange, "audience"), fixture.base);
    EXPECT_EQ(form_value(exchange, "resource"), fixture.base + "/mcp");
    EXPECT_EQ(form_value(exchange, "scope"), "chat.read chat.history");
    EXPECT_EQ(form_value(exchange, "subject_token"), "the-id-token");
    EXPECT_EQ(form_value(exchange, "subject_token_type"), "urn:ietf:params:oauth:token-type:id_token");
    EXPECT_EQ(form_value(exchange, "client_id"), "idp-client");
    EXPECT_EQ(form_value(exchange, "client_secret"), "idp-secret");
    EXPECT_TRUE(fixture.server.authorizations()[2].empty());

    // Step two, at the authorization server: the ID-JAG as a JWT bearer assertion.
    const auto& grant = fixture.server.bodies()[3];
    EXPECT_EQ(form_value(grant, "grant_type"), "urn:ietf:params:oauth:grant-type:jwt-bearer");
    EXPECT_EQ(form_value(grant, "assertion"), "the-id-jag");
    // client_secret_basic carries the credentials in the header alone.
    EXPECT_FALSE(form_has(grant, "client_id"));
    EXPECT_FALSE(form_has(grant, "client_secret"));
    EXPECT_EQ(fixture.server.authorizations()[3], "Basic bWNwLWNsaWVudDphcy1zZWNyZXQ=");
}

TEST(EnterpriseAuthorizationTest, ASamlRefreshTokenIsPassedWithItsTokenType) {
    Fixture fixture;
    fixture.serve(4);
    fixture.run(fixture.config(), {"saml-refresh", std::string(mcp::auth::g_REFRESH_TOKEN_TYPE)},
                [&](auto& authenticator) -> mcp::Task<void> {
                    EXPECT_TRUE(co_await authenticator.try_handle_challenge(fixture.challenge()));
                });
    EXPECT_EQ(form_value(fixture.server.bodies()[2], "subject_token"), "saml-refresh");
    EXPECT_EQ(form_value(fixture.server.bodies()[2], "subject_token_type"),
              "urn:ietf:params:oauth:token-type:refresh_token");
}

TEST(EnterpriseAuthorizationTest, ALocalScopeReplacesTheResourcesScopes) {
    Fixture fixture;
    fixture.serve(4);
    auto config = fixture.config();
    config.scope = "chat.read";
    fixture.run(config, id_token(), [&](auto& authenticator) -> mcp::Task<void> {
        EXPECT_TRUE(co_await authenticator.try_handle_challenge(fixture.challenge()));
    });
    EXPECT_EQ(form_value(fixture.server.bodies()[2], "scope"), "chat.read");
}

TEST(EnterpriseAuthorizationTest, AProfileListThatOmitsIdJagIsRefusedBeforeTheProviderRuns) {
    Fixture fixture;
    fixture.auth_server_extra = {{"authorization_grant_profiles_supported",
                                  json::array({"urn:ietf:params:oauth:grant-profile:other"})}};
    fixture.serve(3);
    std::string message;
    fixture.run(fixture.config(), id_token(), [&](auto& authenticator) -> mcp::Task<void> {
        try {
            (void)co_await authenticator.try_handle_challenge(fixture.challenge());
        } catch (const std::runtime_error& error) {
            message = error.what();
        }
    });
    EXPECT_NE(message.find("ID-JAG"), std::string::npos) << message;
    EXPECT_EQ(fixture.provider_calls, 0);
    EXPECT_EQ(fixture.server.targets().size(), 2U);
}

TEST(EnterpriseAuthorizationTest, AServerThatPublishesNoProfileListIsTried) {
    Fixture fixture;
    fixture.auth_server_extra = {
        {"token_endpoint_auth_methods_supported", json::array({"client_secret_basic"})}};
    fixture.serve(4);
    fixture.run(fixture.config(), id_token(), [&](auto& authenticator) -> mcp::Task<void> {
        EXPECT_TRUE(co_await authenticator.try_handle_challenge(fixture.challenge()));
    });
    EXPECT_EQ(fixture.provider_calls, 1);
    EXPECT_EQ(fixture.server.targets().size(), 4U);
}

TEST(EnterpriseAuthorizationTest, ATokenThatIsNotAnIdJagIsNotPresented) {
    Fixture fixture;
    fixture.idp_reply = {{"issued_token_type", "urn:ietf:params:oauth:token-type:access_token"},
                         {"access_token", "plain"},
                         {"token_type", "Bearer"}};
    fixture.serve(4);
    bool refused = false;
    fixture.run(fixture.config(), id_token(), [&](auto& authenticator) -> mcp::Task<void> {
        try {
            (void)co_await authenticator.try_handle_challenge(fixture.challenge());
        } catch (const std::runtime_error&) {
            refused = true;
        }
        EXPECT_TRUE(authenticator.get_access_token().empty());
    });
    EXPECT_TRUE(refused);
    // The authorization server's token endpoint was never reached.
    EXPECT_EQ(fixture.server.targets().size(), 3U);
}

TEST(EnterpriseAuthorizationTest, AnIdpOriginOutsideTheFetchPolicyIsRefused) {
    Fixture fixture;
    fixture.serve(3);
    auto config = fixture.config();
    config.idp_token_endpoint = "http://127.0.0.1:1/token";
    bool refused = false;
    fixture.run(config, id_token(), [&](auto& authenticator) -> mcp::Task<void> {
        try {
            (void)co_await authenticator.try_handle_challenge(fixture.challenge());
        } catch (const mcp::auth::MetadataPolicyError&) {
            refused = true;
        }
    });
    EXPECT_TRUE(refused);
    EXPECT_EQ(fixture.server.targets().size(), 2U);
}

TEST(EnterpriseAuthorizationTest, AnEmptyIdentityAssertionIsRefused) {
    Fixture fixture;
    fixture.serve(3);
    bool refused = false;
    fixture.run(fixture.config(), {"", std::string(mcp::auth::g_ID_TOKEN_TYPE)},
                [&](auto& authenticator) -> mcp::Task<void> {
                    try {
                        (void)co_await authenticator.try_handle_challenge(fixture.challenge());
                    } catch (const std::runtime_error&) {
                        refused = true;
                    }
                });
    EXPECT_TRUE(refused);
    EXPECT_EQ(fixture.server.targets().size(), 2U);
}

TEST(EnterpriseAuthorizationTest, APublicClientSendsOnlyItsClientId) {
    Fixture fixture;
    fixture.serve(4);
    auto config = fixture.config();
    // A client ID metadata document URL as the client identifier, without credentials.
    config.client.client_id = "https://client.example.com/client.json";
    config.client.client_secret.reset();
    fixture.run(config, id_token(), [&](auto& authenticator) -> mcp::Task<void> {
        EXPECT_TRUE(co_await authenticator.try_handle_challenge(fixture.challenge()));
    });
    const auto& grant = fixture.server.bodies()[3];
    EXPECT_EQ(form_value(grant, "client_id"), "https://client.example.com/client.json");
    EXPECT_FALSE(form_has(grant, "client_secret"));
    EXPECT_FALSE(form_has(grant, "client_assertion"));
    EXPECT_TRUE(fixture.server.authorizations()[3].empty());
}

TEST(EnterpriseAuthorizationTest, APrivateKeyAuthenticatesTheClientAtTheAuthorizationServer) {
    Fixture fixture;
    fixture.auth_server_extra = {
        {"authorization_grant_profiles_supported",
         json::array({"urn:ietf:params:oauth:grant-profile:id-jag"})},
        {"token_endpoint_auth_methods_supported", json::array({"private_key_jwt"})}};
    fixture.serve(4);
    auto config = fixture.config();
    config.client.client_secret.reset();
    mcp::auth::PrivateKeyJwtConfig jwt;
    jwt.private_key_pem = mcp_test::private_key_pem(mcp_test::shared_ec_key());
    config.client.private_key_jwt = jwt;
    fixture.run(config, id_token(), [&](auto& authenticator) -> mcp::Task<void> {
        EXPECT_TRUE(co_await authenticator.try_handle_challenge(fixture.challenge()));
    });

    const auto& grant = fixture.server.bodies()[3];
    EXPECT_EQ(form_value(grant, "grant_type"), "urn:ietf:params:oauth:grant-type:jwt-bearer");
    EXPECT_EQ(form_value(grant, "client_id"), "mcp-client");
    const auto decoded =
        mcp_test::verify_jwt(form_value(grant, "client_assertion"), mcp_test::shared_ec_key(), "ES256");
    EXPECT_TRUE(decoded.signature_valid);
    EXPECT_EQ(decoded.claims["sub"], "mcp-client");
    EXPECT_EQ(decoded.claims["aud"], fixture.base);
}

TEST(EnterpriseAuthorizationTest, AnIdpClientCanAuthenticateWithAPrivateKey) {
    Fixture fixture;
    fixture.serve(4);
    auto config = fixture.config();
    config.idp_client.client_secret.reset();
    mcp::auth::PrivateKeyJwtConfig jwt;
    jwt.private_key_pem = mcp_test::private_key_pem(mcp_test::shared_rsa_key());
    config.idp_client.private_key_jwt = jwt;
    fixture.run(config, id_token(), [&](auto& authenticator) -> mcp::Task<void> {
        EXPECT_TRUE(co_await authenticator.try_handle_challenge(fixture.challenge()));
    });
    const auto& exchange = fixture.server.bodies()[2];
    const auto decoded = mcp_test::verify_jwt(form_value(exchange, "client_assertion"),
                                              mcp_test::shared_rsa_key(), "RS256");
    EXPECT_TRUE(decoded.signature_valid);
    EXPECT_EQ(decoded.claims["aud"], fixture.base + "/idp/token");
}

TEST(EnterpriseAuthorizationTest, ACachedTokenIsReusedOnlyForTheIdentityItWasIssuedFor) {
    Fixture fixture;
    fixture.serve(6);
    fixture.run(fixture.config(), id_token(), [&](auto& authenticator) -> mcp::Task<void> {
        EXPECT_TRUE(co_await authenticator.authorize());
        EXPECT_EQ(authenticator.get_access_token(), "2YotnFZFEjr1zCsicMWpAA");
        // The same identity is served from the cache: no exchange, no token request.
        EXPECT_TRUE(co_await authenticator.authorize());
        EXPECT_EQ(fixture.server.targets().size(), 4U);

        // Another user's assertion is a new grant, never the previous user's token.
        fixture.switched_assertion = mcp::auth::IdentityAssertion{"another-users-id-token"};
        fixture.token_reply["access_token"] = "second-users-token";
        EXPECT_TRUE(co_await authenticator.authorize());
        EXPECT_EQ(authenticator.get_access_token(), "second-users-token");
    });
    ASSERT_EQ(fixture.server.targets().size(), 6U);
    EXPECT_EQ(fixture.server.targets()[4], "/idp/token");
    EXPECT_EQ(fixture.server.targets()[5], "/token");
}

TEST(EnterpriseAuthorizationTest, RefreshRunsTheExchangeAgainWithAFreshAssertion) {
    Fixture fixture;
    fixture.serve(6);
    fixture.run(fixture.config(), id_token(), [&](auto& authenticator) -> mcp::Task<void> {
        EXPECT_FALSE(co_await authenticator.try_refresh_token());
        EXPECT_TRUE(co_await authenticator.try_handle_challenge(fixture.challenge()));
        fixture.token_reply["access_token"] = "renewed";
        EXPECT_TRUE(co_await authenticator.try_refresh_token());
        EXPECT_EQ(authenticator.get_access_token(), "renewed");
    });
    EXPECT_EQ(fixture.provider_calls, 2);
    ASSERT_EQ(fixture.server.targets().size(), 6U);
    EXPECT_EQ(fixture.server.targets()[4], "/idp/token");
    EXPECT_EQ(fixture.server.targets()[5], "/token");
}

TEST(EnterpriseAuthorizationTest, AuthorizeUsesTheWellKnownLocation) {
    Fixture fixture;
    fixture.serve(5);
    // Serve the protected resource document at the well-known path as well.
    const auto handler = [&fixture](const http::request<http::string_body>& request) {
        const std::string target(request.target());
        if (target == "/.well-known/oauth-protected-resource/mcp") {
            return json_response({{"resource", fixture.base + "/mcp"},
                                  {"authorization_servers", json::array({fixture.base})}});
        }
        if (target == "/.well-known/oauth-authorization-server") {
            json metadata = {{"issuer", fixture.base},
                             {"authorization_endpoint", fixture.base + "/a"},
                             {"token_endpoint", fixture.base + "/token"}};
            metadata.update(fixture.auth_server_extra);
            return json_response(metadata);
        }
        if (target == "/idp/token") {
            return json_response(fixture.idp_reply);
        }
        return json_response(fixture.token_reply);
    };
    fixture.server.set_handler(handler);
    fixture.run(fixture.config(), id_token(), [&](auto& authenticator) -> mcp::Task<void> {
        EXPECT_TRUE(co_await authenticator.authorize());
        EXPECT_EQ(authenticator.get_access_token(), "2YotnFZFEjr1zCsicMWpAA");
    });
    ASSERT_EQ(fixture.server.targets().size(), 4U);
    EXPECT_EQ(fixture.server.targets()[0], "/.well-known/oauth-protected-resource/mcp");
}

TEST(EnterpriseAuthorizationTest, TheConfigurationIsValidated) {
    asio::io_context io_ctx;
    auto store = std::make_shared<mcp::auth::InMemoryTokenStore>();
    const mcp::auth::IdentityAssertionProvider provider =
        []() -> mcp::Task<mcp::auth::IdentityAssertion> { co_return mcp::auth::IdentityAssertion{}; };
    mcp::auth::EnterpriseAuthorizationConfig config;
    config.server_url = "http://127.0.0.1/mcp";
    config.idp_token_endpoint = "http://127.0.0.1/idp";
    config.idp_client.client_id = "idp";
    config.client.client_id = "client";

    EXPECT_NO_THROW(mcp::auth::EnterpriseAuthorizationAuthenticator(io_ctx.get_executor(), store,
                                                                    config, provider));
    EXPECT_THROW(mcp::auth::EnterpriseAuthorizationAuthenticator(io_ctx.get_executor(), nullptr, config,
                                                                 provider),
                 std::invalid_argument);
    EXPECT_THROW(
        mcp::auth::EnterpriseAuthorizationAuthenticator(io_ctx.get_executor(), store, config, nullptr),
        std::invalid_argument);
    auto no_idp = config;
    no_idp.idp_token_endpoint.clear();
    EXPECT_THROW(
        mcp::auth::EnterpriseAuthorizationAuthenticator(io_ctx.get_executor(), store, no_idp, provider),
        std::invalid_argument);
    auto no_client = config;
    no_client.client.client_id.clear();
    EXPECT_THROW(mcp::auth::EnterpriseAuthorizationAuthenticator(io_ctx.get_executor(), store,
                                                                 no_client, provider),
                 std::invalid_argument);
}

TEST(EnterpriseAuthorizationCapabilityTest, BothSidesDeclareTheExtension) {
    mcp::ClientCapabilities client;
    mcp::auth::declare_enterprise_authorization_support(client);
    EXPECT_EQ(json(client)["extensions"]["io.modelcontextprotocol/enterprise-managed-authorization"],
              json::object());

    mcp::Server server({"ema-server", "1.0"}, {});
    mcp::auth::declare_enterprise_authorization_support(server);
    EXPECT_EQ(server.capabilities().extensions->count(
                  "io.modelcontextprotocol/enterprise-managed-authorization"),
              1U);
}

}  // namespace

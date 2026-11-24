/**
 * @file auth_client_credentials_test.cpp
 * @brief Client authentication at a token endpoint (client secret and private_key_jwt) and the
 *        OAuth client credentials flow, against a loopback authorization server.
 */

#include "support/jwt_test_keys.hpp"
#include "support/loopback_server.hpp"
#include "support/spawn.hpp"

#include <gtest/gtest.h>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <exception>
#include <functional>
#include <mcp/auth/client_authentication.hpp>
#include <mcp/auth/client_credentials.hpp>
#include <mcp/core/async.hpp>
#include <mcp/server/server.hpp>
#include <mcp/transport/http_client.hpp>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace auth_client_credentials_test {

namespace asio = boost::asio;
namespace http = boost::beast::http;
using json = nlohmann::json;
using mcp_test::form_has;
using mcp_test::form_value;
using mcp_test::json_response;
using mcp_test::status_response;

namespace {

mcp::auth::MetadataFetchPolicy loopback_policy(const std::string& origin) {
    mcp::auth::MetadataFetchPolicy policy;
    policy.allowed_origins.push_back(origin);
    policy.allow_plain_http_loopback = true;
    return policy;
}

json token_document(const std::string& token = "machine-access-token") {
    return {{"access_token", token}, {"token_type", "Bearer"}, {"expires_in", 3600}};
}

/// Authorization server metadata that supports the client credentials grant.
json server_metadata(const std::string& base, json extra = json::object()) {
    json metadata = {{"issuer", base},
                     {"authorization_endpoint", base + "/authorize"},
                     {"token_endpoint", base + "/token"},
                     {"token_endpoint_auth_methods_supported", json::array({"client_secret_basic"})}};
    metadata.update(extra);
    return metadata;
}

/// A mock MCP resource server and authorization server on one origin.
struct Fixture {
    asio::io_context io_ctx;
    mcp_test::LoopbackServer server{io_ctx};
    std::string base = server.base_url();
    json auth_server_extra = json::object();
    json token_reply = token_document();
    json prm_override = json::object();

    void serve(int budget) {
        server.set_handler([this](const http::request<http::string_body>& request) {
            const std::string target(request.target());
            if (target == "/prm" || target == "/.well-known/oauth-protected-resource/mcp") {
                json prm = {{"resource", base + "/mcp"},
                            {"authorization_servers", json::array({base})},
                            {"scopes_supported", json::array({"mcp:read", "mcp:write"})}};
                prm.update(prm_override);
                return json_response(prm);
            }
            if (target == "/.well-known/oauth-authorization-server") {
                return json_response(server_metadata(base, auth_server_extra));
            }
            if (target == "/token") {
                return json_response(token_reply);
            }
            return status_response(http::status::not_found);
        });
        mcp_test::spawn_detached(io_ctx, server.serve(budget));
    }

    [[nodiscard]] std::string challenge() const {
        return R"(Bearer realm="mcp", resource_metadata=")" + base + R"(/prm")";
    }

    mcp::auth::ClientCredentialsConfig config() const {
        mcp::auth::ClientCredentialsConfig config;
        config.server_url = base + "/mcp";
        config.client.client_id = "machine-client";
        config.client.client_secret = "s3cret";
        config.policy = loopback_policy(base);
        return config;
    }

    /// Runs @p body with an authenticator built from @p config, then closes the server.
    void run(const mcp::auth::ClientCredentialsConfig& config,
             std::function<mcp::Task<void>(mcp::auth::ClientCredentialsAuthenticator&,
                                           mcp::auth::TokenStore&)>
                 body) {
        auto store = std::make_shared<mcp::auth::InMemoryTokenStore>();
        mcp_test::spawn_detached(io_ctx, [&, store]() -> mcp::Task<void> {
            mcp::auth::ClientCredentialsAuthenticator authenticator(io_ctx.get_executor(), store,
                                                                    config);
            try {
                co_await body(authenticator, *store);
            } catch (const std::exception& error) {
                ADD_FAILURE() << "unexpected exception: " << error.what();
            }
            server.close();
        });
        io_ctx.run();
    }
};

TEST(ClientCredentialsTest, AClientSecretIsSentInTheBasicHeaderTheServerAdvertises) {
    Fixture fixture;
    fixture.serve(3);
    fixture.run(fixture.config(), [&](auto& authenticator, auto& store) -> mcp::Task<void> {
        const bool handled = co_await authenticator.try_handle_challenge(fixture.challenge());
        EXPECT_TRUE(handled);
        EXPECT_EQ(authenticator.get_access_token(), "machine-access-token");
        EXPECT_EQ(store.load(fixture.base + "/mcp")->access_token, "machine-access-token");
    });

    ASSERT_EQ(fixture.server.targets().size(), 3U);
    EXPECT_EQ(fixture.server.targets()[0], "/prm");
    EXPECT_EQ(fixture.server.targets()[1], "/.well-known/oauth-authorization-server");
    EXPECT_EQ(fixture.server.targets()[2], "/token");

    const auto& body = fixture.server.bodies()[2];
    EXPECT_EQ(form_value(body, "grant_type"), "client_credentials");
    EXPECT_EQ(form_value(body, "resource"), fixture.base + "/mcp");
    EXPECT_EQ(form_value(body, "scope"), "mcp:read mcp:write");
    EXPECT_FALSE(form_has(body, "client_secret"));
    EXPECT_FALSE(form_has(body, "client_id"));
    EXPECT_EQ(fixture.server.authorizations()[2], "Basic bWFjaGluZS1jbGllbnQ6czNjcmV0");
}

TEST(ClientCredentialsTest, AClientSecretGoesInTheBodyWhenOnlyPostIsAdvertised) {
    Fixture fixture;
    fixture.auth_server_extra = {
        {"token_endpoint_auth_methods_supported", json::array({"client_secret_post"})}};
    fixture.serve(3);
    fixture.run(fixture.config(), [&](auto& authenticator, auto&) -> mcp::Task<void> {
        EXPECT_TRUE(co_await authenticator.try_handle_challenge(fixture.challenge()));
    });

    const auto& body = fixture.server.bodies()[2];
    EXPECT_EQ(form_value(body, "client_id"), "machine-client");
    EXPECT_EQ(form_value(body, "client_secret"), "s3cret");
    EXPECT_TRUE(fixture.server.authorizations()[2].empty());
}

TEST(ClientCredentialsTest, ALocalScopeOverridesTheChallengeAndTheResource) {
    Fixture fixture;
    fixture.serve(3);
    auto config = fixture.config();
    config.scope = "mcp:admin";
    fixture.run(config, [&](auto& authenticator, auto&) -> mcp::Task<void> {
        EXPECT_TRUE(co_await authenticator.try_handle_challenge(
            R"(Bearer scope="ignored", resource_metadata=")" + fixture.base + R"(/prm")"));
    });
    EXPECT_EQ(form_value(fixture.server.bodies()[2], "scope"), "mcp:admin");
}

TEST(ClientCredentialsTest, TheChallengeScopeIsUsedWhenNoLocalScopeIsSet) {
    Fixture fixture;
    fixture.serve(3);
    fixture.run(fixture.config(), [&](auto& authenticator, auto&) -> mcp::Task<void> {
        EXPECT_TRUE(co_await authenticator.try_handle_challenge(
            R"(Bearer scope="mcp:read", resource_metadata=")" + fixture.base + R"(/prm")"));
    });
    EXPECT_EQ(form_value(fixture.server.bodies()[2], "scope"), "mcp:read");
}

TEST(ClientCredentialsTest, AStepUpChallengeRequestsTheUnionWithWhatWasGranted) {
    Fixture fixture;
    fixture.serve(6);
    fixture.run(fixture.config(), [&](auto& authenticator, auto&) -> mcp::Task<void> {
        EXPECT_TRUE(co_await authenticator.try_handle_challenge(
            R"(Bearer scope="mcp:read", resource_metadata=")" + fixture.base + R"(/prm")"));
        EXPECT_TRUE(co_await authenticator.try_handle_challenge(
            R"(Bearer error="insufficient_scope", scope="mcp:write", resource_metadata=")" +
            fixture.base + R"(/prm")"));
    });
    // Discovery is cached, so the second grant is one more request.
    ASSERT_EQ(fixture.server.targets().size(), 4U);
    EXPECT_EQ(form_value(fixture.server.bodies()[2], "scope"), "mcp:read");
    EXPECT_EQ(form_value(fixture.server.bodies()[3], "scope"), "mcp:write mcp:read");
}

TEST(ClientCredentialsTest, ARefusedChallengeAlwaysRequestsAFreshToken) {
    Fixture fixture;
    fixture.serve(4);
    fixture.run(fixture.config(), [&](auto& authenticator, auto&) -> mcp::Task<void> {
        EXPECT_TRUE(co_await authenticator.try_handle_challenge(fixture.challenge()));
        fixture.token_reply = token_document("second-token");
        EXPECT_TRUE(co_await authenticator.try_handle_challenge(fixture.challenge()));
        EXPECT_EQ(authenticator.get_access_token(), "second-token");
    });
    EXPECT_EQ(fixture.server.targets().size(), 4U);
}

// A resource document that names another server, or no authorization server, is refused.
TEST(ClientCredentialsTest, ARefusedProtectedResourceIsNeverFollowed) {
    struct Case {
        json prm_override;
        const char* reason;
    };
    const std::vector<Case> cases = {
        {{{"resource", "https://other.example/mcp"}}, "does not identify"},
        {{{"authorization_servers", json::array()}}, "listed no authorization servers"}};
    for (const auto& refusal : cases) {
        SCOPED_TRACE(refusal.reason);
        Fixture fixture;
        fixture.prm_override = refusal.prm_override;
        fixture.serve(3);
        bool refused = false;
        fixture.run(fixture.config(), [&](auto& authenticator, auto& store) -> mcp::Task<void> {
            try {
                (void)co_await authenticator.try_handle_challenge(fixture.challenge());
            } catch (const std::runtime_error& error) {
                refused = std::string(error.what()).find(refusal.reason) != std::string::npos;
            }
            EXPECT_FALSE(store.load(fixture.base + "/mcp").has_value());
        });
        EXPECT_TRUE(refused);
        // Only the resource document was fetched: no authorization server request, no credentials
        // sent.
        EXPECT_EQ(fixture.server.targets().size(), 1U);
    }
}

// RFC 9728 section 3.3: the resource a document names identifies the configured server when it is
// that URL, or a prefix of it on the same origin that ends where a path segment of the server URL
// ends. A query or fragment, a longer path, or a path that only shares characters with the
// server's does not, and nothing the document names is fetched.
TEST(ClientCredentialsTest, TheResourceIdentifiesTheServerOnlyByAWholePathPrefix) {
    const std::vector<std::pair<std::string, bool>> cases = {
        {"", true},
        {"/", true},
        {"/mcp/", true},
        {"/mc", false},
        {"/mcp/tools", false},
        {"/other", false},
        {"/mcp?tenant=1", false},
        {"/mcp#top", false},
    };
    for (const auto& [path, identifies] : cases) {
        SCOPED_TRACE(path);
        Fixture fixture;
        fixture.prm_override = {{"resource", fixture.base + path}};
        fixture.serve(3);
        bool handled = false;
        std::string failure;
        fixture.run(fixture.config(), [&](auto& authenticator, auto&) -> mcp::Task<void> {
            try {
                handled = co_await authenticator.try_handle_challenge(fixture.challenge());
            } catch (const std::runtime_error& error) {
                failure = error.what();
            }
        });
        EXPECT_EQ(handled, identifies) << failure;
        if (!identifies) {
            EXPECT_NE(failure.find("does not identify"), std::string::npos) << failure;
            EXPECT_EQ(fixture.server.targets().size(), 1U);
        }
    }
}

TEST(ClientCredentialsTest, AServerThatListsOtherGrantsOnlyIsRefusedBeforeAnyCredentialIsSent) {
    Fixture fixture;
    fixture.auth_server_extra = {{"grant_types_supported", json::array({"authorization_code"})}};
    fixture.serve(3);
    bool refused = false;
    fixture.run(fixture.config(), [&](auto& authenticator, auto&) -> mcp::Task<void> {
        try {
            (void)co_await authenticator.try_handle_challenge(fixture.challenge());
        } catch (const std::runtime_error& error) {
            refused = std::string(error.what()).find("client_credentials") != std::string::npos;
        }
    });
    EXPECT_TRUE(refused);
    EXPECT_EQ(fixture.server.targets().size(), 2U);
}

TEST(ClientCredentialsTest, AMismatchedIssuerIsRefused) {
    Fixture fixture;
    fixture.auth_server_extra = {{"issuer", "https://elsewhere.example"}};
    fixture.serve(3);
    bool refused = false;
    fixture.run(fixture.config(), [&](auto& authenticator, auto&) -> mcp::Task<void> {
        try {
            (void)co_await authenticator.try_handle_challenge(fixture.challenge());
        } catch (const std::runtime_error&) {
            refused = true;
        }
    });
    EXPECT_TRUE(refused);
    EXPECT_EQ(fixture.server.targets().size(), 2U);
}

TEST(ClientCredentialsTest, ANonBearerTokenIsRefused) {
    Fixture fixture;
    fixture.token_reply = {{"access_token", "x"}, {"token_type", "DPoP"}};
    fixture.serve(3);
    bool refused = false;
    fixture.run(fixture.config(), [&](auto& authenticator, auto& store) -> mcp::Task<void> {
        try {
            (void)co_await authenticator.try_handle_challenge(fixture.challenge());
        } catch (const std::runtime_error&) {
            refused = true;
        }
        EXPECT_FALSE(store.load(fixture.base + "/mcp").has_value());
    });
    EXPECT_TRUE(refused);
}

TEST(ClientCredentialsTest, ATokenErrorStatusFailsTheGrantWithoutLeakingTheSecret) {
    Fixture fixture;
    fixture.serve(3);
    fixture.server.set_handler([&](const http::request<http::string_body>& request) {
        const std::string target(request.target());
        if (target == "/prm") {
            return json_response({{"resource", fixture.base + "/mcp"},
                                  {"authorization_servers", json::array({fixture.base})}});
        }
        if (target == "/.well-known/oauth-authorization-server") {
            return json_response(server_metadata(fixture.base));
        }
        return status_response(http::status::unauthorized);
    });
    std::string message;
    fixture.run(fixture.config(), [&](auto& authenticator, auto&) -> mcp::Task<void> {
        try {
            (void)co_await authenticator.try_handle_challenge(fixture.challenge());
        } catch (const std::runtime_error& error) {
            message = error.what();
        }
    });
    EXPECT_FALSE(message.empty());
    EXPECT_EQ(message.find("s3cret"), std::string::npos);
}

TEST(ClientCredentialsTest, AResponseWithoutABearerChallengeIsNotHandled) {
    Fixture fixture;
    fixture.serve(1);
    fixture.run(fixture.config(), [&](auto& authenticator, auto&) -> mcp::Task<void> {
        EXPECT_FALSE(co_await authenticator.try_handle_challenge("Basic realm=\"x\""));
        EXPECT_FALSE(co_await authenticator.try_handle_challenge(""));
    });
    EXPECT_TRUE(fixture.server.targets().empty());
}

TEST(ClientCredentialsTest, AuthorizeDiscoversFromTheWellKnownLocationAndReusesTheCachedToken) {
    Fixture fixture;
    fixture.serve(3);
    fixture.run(fixture.config(), [&](auto& authenticator, auto&) -> mcp::Task<void> {
        EXPECT_TRUE(co_await authenticator.authorize());
        EXPECT_EQ(authenticator.get_access_token(), "machine-access-token");
        // The second call needs neither discovery nor a grant: both are cached for the issuer.
        EXPECT_TRUE(co_await authenticator.authorize());
    });
    ASSERT_EQ(fixture.server.targets().size(), 3U);
    EXPECT_EQ(fixture.server.targets()[0], "/.well-known/oauth-protected-resource/mcp");
}

TEST(ClientCredentialsTest, RefreshNeedsAPriorGrantAndThenRequestsANewToken) {
    Fixture fixture;
    fixture.serve(4);
    fixture.run(fixture.config(), [&](auto& authenticator, auto&) -> mcp::Task<void> {
        EXPECT_FALSE(co_await authenticator.try_refresh_token());
        EXPECT_TRUE(co_await authenticator.try_handle_challenge(fixture.challenge()));
        fixture.token_reply = token_document("renewed-token");
        EXPECT_TRUE(co_await authenticator.try_refresh_token());
        EXPECT_EQ(authenticator.get_access_token(), "renewed-token");
    });
    ASSERT_EQ(fixture.server.targets().size(), 4U);
    EXPECT_EQ(form_value(fixture.server.bodies()[3], "grant_type"), "client_credentials");
    EXPECT_EQ(form_value(fixture.server.bodies()[3], "resource"), fixture.base + "/mcp");
}

TEST(ClientCredentialsTest, AFailedRefreshReportsFalseAndKeepsTheStoredToken) {
    Fixture fixture;
    fixture.serve(4);
    fixture.run(fixture.config(), [&](auto& authenticator, auto&) -> mcp::Task<void> {
        EXPECT_TRUE(co_await authenticator.try_handle_challenge(fixture.challenge()));
        fixture.token_reply = {{"error", "invalid_client"}};
        EXPECT_FALSE(co_await authenticator.try_refresh_token());
        EXPECT_EQ(authenticator.get_access_token(), "machine-access-token");
    });
}

TEST(ClientCredentialsTest, ACloseRefusesFurtherWork) {
    Fixture fixture;
    fixture.serve(1);
    fixture.run(fixture.config(), [&](auto& authenticator, auto&) -> mcp::Task<void> {
        authenticator.close();
        bool refused = false;
        try {
            (void)co_await authenticator.try_handle_challenge(fixture.challenge());
        } catch (const std::runtime_error&) {
            refused = true;
        }
        EXPECT_TRUE(refused);
        EXPECT_FALSE(co_await authenticator.try_refresh_token());
    });
    EXPECT_TRUE(fixture.server.targets().empty());
}

TEST(ClientCredentialsTest, AnOriginTheFetchPolicyDoesNotListIsRefused) {
    Fixture fixture;
    fixture.serve(1);
    auto config = fixture.config();
    config.policy = mcp::auth::MetadataFetchPolicy{};
    bool refused = false;
    fixture.run(config, [&](auto& authenticator, auto&) -> mcp::Task<void> {
        try {
            (void)co_await authenticator.try_handle_challenge(fixture.challenge());
        } catch (const mcp::auth::MetadataPolicyError&) {
            refused = true;
        }
    });
    EXPECT_TRUE(refused);
    EXPECT_TRUE(fixture.server.targets().empty());
}

TEST(ClientCredentialsTest, TheConfigurationIsValidated) {
    asio::io_context io_ctx;
    auto store = std::make_shared<mcp::auth::InMemoryTokenStore>();
    mcp::auth::ClientCredentialsConfig config;
    config.server_url = "http://127.0.0.1/mcp";
    config.client.client_id = "id";
    config.client.client_secret = "secret";

    EXPECT_THROW(mcp::auth::ClientCredentialsAuthenticator(io_ctx.get_executor(), nullptr, config),
                 std::invalid_argument);
    auto no_url = config;
    no_url.server_url.clear();
    EXPECT_THROW(mcp::auth::ClientCredentialsAuthenticator(io_ctx.get_executor(), store, no_url),
                 std::invalid_argument);
    auto no_credentials = config;
    no_credentials.client.client_secret.reset();
    EXPECT_THROW(
        mcp::auth::ClientCredentialsAuthenticator(io_ctx.get_executor(), store, no_credentials),
        std::invalid_argument);
    auto unauthenticated = config;
    unauthenticated.client.method = "none";
    EXPECT_THROW(
        mcp::auth::ClientCredentialsAuthenticator(io_ctx.get_executor(), store, unauthenticated),
        std::invalid_argument);
}

TEST(ClientCredentialsTest, TheTransportReplaysARefusedRequestWithTheMachineToken) {
    Fixture fixture;
    fixture.server.set_handler([&](const http::request<http::string_body>& request) {
        const std::string target(request.target());
        if (target == "/mcp") {
            if (request[http::field::authorization] != "Bearer machine-access-token") {
                http::response<http::string_body> challenge{http::status::unauthorized, 11};
                challenge.set(http::field::www_authenticate,
                              R"(Bearer resource_metadata=")" + fixture.base + R"(/prm")");
                return challenge;
            }
            return status_response(http::status::accepted);
        }
        if (target == "/prm") {
            return json_response({{"resource", fixture.base + "/mcp"},
                                  {"authorization_servers", json::array({fixture.base})}});
        }
        if (target == "/.well-known/oauth-authorization-server") {
            return json_response(server_metadata(fixture.base));
        }
        if (target == "/token") {
            return json_response(token_document());
        }
        return status_response(http::status::not_found);
    });
    mcp_test::spawn_detached(fixture.io_ctx, fixture.server.serve(5));

    const std::string wire = R"({"jsonrpc":"2.0","id":1,"method":"ping"})";
    std::exception_ptr failure;
    mcp_test::spawn_detached(fixture.io_ctx, [&]() -> mcp::Task<void> {
        auto inner = std::make_shared<mcp::HttpClientTransport>(fixture.io_ctx.get_executor(),
                                                                fixture.base + "/mcp");
        auto store = std::make_shared<mcp::auth::InMemoryTokenStore>();
        auto authenticator = std::make_shared<mcp::auth::ClientCredentialsAuthenticator>(
            fixture.io_ctx.get_executor(), store, fixture.config());
        mcp::auth::OAuthClientTransport transport(inner, authenticator);
        try {
            co_await transport.write_message(wire);
        } catch (...) {
            failure = std::current_exception();
        }
        transport.close();
        fixture.server.close();
    });
    fixture.io_ctx.run();

    EXPECT_EQ(failure, nullptr);
    ASSERT_EQ(fixture.server.targets().size(), 5U);
    EXPECT_EQ(fixture.server.targets()[0], "/mcp");
    EXPECT_EQ(fixture.server.targets()[3], "/token");
    EXPECT_EQ(fixture.server.targets()[4], "/mcp");
    EXPECT_EQ(fixture.server.bodies()[4], wire);
    EXPECT_EQ(fixture.server.authorizations()[4], "Bearer machine-access-token");
}

TEST(ClientCredentialsCapabilityTest, BothSidesDeclareTheExtension) {
    mcp::ClientCapabilities client;
    mcp::auth::declare_client_credentials_support(client);
    EXPECT_EQ(json(client)["extensions"]["io.modelcontextprotocol/oauth-client-credentials"],
              json::object());

    mcp::Server server({"cc-server", "1.0"}, {});
    mcp::auth::declare_client_credentials_support(server);
    EXPECT_EQ(
        server.capabilities().extensions->count("io.modelcontextprotocol/oauth-client-credentials"),
        1U);
}

// ---------------------------------------------------------------------------
// private_key_jwt
// ---------------------------------------------------------------------------

class JwtClientCredentialsTest : public ::testing::TestWithParam<std::string> {};

TEST_P(JwtClientCredentialsTest, TheAssertionIsSignedByTheKeyAndNamesTheClientAndTheIssuer) {
    const std::string algorithm = GetParam();
    EVP_PKEY* key = algorithm == "ES256" ? mcp_test::shared_ec_key() : mcp_test::shared_rsa_key();

    Fixture fixture;
    fixture.auth_server_extra = {
        {"token_endpoint_auth_methods_supported", json::array({"private_key_jwt"})},
        {"token_endpoint_auth_signing_alg_values_supported", json::array({algorithm})}};
    fixture.serve(3);

    auto config = fixture.config();
    config.client.client_secret.reset();
    mcp::auth::PrivateKeyJwtConfig jwt;
    jwt.private_key_pem = mcp_test::private_key_pem(key);
    jwt.key_id = "key-1";
    config.client.private_key_jwt = jwt;
    fixture.run(config, [&](auto& authenticator, auto&) -> mcp::Task<void> {
        EXPECT_TRUE(co_await authenticator.try_handle_challenge(fixture.challenge()));
    });

    const auto& body = fixture.server.bodies()[2];
    EXPECT_EQ(form_value(body, "grant_type"), "client_credentials");
    EXPECT_EQ(form_value(body, "client_assertion_type"),
              "urn:ietf:params:oauth:client-assertion-type:jwt-bearer");
    // RFC 7523 section 3: the client is identified by the assertion, not by a client_id parameter.
    EXPECT_FALSE(form_has(body, "client_id"));
    EXPECT_FALSE(form_has(body, "client_secret"));
    EXPECT_TRUE(fixture.server.authorizations()[2].empty());

    const auto decoded = mcp_test::verify_jwt(form_value(body, "client_assertion"), key, algorithm);
    EXPECT_TRUE(decoded.signature_valid);
    EXPECT_EQ(decoded.header["alg"], algorithm);
    EXPECT_EQ(decoded.header["kid"], "key-1");
    EXPECT_EQ(decoded.claims["iss"], "machine-client");
    EXPECT_EQ(decoded.claims["sub"], "machine-client");
    EXPECT_EQ(decoded.claims["aud"], fixture.base);
    EXPECT_FALSE(decoded.claims["jti"].get<std::string>().empty());
    EXPECT_EQ(decoded.claims["exp"].get<long>() - decoded.claims["iat"].get<long>(), 60);
}

INSTANTIATE_TEST_SUITE_P(Algorithms, JwtClientCredentialsTest,
                         ::testing::Values("RS256", "PS256", "ES256"));

TEST(ClientAuthenticationTest, EachAssertionCarriesItsOwnJti) {
    mcp::auth::PrivateKeyJwtConfig jwt;
    jwt.private_key_pem = mcp_test::private_key_pem(mcp_test::shared_rsa_key());
    const auto first = mcp::auth::sign_client_assertion(jwt, "c", "https://as/token", "RS256");
    const auto second = mcp::auth::sign_client_assertion(jwt, "c", "https://as/token", "RS256");
    EXPECT_NE(mcp_test::verify_jwt(first, mcp_test::shared_rsa_key(), "RS256").claims["jti"],
              mcp_test::verify_jwt(second, mcp_test::shared_rsa_key(), "RS256").claims["jti"]);
}

TEST(ClientAuthenticationTest, AnAudienceOverrideReplacesTheIssuer) {
    mcp::auth::PrivateKeyJwtConfig jwt;
    jwt.private_key_pem = mcp_test::private_key_pem(mcp_test::shared_rsa_key());
    jwt.audience = "https://issuer.example";
    const auto assertion = mcp::auth::sign_client_assertion(jwt, "c", "https://as/token", "RS256");
    EXPECT_EQ(mcp_test::verify_jwt(assertion, mcp_test::shared_rsa_key(), "RS256").claims["aud"],
              "https://issuer.example");
}

TEST(ClientAuthenticationTest, TheAlgorithmFollowsTheKeyAndTheServersList) {
    mcp::auth::PrivateKeyJwtConfig rsa;
    rsa.private_key_pem = mcp_test::private_key_pem(mcp_test::shared_rsa_key());
    EXPECT_EQ(mcp::auth::choose_signing_algorithm(rsa, std::nullopt), "RS256");
    EXPECT_EQ(mcp::auth::choose_signing_algorithm(rsa, std::vector<std::string>{"ES256", "PS256"}),
              "PS256");
    EXPECT_THROW((void)mcp::auth::choose_signing_algorithm(rsa, std::vector<std::string>{"ES256"}),
                 std::runtime_error);

    rsa.algorithm = "PS256";
    EXPECT_EQ(mcp::auth::choose_signing_algorithm(rsa, std::vector<std::string>{"RS256"}), "PS256");
    rsa.algorithm = "ES256";
    EXPECT_THROW((void)mcp::auth::choose_signing_algorithm(rsa, std::nullopt), std::invalid_argument);

    mcp::auth::PrivateKeyJwtConfig ec;
    ec.private_key_pem = mcp_test::private_key_pem(mcp_test::shared_ec_key());
    EXPECT_EQ(mcp::auth::choose_signing_algorithm(ec, std::nullopt), "ES256");

    mcp::auth::PrivateKeyJwtConfig broken;
    broken.private_key_pem = "not a key";
    EXPECT_THROW((void)mcp::auth::choose_signing_algorithm(broken, std::nullopt),
                 std::invalid_argument);
}

TEST(ClientAuthenticationTest, AnRsaKeyUnder2048BitsIsRefused) {
    const std::unique_ptr<EVP_PKEY, mcp_test::EvpKeyDeleter> small(
        EVP_PKEY_Q_keygen(nullptr, nullptr, "RSA", static_cast<std::size_t>(1024)));
    ASSERT_NE(small, nullptr);
    mcp::auth::PrivateKeyJwtConfig jwt;
    jwt.private_key_pem = mcp_test::private_key_pem(small.get());
    EXPECT_THROW((void)mcp::auth::choose_signing_algorithm(jwt, std::nullopt), std::invalid_argument);
    EXPECT_THROW((void)mcp::auth::sign_client_assertion(jwt, "c", "https://as/token", "RS256"),
                 std::invalid_argument);
}

// Only RSA keys and EC keys on P-256 sign client assertions, each with its own algorithms.
TEST(ClientAuthenticationTest, AKeyOfAnotherTypeOrCurveOrAlgorithmIsRefused) {
    for (const auto& [type, curve] :
         std::vector<std::pair<std::string, std::string>>{{"ED25519", ""}, {"EC", "P-384"}}) {
        const std::unique_ptr<EVP_PKEY, mcp_test::EvpKeyDeleter> key(
            curve.empty() ? EVP_PKEY_Q_keygen(nullptr, nullptr, type.c_str())
                          : EVP_PKEY_Q_keygen(nullptr, nullptr, type.c_str(), curve.c_str()));
        ASSERT_NE(key, nullptr) << type << curve;
        mcp::auth::PrivateKeyJwtConfig jwt;
        jwt.private_key_pem = mcp_test::private_key_pem(key.get());
        EXPECT_THROW((void)mcp::auth::choose_signing_algorithm(jwt, std::nullopt),
                     std::invalid_argument)
            << type << curve;
    }

    mcp::auth::PrivateKeyJwtConfig ec;
    ec.private_key_pem = mcp_test::private_key_pem(mcp_test::shared_ec_key());
    EXPECT_THROW((void)mcp::auth::sign_client_assertion(ec, "c", "https://as/token", "RS256"),
                 std::invalid_argument);
}

// A method the application names must fit its credentials, and must be one the SDK implements.
TEST(ClientAuthenticationTest, ANamedMethodTheCredentialsCannotSatisfyIsRefused) {
    mcp::auth::AuthServerMetadata server;
    server.token_endpoint = "https://as.example/token";
    server.raw = json::object();
    mcp::auth::ClientAuthentication credentials;
    credentials.client_id = "c";

    for (const char* method :
         {"private_key_jwt", "client_secret_basic", "client_secret_post", "tls_client_auth"}) {
        credentials.method = method;
        mcp::KeyValuePairList params;
        EXPECT_THROW((void)mcp::auth::authenticate_token_request(credentials, server, params),
                     std::invalid_argument)
            << method;
        EXPECT_TRUE(params.empty()) << method;
    }

    credentials.method = "none";
    mcp::KeyValuePairList params;
    EXPECT_TRUE(mcp::auth::authenticate_token_request(credentials, server, params).empty());
    ASSERT_EQ(params.size(), 1U);
    EXPECT_EQ(params[0], (std::pair<std::string, std::string>{"client_id", "c"}));
}

TEST(ClientAuthenticationTest, AJwtIsPreferredOverASecretWhenTheServerAllowsBoth) {
    mcp::auth::AuthServerMetadata server;
    server.token_endpoint = "https://as.example/token";
    server.token_endpoint_auth_methods_supported =
        std::vector<std::string>{"client_secret_basic", "private_key_jwt"};
    server.raw = json::object();

    mcp::auth::ClientAuthentication credentials;
    credentials.client_id = "c";
    credentials.client_secret = "s";
    mcp::auth::PrivateKeyJwtConfig jwt;
    jwt.private_key_pem = mcp_test::private_key_pem(mcp_test::shared_rsa_key());
    credentials.private_key_jwt = jwt;

    mcp::KeyValuePairList params;
    const auto header = mcp::auth::authenticate_token_request(credentials, server, params);
    EXPECT_TRUE(header.empty());
    ASSERT_EQ(params.size(), 2U);
    EXPECT_EQ(params[0].first, "client_assertion_type");

    // With the JWT method unlisted the secret is used.
    server.token_endpoint_auth_methods_supported = std::vector<std::string>{"client_secret_basic"};
    mcp::KeyValuePairList basic_params;
    EXPECT_EQ(
        mcp::auth::authenticate_token_request(credentials, server, basic_params).rfind("Basic ", 0),
        0U);
    EXPECT_TRUE(basic_params.empty());
}

TEST(ClientAuthenticationTest, BasicCredentialsAreFormEncodedBeforeTheyAreJoined) {
    mcp::auth::AuthServerMetadata server;
    server.token_endpoint = "https://as.example/token";
    server.raw = json::object();
    mcp::auth::ClientAuthentication credentials;
    credentials.client_id = "my client";
    credentials.client_secret = "a:b+c";
    mcp::KeyValuePairList params;
    const auto header = mcp::auth::authenticate_token_request(credentials, server, params);
    // "my+client:a%3Ab%2Bc"
    EXPECT_EQ(header, "Basic bXkrY2xpZW50OmElM0FiJTJCYw==");
}

TEST(ClientAuthenticationTest, NoCommonMethodIsAnError) {
    mcp::auth::AuthServerMetadata server;
    server.token_endpoint = "https://as.example/token";
    server.token_endpoint_auth_methods_supported = std::vector<std::string>{"private_key_jwt"};
    server.raw = json::object();
    mcp::auth::ClientAuthentication credentials;
    credentials.client_id = "c";
    credentials.client_secret = "s";
    mcp::KeyValuePairList params;
    EXPECT_THROW((void)mcp::auth::authenticate_token_request(credentials, server, params),
                 std::runtime_error);

    credentials.method = "client_secret_basic";
    EXPECT_THROW((void)mcp::auth::authenticate_token_request(credentials, server, params),
                 std::runtime_error);
    EXPECT_TRUE(params.empty());
}

}  // namespace

}  // namespace auth_client_credentials_test

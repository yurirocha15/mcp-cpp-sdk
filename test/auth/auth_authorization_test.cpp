/**
 * @file auth_authorization_test.cpp
 * @brief Loopback tests for challenge-driven OAuth authorization and the outbound-request controls
 *        that guard it (Slice A / 2.E5a).
 *
 * These tests are the primary evidence for the slice: they exercise the full
 * challenge -> discovery -> authorize -> token -> replay contract against loopback fixtures without
 * involving any conformance runner.
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <exception>
#include <functional>
#include <mcp/auth/client_identity.hpp>
#include <mcp/auth/oauth.hpp>
#include <mcp/transport/http_client.hpp>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using json = nlohmann::json;

namespace {

/// Loopback HTTP server that answers a scripted handler and records everything it received.
///
/// `accepts()` is the socket ledger the security tests assert on: a control that refuses a target
/// must leave it at zero even though this server is listening and would happily accept.
class LoopbackServer final {
   public:
    using Handler =
        std::function<http::response<http::string_body>(const http::request<http::string_body>&)>;

    explicit LoopbackServer(asio::io_context& io_ctx)
        : acceptor_(io_ctx, {asio::ip::make_address("127.0.0.1"), 0}) {}

    [[nodiscard]] unsigned short port() const { return acceptor_.local_endpoint().port(); }
    [[nodiscard]] std::string base_url() const { return "http://127.0.0.1:" + std::to_string(port()); }
    [[nodiscard]] std::string origin() const { return base_url(); }

    void set_handler(Handler handler) { handler_ = std::move(handler); }

    [[nodiscard]] const std::vector<std::string>& targets() const { return targets_; }
    [[nodiscard]] const std::vector<std::string>& bodies() const { return bodies_; }
    [[nodiscard]] const std::vector<std::string>& authorizations() const { return authorizations_; }
    [[nodiscard]] int accepts() const { return accepts_; }

    /// Serve at most `request_budget` requests, then stop. close() aborts a pending accept so the
    /// io_context always drains.
    asio::awaitable<void> serve(int request_budget) {
        for (int index = 0; index < request_budget; ++index) {
            boost::system::error_code accept_error;
            auto socket = co_await acceptor_.async_accept(
                asio::redirect_error(asio::use_awaitable, accept_error));
            if (accept_error) {
                co_return;
            }
            ++accepts_;

            beast::tcp_stream stream(std::move(socket));
            beast::flat_buffer buffer;
            http::request<http::string_body> request;
            boost::system::error_code read_error;
            co_await http::async_read(stream, buffer, request,
                                      asio::redirect_error(asio::use_awaitable, read_error));
            if (read_error) {
                co_return;
            }

            targets_.emplace_back(request.target());
            bodies_.push_back(request.body());
            authorizations_.emplace_back(request[http::field::authorization]);

            auto response = handler_(request);
            response.version(request.version());
            response.prepare_payload();
            boost::system::error_code write_error;
            co_await http::async_write(stream, response,
                                       asio::redirect_error(asio::use_awaitable, write_error));

            beast::error_code shutdown_error;
            (void)stream.socket().shutdown(asio::ip::tcp::socket::shutdown_both, shutdown_error);
        }
    }

    void close() {
        boost::system::error_code ignored;
        (void)acceptor_.close(ignored);
    }

   private:
    asio::ip::tcp::acceptor acceptor_;
    Handler handler_;
    std::vector<std::string> targets_;
    std::vector<std::string> bodies_;
    std::vector<std::string> authorizations_;
    int accepts_{0};
};

http::response<http::string_body> json_response(const json& body) {
    http::response<http::string_body> response{http::status::ok, 11};
    response.set(http::field::content_type, "application/json");
    response.body() = body.dump();
    return response;
}

http::response<http::string_body> status_response(http::status status) {
    http::response<http::string_body> response{status, 11};
    response.body() = "{}";
    return response;
}

json auth_server_metadata(const std::string& base, bool iss_supported) {
    json metadata = {{"issuer", base},
                     {"authorization_endpoint", base + "/authorize"},
                     {"token_endpoint", base + "/token"},
                     {"response_types_supported", json::array({"code"})},
                     {"code_challenge_methods_supported", json::array({"S256"})}};
    if (iss_supported) {
        metadata["authorization_response_iss_parameter_supported"] = true;
    }
    return metadata;
}

json token_document() {
    return {{"access_token", "granted-access-token"},
            {"token_type", "Bearer"},
            {"refresh_token", "granted-refresh-token"},
            {"expires_in", 3600}};
}

/// Extract one query parameter from an authorization URL.
std::string query_value(const std::string& url, const std::string& name) {
    const auto response = mcp::auth::parse_authorization_response(url);
    if (name == "state") {
        return response.state.value_or("");
    }
    const auto needle = name + "=";
    auto position = url.find("?" + needle);
    if (position == std::string::npos) {
        position = url.find("&" + needle);
    }
    if (position == std::string::npos) {
        return {};
    }
    const auto start = position + needle.size() + 1;
    const auto end = url.find('&', start);
    return url.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

mcp::auth::MetadataFetchPolicy loopback_policy(const std::string& origin) {
    mcp::auth::MetadataFetchPolicy policy;
    policy.allowed_origins.push_back(origin);
    // The narrow opt-out the loopback fixture needs; never enabled implicitly.
    policy.allow_plain_http_loopback = true;
    return policy;
}

/// Consent callback that echoes the recorded state and issuer, as a compliant server would.
mcp::auth::AuthorizationCallback echoing_callback(std::string* captured_url) {
    return [captured_url](const mcp::auth::AuthorizationRequest& request)
               -> mcp::Task<mcp::auth::AuthorizationResponse> {
        if (captured_url != nullptr) {
            *captured_url = request.authorization_url;
        }
        mcp::auth::AuthorizationResponse response;
        response.code = "test-authorization-code";
        response.state = request.state;
        response.iss = request.issuer;
        co_return response;
    };
}

struct ManagerFixture {
    std::shared_ptr<mcp::auth::InMemoryTokenStore> store =
        std::make_shared<mcp::auth::InMemoryTokenStore>();
    mcp::auth::OAuthAuthorizationConfig config;
    std::string authorization_url;
    int resolver_calls{0};
};

}  // namespace

TEST(AuthAuthorizationManagerTest, CompletesChallengeDiscoveryAuthorizeAndTokenExchange) {
    asio::io_context io_ctx;
    LoopbackServer server(io_ctx);
    const auto base = server.base_url();

    server.set_handler([&base](const http::request<http::string_body>& request) {
        const std::string target(request.target());
        if (target == "/custom/prm.json") {
            return json_response({{"resource", base + "/mcp"},
                                  {"authorization_servers", json::array({base})},
                                  {"scopes_supported", json::array({"mcp:read", "mcp:write"})}});
        }
        if (target == "/.well-known/oauth-authorization-server") {
            return json_response(auth_server_metadata(base, true));
        }
        if (target == "/token") {
            return json_response(token_document());
        }
        return status_response(http::status::not_found);
    });
    asio::co_spawn(io_ctx, server.serve(3), asio::detached);

    ManagerFixture fixture;
    fixture.config.server_url = base + "/mcp";
    fixture.config.client_id = "test-client";
    fixture.config.redirect_uri = "http://127.0.0.1:9999/callback";
    fixture.config.policy = loopback_policy(server.origin());

    bool authorized = false;
    std::exception_ptr failure;
    std::optional<mcp::auth::AuthorizationRequest> record;
    std::string access_token;
    asio::co_spawn(
        io_ctx,
        [&]() -> mcp::Task<void> {
            mcp::auth::OAuthAuthorizationManager manager(io_ctx.get_executor(), fixture.store,
                                                         fixture.config,
                                                         echoing_callback(&fixture.authorization_url));
            try {
                authorized = co_await manager.try_handle_challenge(
                    R"(Bearer realm="mcp", resource_metadata=")" + base + R"(/custom/prm.json")");
                record = manager.last_authorization_request();
                access_token = manager.get_access_token();
            } catch (...) {
                failure = std::current_exception();
            }
            server.close();
        },
        asio::detached);

    io_ctx.run();

    ASSERT_EQ(failure, nullptr);
    EXPECT_TRUE(authorized);
    ASSERT_TRUE(record.has_value());
    EXPECT_EQ(record->issuer, base);
    EXPECT_TRUE(record->issuer_parameter_supported);
    EXPECT_FALSE(record->state.empty());
    EXPECT_EQ(record->code_verifier.size(), 64U);
    EXPECT_EQ(record->resource, base + "/mcp");
    EXPECT_EQ(record->scope, "mcp:read mcp:write");
    EXPECT_EQ(access_token, "granted-access-token");

    // The challenge named the metadata location, so it is fetched directly and the well-known
    // fallback is never probed.
    ASSERT_EQ(server.targets().size(), 3U);
    EXPECT_EQ(server.targets()[0], "/custom/prm.json");
    EXPECT_EQ(server.targets()[1], "/.well-known/oauth-authorization-server");
    EXPECT_EQ(server.targets()[2], "/token");

    EXPECT_NE(fixture.authorization_url.find("code_challenge_method=S256"), std::string::npos);
    EXPECT_NE(fixture.authorization_url.find("code_challenge="), std::string::npos);
    EXPECT_NE(fixture.authorization_url.find("response_type=code"), std::string::npos);
    EXPECT_EQ(query_value(fixture.authorization_url, "resource"),
              mcp::auth::detail::url_encode(base + "/mcp"));

    const auto& token_body = server.bodies()[2];
    EXPECT_NE(token_body.find("grant_type=authorization_code"), std::string::npos);
    EXPECT_NE(token_body.find("code=test-authorization-code"), std::string::npos);
    EXPECT_NE(token_body.find("code_verifier="), std::string::npos);
    EXPECT_NE(token_body.find("resource="), std::string::npos);
}

TEST(AuthAuthorizationManagerTest, FallsBackToWellKnownOrderWhenTheChallengeNamesNoMetadata) {
    asio::io_context io_ctx;
    LoopbackServer server(io_ctx);
    const auto base = server.base_url();

    server.set_handler([&base](const http::request<http::string_body>& request) {
        const std::string target(request.target());
        if (target == "/.well-known/oauth-protected-resource/mcp") {
            return status_response(http::status::not_found);
        }
        if (target == "/.well-known/oauth-protected-resource") {
            return json_response(
                {{"resource", base + "/mcp"}, {"authorization_servers", json::array({base})}});
        }
        if (target == "/.well-known/oauth-authorization-server") {
            return json_response(auth_server_metadata(base, false));
        }
        if (target == "/token") {
            return json_response(token_document());
        }
        return status_response(http::status::not_found);
    });
    asio::co_spawn(io_ctx, server.serve(4), asio::detached);

    ManagerFixture fixture;
    fixture.config.server_url = base + "/mcp";
    fixture.config.client_id = "test-client";
    fixture.config.redirect_uri = "http://127.0.0.1:9999/callback";
    fixture.config.policy = loopback_policy(server.origin());

    bool authorized = false;
    asio::co_spawn(
        io_ctx,
        [&]() -> mcp::Task<void> {
            mcp::auth::OAuthAuthorizationManager manager(io_ctx.get_executor(), fixture.store,
                                                         fixture.config, echoing_callback(nullptr));
            authorized = co_await manager.try_handle_challenge(R"(Bearer realm="mcp")");
            server.close();
        },
        asio::detached);

    io_ctx.run();

    EXPECT_TRUE(authorized);
    ASSERT_EQ(server.targets().size(), 4U);
    // Path-based location is tried before the root one.
    EXPECT_EQ(server.targets()[0], "/.well-known/oauth-protected-resource/mcp");
    EXPECT_EQ(server.targets()[1], "/.well-known/oauth-protected-resource");
}

TEST(AuthAuthorizationManagerTest, RejectsAnIssuerMismatchBeforeExchangingTheCode) {
    asio::io_context io_ctx;
    LoopbackServer server(io_ctx);
    const auto base = server.base_url();

    server.set_handler([&base](const http::request<http::string_body>& request) {
        const std::string target(request.target());
        if (target == "/prm") {
            return json_response(
                {{"resource", base + "/mcp"}, {"authorization_servers", json::array({base})}});
        }
        if (target == "/.well-known/oauth-authorization-server") {
            return json_response(auth_server_metadata(base, true));
        }
        return status_response(http::status::not_found);
    });
    asio::co_spawn(io_ctx, server.serve(2), asio::detached);

    ManagerFixture fixture;
    fixture.config.server_url = base + "/mcp";
    fixture.config.client_id = "test-client";
    fixture.config.redirect_uri = "http://127.0.0.1:9999/callback";
    fixture.config.policy = loopback_policy(server.origin());

    std::string message;
    asio::co_spawn(
        io_ctx,
        [&]() -> mcp::Task<void> {
            auto callback = [](const mcp::auth::AuthorizationRequest& request)
                -> mcp::Task<mcp::auth::AuthorizationResponse> {
                mcp::auth::AuthorizationResponse response;
                response.code = "test-authorization-code";
                response.state = request.state;
                // Equivalent under RFC 3986 normalization, but the comparison is not canonicalizing.
                response.iss = request.issuer + "/";
                co_return response;
            };
            mcp::auth::OAuthAuthorizationManager manager(io_ctx.get_executor(), fixture.store,
                                                         fixture.config, callback);
            try {
                (void)co_await manager.try_handle_challenge(R"(Bearer resource_metadata=")" + base +
                                                            R"(/prm")");
            } catch (const std::exception& error) {
                message = error.what();
            }
            server.close();
        },
        asio::detached);

    io_ctx.run();

    EXPECT_NE(message.find("iss did not match"), std::string::npos);
    // The token endpoint was never reached.
    EXPECT_EQ(server.targets().size(), 2U);
    EXPECT_TRUE(fixture.store->load(base + "/mcp") == std::nullopt);
}

TEST(AuthAuthorizationManagerTest, RefreshesTheStoredTokenAfterAuthorization) {
    asio::io_context io_ctx;
    LoopbackServer server(io_ctx);
    const auto base = server.base_url();

    int token_requests = 0;
    server.set_handler([&](const http::request<http::string_body>& request) {
        const std::string target(request.target());
        if (target == "/prm") {
            return json_response(
                {{"resource", base + "/mcp"}, {"authorization_servers", json::array({base})}});
        }
        if (target == "/.well-known/oauth-authorization-server") {
            return json_response(auth_server_metadata(base, true));
        }
        if (target == "/token") {
            ++token_requests;
            if (token_requests == 1) {
                return json_response(token_document());
            }
            return json_response({{"access_token", "renewed-access-token"},
                                  {"token_type", "Bearer"},
                                  {"expires_in", 3600}});
        }
        return status_response(http::status::not_found);
    });
    asio::co_spawn(io_ctx, server.serve(4), asio::detached);

    ManagerFixture fixture;
    fixture.config.server_url = base + "/mcp";
    fixture.config.client_id = "test-client";
    fixture.config.redirect_uri = "http://127.0.0.1:9999/callback";
    fixture.config.policy = loopback_policy(server.origin());

    bool refreshed = false;
    std::string token_after_refresh;
    asio::co_spawn(
        io_ctx,
        [&]() -> mcp::Task<void> {
            mcp::auth::OAuthAuthorizationManager manager(io_ctx.get_executor(), fixture.store,
                                                         fixture.config, echoing_callback(nullptr));
            (void)co_await manager.try_handle_challenge(R"(Bearer resource_metadata=")" + base +
                                                        R"(/prm")");
            refreshed = co_await manager.try_refresh_token();
            token_after_refresh = manager.get_access_token();
            server.close();
        },
        asio::detached);

    io_ctx.run();

    EXPECT_TRUE(refreshed);
    EXPECT_EQ(token_after_refresh, "renewed-access-token");
    EXPECT_EQ(token_requests, 2);
    // The refresh grant reuses the refresh token issued with the original grant.
    EXPECT_NE(server.bodies()[3].find("grant_type=refresh_token"), std::string::npos);
    EXPECT_NE(server.bodies()[3].find("refresh_token=granted-refresh-token"), std::string::npos);
}

TEST(AuthDiscoveryChallengeUrlTest, ChallengeSuppliedUrlSuppressesTheWellKnownFallback) {
    asio::io_context io_ctx;
    LoopbackServer server(io_ctx);
    const auto base = server.base_url();

    server.set_handler([&base](const http::request<http::string_body>& request) {
        const std::string target(request.target());
        if (target == "/custom/location.json") {
            return json_response(
                {{"resource", base + "/mcp"}, {"authorization_servers", json::array({base})}});
        }
        // Any well-known probe would be a priority-order violation, so report it as such.
        return json_response({{"resource", "well-known-should-not-be-probed"},
                              {"authorization_servers", json::array({base})}});
    });
    asio::co_spawn(io_ctx, server.serve(2), asio::detached);

    std::string resource;
    asio::co_spawn(
        io_ctx,
        [&]() -> mcp::Task<void> {
            auto client = std::make_shared<mcp::auth::OAuthHttpClient>(io_ctx.get_executor());
            client->set_metadata_policy(loopback_policy(server.origin()));
            mcp::auth::OAuthDiscoveryClient discovery(client);
            const auto metadata = co_await discovery.discover_protected_resource(
                base + "/mcp", std::optional<std::string>(base + "/custom/location.json"));
            resource = metadata.resource;
            server.close();
        },
        asio::detached);

    io_ctx.run();

    EXPECT_EQ(resource, base + "/mcp");
    ASSERT_EQ(server.targets().size(), 1U);
    EXPECT_EQ(server.targets()[0], "/custom/location.json");
}

namespace {

/// An Authenticator written before the challenge hook existed: it overrides only the two original
/// pure virtuals and must keep working unchanged.
class LegacyAuthenticator final : public mcp::auth::Authenticator {
   public:
    [[nodiscard]] std::string get_access_token() const override { return access_token_; }

    mcp::Task<bool> try_refresh_token() override {
        ++refreshes_;
        access_token_ = "legacy-refreshed-token";
        co_return true;
    }

    [[nodiscard]] int refreshes() const { return refreshes_; }

   private:
    std::string access_token_;
    int refreshes_{0};
};

}  // namespace

TEST(AuthChallengeReplayTest, LegacyAuthenticatorsStillFallThroughToRefresh) {
    asio::io_context io_ctx;
    LoopbackServer server(io_ctx);

    server.set_handler([](const http::request<http::string_body>& request) {
        if (request[http::field::authorization].empty()) {
            http::response<http::string_body> challenge{http::status::unauthorized, 11};
            challenge.set(http::field::www_authenticate,
                          R"(Bearer realm="mcp", resource_metadata="https://blocked.test/prm")");
            return challenge;
        }
        return status_response(http::status::accepted);
    });
    asio::co_spawn(io_ctx, server.serve(2), asio::detached);

    auto authenticator = std::make_shared<LegacyAuthenticator>();
    const std::string wire = R"({"jsonrpc":"2.0","id":7,"method":"ping"})";
    std::exception_ptr failure;

    asio::co_spawn(
        io_ctx,
        [&]() -> mcp::Task<void> {
            auto inner = std::make_shared<mcp::HttpClientTransport>(io_ctx.get_executor(),
                                                                    server.base_url() + "/mcp");
            mcp::auth::OAuthClientTransport transport(inner, authenticator);
            try {
                co_await transport.write_message(wire);
            } catch (...) {
                failure = std::current_exception();
            }
            transport.close();
            server.close();
        },
        asio::detached);

    io_ctx.run();

    ASSERT_EQ(failure, nullptr);
    // The default challenge hook reported that it handled nothing, so the legacy refresh ran.
    EXPECT_EQ(authenticator->refreshes(), 1);
    ASSERT_EQ(server.bodies().size(), 2U);
    EXPECT_EQ(server.bodies()[1], wire);
    EXPECT_EQ(server.authorizations()[1], "Bearer legacy-refreshed-token");
}

namespace {

/// Drive one challenge against a manager whose resolver is instrumented, and report the refusal.
struct RefusalOutcome {
    bool threw{false};
    mcp::auth::MetadataUrlDecision decision{mcp::auth::MetadataUrlDecision::allowed};
    int resolver_calls{0};
};

RefusalOutcome refuse_challenge(const std::string& challenge,
                                const std::vector<std::string>& extra_origins) {
    asio::io_context io_ctx;
    LoopbackServer server(io_ctx);

    server.set_handler(
        [](const http::request<http::string_body>&) { return status_response(http::status::ok); });
    asio::co_spawn(io_ctx, server.serve(1), asio::detached);

    RefusalOutcome outcome;
    auto store = std::make_shared<mcp::auth::InMemoryTokenStore>();

    mcp::auth::OAuthAuthorizationConfig config;
    config.server_url = server.base_url() + "/mcp";
    config.client_id = "test-client";
    config.redirect_uri = "http://127.0.0.1:9999/callback";
    config.policy = loopback_policy(server.origin());
    for (const auto& origin : extra_origins) {
        config.policy.allowed_origins.push_back(origin);
    }
    config.host_resolver = [&outcome](const std::string&,
                                      const std::string&) -> std::vector<std::string> {
        ++outcome.resolver_calls;
        return {"127.0.0.1"};
    };

    asio::co_spawn(
        io_ctx,
        [&]() -> mcp::Task<void> {
            mcp::auth::OAuthAuthorizationManager manager(io_ctx.get_executor(), store, config,
                                                         echoing_callback(nullptr));
            try {
                (void)co_await manager.try_handle_challenge(challenge);
            } catch (const mcp::auth::MetadataPolicyError& error) {
                outcome.threw = true;
                outcome.decision = error.decision();
            } catch (...) {
                // Reported as a non-refusal by leaving `threw` false.
            }
            server.close();
        },
        asio::detached);

    io_ctx.run();
    return outcome;
}

}  // namespace

// The link-local metadata service is the canonical SSRF target. It must be refused before the host
// is ever resolved, and resolution is the only route to a socket.
TEST(AuthMetadataSsrfTest, RefusesTheLinkLocalMetadataServiceWithoutResolving) {
    const auto outcome =
        refuse_challenge(R"(Bearer resource_metadata="http://169.254.169.254/latest/meta-data")",
                         {"http://169.254.169.254"});
    EXPECT_TRUE(outcome.threw);
    EXPECT_EQ(outcome.decision, mcp::auth::MetadataUrlDecision::scheme_not_allowed);
    EXPECT_EQ(outcome.resolver_calls, 0);
}

TEST(AuthMetadataSsrfTest, RefusesAPrivateRangeTargetWithoutResolving) {
    const auto outcome = refuse_challenge(R"(Bearer resource_metadata="http://10.10.10.10/prm")",
                                          {"http://10.10.10.10"});
    EXPECT_TRUE(outcome.threw);
    EXPECT_EQ(outcome.decision, mcp::auth::MetadataUrlDecision::scheme_not_allowed);
    EXPECT_EQ(outcome.resolver_calls, 0);
}

TEST(AuthMetadataSsrfTest, RefusesPlainHttpToANonLoopbackHostWithoutResolving) {
    const auto outcome =
        refuse_challenge(R"(Bearer resource_metadata="http://metadata.example.test/prm")",
                         {"http://metadata.example.test"});
    EXPECT_TRUE(outcome.threw);
    EXPECT_EQ(outcome.decision, mcp::auth::MetadataUrlDecision::scheme_not_allowed);
    EXPECT_EQ(outcome.resolver_calls, 0);
}

TEST(AuthMetadataSsrfTest, RefusesAnUnlistedOriginWithoutResolving) {
    const auto outcome =
        refuse_challenge(R"(Bearer resource_metadata="https://unlisted.example.test/prm")", {});
    EXPECT_TRUE(outcome.threw);
    EXPECT_EQ(outcome.decision, mcp::auth::MetadataUrlDecision::origin_not_allowed);
    EXPECT_EQ(outcome.resolver_calls, 0);
}

// Address-level refusal. The server below is live and would accept instantly, and one of the two
// resolved answers points straight at it, so a non-zero accept count means a socket was opened
// despite the other answer being blocked.
TEST(AuthMetadataSsrfTest, RefusesTheWholeAnswerWhenOneResolvedAddressIsBlocked) {
    asio::io_context io_ctx;
    LoopbackServer canary(io_ctx);

    canary.set_handler([](const http::request<http::string_body>&) {
        return json_response({{"resource", "http://unused"}});
    });
    asio::co_spawn(io_ctx, canary.serve(1), asio::detached);

    int resolver_calls = 0;
    bool refused = false;
    auto decision = mcp::auth::MetadataUrlDecision::allowed;

    asio::co_spawn(
        io_ctx,
        [&]() -> mcp::Task<void> {
            mcp::auth::OAuthHttpClient client(io_ctx.get_executor());
            auto policy = loopback_policy("http://localhost:" + std::to_string(canary.port()));
            client.set_metadata_policy(policy);
            client.set_host_resolver(
                [&](const std::string&, const std::string&) -> std::vector<std::string> {
                    ++resolver_calls;
                    return {"127.0.0.1", "169.254.169.254"};
                });
            try {
                (void)co_await client.get_json("http://localhost:" + std::to_string(canary.port()) +
                                               "/prm");
            } catch (const mcp::auth::MetadataPolicyError& error) {
                refused = true;
                decision = error.decision();
            }
            canary.close();
        },
        asio::detached);

    io_ctx.run();

    EXPECT_TRUE(refused);
    EXPECT_EQ(decision, mcp::auth::MetadataUrlDecision::address_link_local);
    EXPECT_EQ(resolver_calls, 1);
    EXPECT_EQ(canary.accepts(), 0);
}

// Resolve-then-pin: the addresses of one lookup are used for the connection and no second lookup
// happens, so a name that rebinds after the first answer cannot redirect the fetch.
TEST(AuthMetadataSsrfTest, PinsTheFirstResolutionSoRebindingCannotRedirectTheFetch) {
    asio::io_context io_ctx;
    LoopbackServer server(io_ctx);

    server.set_handler([](const http::request<http::string_body>&) {
        return json_response(
            {{"resource", "http://pinned"}, {"authorization_servers", json::array({"http://pinned"})}});
    });
    asio::co_spawn(io_ctx, server.serve(1), asio::detached);

    int resolver_calls = 0;
    std::string resolved_resource;

    asio::co_spawn(
        io_ctx,
        [&]() -> mcp::Task<void> {
            mcp::auth::OAuthHttpClient client(io_ctx.get_executor());
            client.set_metadata_policy(
                loopback_policy("http://localhost:" + std::to_string(server.port())));
            client.set_host_resolver(
                [&](const std::string&, const std::string&) -> std::vector<std::string> {
                    ++resolver_calls;
                    // A rebinding resolver: benign first, hostile on any later lookup.
                    if (resolver_calls == 1) {
                        return {"127.0.0.1"};
                    }
                    return {"169.254.169.254"};
                });
            const auto document =
                co_await client.get_json("http://localhost:" + std::to_string(server.port()) + "/prm");
            resolved_resource = document.at("resource").get<std::string>();
            server.close();
        },
        asio::detached);

    io_ctx.run();

    EXPECT_EQ(resolved_resource, "http://pinned");
    EXPECT_EQ(resolver_calls, 1);
    EXPECT_EQ(server.accepts(), 1);
}

TEST(AuthMetadataRedirectTest, RefusesARedirectOntoABlockedTarget) {
    asio::io_context io_ctx;
    LoopbackServer server(io_ctx);

    server.set_handler([](const http::request<http::string_body>&) {
        http::response<http::string_body> response{http::status::found, 11};
        response.set(http::field::location, "http://169.254.169.254/latest/meta-data");
        return response;
    });
    asio::co_spawn(io_ctx, server.serve(2), asio::detached);

    bool refused = false;
    auto decision = mcp::auth::MetadataUrlDecision::allowed;

    asio::co_spawn(
        io_ctx,
        [&]() -> mcp::Task<void> {
            mcp::auth::OAuthHttpClient client(io_ctx.get_executor());
            auto policy = loopback_policy(server.origin());
            policy.allowed_origins.emplace_back("http://169.254.169.254");
            client.set_metadata_policy(policy);
            try {
                (void)co_await client.get_json(server.base_url() + "/prm");
            } catch (const mcp::auth::MetadataPolicyError& error) {
                refused = true;
                decision = error.decision();
            }
            server.close();
        },
        asio::detached);

    io_ctx.run();

    EXPECT_TRUE(refused);
    EXPECT_EQ(decision, mcp::auth::MetadataUrlDecision::scheme_not_allowed);
    // Only the original hop was made; the redirect target was refused rather than followed.
    EXPECT_EQ(server.accepts(), 1);
}

TEST(AuthMetadataRedirectTest, BoundsTheRedirectChain) {
    asio::io_context io_ctx;
    LoopbackServer server(io_ctx);

    server.set_handler([](const http::request<http::string_body>&) {
        http::response<http::string_body> response{http::status::found, 11};
        response.set(http::field::location, "/again");
        return response;
    });
    asio::co_spawn(io_ctx, server.serve(5), asio::detached);

    bool refused = false;
    auto decision = mcp::auth::MetadataUrlDecision::allowed;

    asio::co_spawn(
        io_ctx,
        [&]() -> mcp::Task<void> {
            mcp::auth::OAuthHttpClient client(io_ctx.get_executor());
            auto policy = loopback_policy(server.origin());
            policy.max_redirects = 2;
            client.set_metadata_policy(policy);
            try {
                (void)co_await client.get_json(server.base_url() + "/prm");
            } catch (const mcp::auth::MetadataPolicyError& error) {
                refused = true;
                decision = error.decision();
            }
            server.close();
        },
        asio::detached);

    io_ctx.run();

    EXPECT_TRUE(refused);
    EXPECT_EQ(decision, mcp::auth::MetadataUrlDecision::redirect_limit_exceeded);
    // The initial request plus exactly two redirects.
    EXPECT_EQ(server.accepts(), 3);
}

TEST(AuthMetadataSizeCapTest, RejectsAnOversizedMetadataResponse) {
    asio::io_context io_ctx;
    LoopbackServer server(io_ctx);

    server.set_handler([](const http::request<http::string_body>&) {
        http::response<http::string_body> response{http::status::ok, 11};
        response.set(http::field::content_type, "application/json");
        response.body() = json{{"padding", std::string(64 * 1024, 'x')}}.dump();
        return response;
    });
    asio::co_spawn(io_ctx, server.serve(1), asio::detached);

    bool refused = false;
    auto decision = mcp::auth::MetadataUrlDecision::allowed;

    asio::co_spawn(
        io_ctx,
        [&]() -> mcp::Task<void> {
            mcp::auth::OAuthHttpClient client(io_ctx.get_executor());
            auto policy = loopback_policy(server.origin());
            policy.max_response_bytes = 4096;
            client.set_metadata_policy(policy);
            try {
                (void)co_await client.get_json(server.base_url() + "/prm");
            } catch (const mcp::auth::MetadataPolicyError& error) {
                refused = true;
                decision = error.decision();
            }
            server.close();
        },
        asio::detached);

    io_ctx.run();

    EXPECT_TRUE(refused);
    EXPECT_EQ(decision, mcp::auth::MetadataUrlDecision::response_too_large);
}

TEST(AuthMetadataSizeCapTest, AcceptsAResponseWithinTheCap) {
    asio::io_context io_ctx;
    LoopbackServer server(io_ctx);

    server.set_handler([](const http::request<http::string_body>&) {
        return json_response({{"resource", "http://small"}});
    });
    asio::co_spawn(io_ctx, server.serve(1), asio::detached);

    std::string resource;
    asio::co_spawn(
        io_ctx,
        [&]() -> mcp::Task<void> {
            mcp::auth::OAuthHttpClient client(io_ctx.get_executor());
            auto policy = loopback_policy(server.origin());
            policy.max_response_bytes = 4096;
            client.set_metadata_policy(policy);
            const auto document = co_await client.get_json(server.base_url() + "/prm");
            resource = document.at("resource").get<std::string>();
            server.close();
        },
        asio::detached);

    io_ctx.run();
    EXPECT_EQ(resource, "http://small");
}

// End-to-end: an unauthenticated MCP request draws a challenge, the challenge drives a full
// authorization exchange, and the original request is replayed byte-for-byte with the new token.
TEST(AuthChallengeReplayTest, ReplaysTheExactRequestAfterChallengeDrivenAuthorization) {
    asio::io_context io_ctx;
    LoopbackServer server(io_ctx);
    const auto base = server.base_url();

    server.set_handler([&base](const http::request<http::string_body>& request) {
        const std::string target(request.target());
        if (target == "/mcp") {
            if (request[http::field::authorization].empty()) {
                http::response<http::string_body> challenge{http::status::unauthorized, 11};
                challenge.set(http::field::www_authenticate,
                              R"(Bearer realm="mcp", resource_metadata=")" + base + R"(/prm")");
                return challenge;
            }
            return status_response(http::status::accepted);
        }
        if (target == "/prm") {
            return json_response(
                {{"resource", base + "/mcp"}, {"authorization_servers", json::array({base})}});
        }
        if (target == "/.well-known/oauth-authorization-server") {
            return json_response(auth_server_metadata(base, true));
        }
        if (target == "/token") {
            return json_response(token_document());
        }
        return status_response(http::status::not_found);
    });
    asio::co_spawn(io_ctx, server.serve(5), asio::detached);

    mcp::auth::OAuthAuthorizationConfig config;
    config.server_url = base + "/mcp";
    config.client_id = "test-client";
    config.redirect_uri = "http://127.0.0.1:9999/callback";
    config.policy = loopback_policy(server.origin());

    auto store = std::make_shared<mcp::auth::InMemoryTokenStore>();
    const std::string wire = R"({"jsonrpc":"2.0","id":1,"method":"ping"})";
    std::exception_ptr failure;

    asio::co_spawn(
        io_ctx,
        [&]() -> mcp::Task<void> {
            auto inner =
                std::make_shared<mcp::HttpClientTransport>(io_ctx.get_executor(), base + "/mcp");
            auto manager = std::make_shared<mcp::auth::OAuthAuthorizationManager>(
                io_ctx.get_executor(), store, config, echoing_callback(nullptr));
            mcp::auth::OAuthClientTransport transport(inner, manager);
            try {
                co_await transport.write_message(wire);
            } catch (...) {
                failure = std::current_exception();
            }
            transport.close();
            server.close();
        },
        asio::detached);

    io_ctx.run();

    ASSERT_EQ(failure, nullptr);
    ASSERT_EQ(server.targets().size(), 5U);
    EXPECT_EQ(server.targets()[0], "/mcp");
    EXPECT_EQ(server.targets()[1], "/prm");
    EXPECT_EQ(server.targets()[2], "/.well-known/oauth-authorization-server");
    EXPECT_EQ(server.targets()[3], "/token");
    EXPECT_EQ(server.targets()[4], "/mcp");

    // The replay is the same request, not a reconstruction, and it now carries the acquired token.
    EXPECT_EQ(server.bodies()[4], server.bodies()[0]);
    EXPECT_EQ(server.bodies()[4], wire);
    EXPECT_TRUE(server.authorizations()[0].empty());
    EXPECT_EQ(server.authorizations()[4], "Bearer granted-access-token");
}

namespace {

/// Consent callback that records the scope of every authorization request it is asked to run.
mcp::auth::AuthorizationCallback recording_callback(std::vector<std::string>* scopes) {
    return [scopes](const mcp::auth::AuthorizationRequest& request)
               -> mcp::Task<mcp::auth::AuthorizationResponse> {
        scopes->push_back(request.scope.value_or(""));
        mcp::auth::AuthorizationResponse response;
        response.code = "test-authorization-code";
        response.state = request.state;
        response.iss = request.issuer;
        co_return response;
    };
}

/// True when every space-separated scope in `needle` appears in `haystack`.
bool has_scopes(const std::string& haystack, const std::vector<std::string>& needle) {
    std::vector<std::string> present;
    std::string current;
    for (const char character : haystack) {
        if (character == ' ') {
            if (!current.empty()) {
                present.push_back(current);
            }
            current.clear();
            continue;
        }
        current.push_back(character);
    }
    if (!current.empty()) {
        present.push_back(current);
    }
    for (const auto& wanted : needle) {
        if (std::find(present.begin(), present.end(), wanted) == present.end()) {
            return false;
        }
    }
    return true;
}

}  // namespace

TEST(AuthScopeStepUpTest, UnionsTheGrantedScopeWithAForbiddenChallengeAndReplaysTheRequest) {
    asio::io_context io_ctx;
    LoopbackServer server(io_ctx);
    const auto base = server.base_url();

    int mcp_calls = 0;
    server.set_handler([&](const http::request<http::string_body>& request) {
        const std::string target(request.target());
        if (target == "/mcp") {
            ++mcp_calls;
            if (mcp_calls == 1) {
                // Unauthenticated: the challenge names a scope that is disjoint from the PRM's
                // `scopes_supported`, and the challenge must still win.
                http::response<http::string_body> challenge{http::status::unauthorized, 11};
                challenge.set(http::field::www_authenticate,
                              R"(Bearer scope="mcp:basic", resource_metadata=")" + base + R"(/prm")");
                return challenge;
            }
            if (mcp_calls == 2) {
                // Step-up: the token is valid, but this operation needs a scope the grant lacks.
                http::response<http::string_body> challenge{http::status::forbidden, 11};
                challenge.set(http::field::www_authenticate,
                              R"(Bearer error="insufficient_scope", scope="mcp:write", )"
                              R"(resource_metadata=")" +
                                  base + R"(/prm")");
                return challenge;
            }
            return status_response(http::status::accepted);
        }
        if (target == "/prm") {
            return json_response({{"resource", base + "/mcp"},
                                  {"authorization_servers", json::array({base})},
                                  {"scopes_supported", json::array({"prm:only"})}});
        }
        if (target == "/.well-known/oauth-authorization-server") {
            return json_response(auth_server_metadata(base, true));
        }
        if (target == "/token") {
            return json_response(token_document());
        }
        return status_response(http::status::not_found);
    });
    asio::co_spawn(io_ctx, server.serve(20), asio::detached);

    mcp::auth::OAuthAuthorizationConfig config;
    config.server_url = base + "/mcp";
    config.client_id = "test-client";
    config.redirect_uri = "http://127.0.0.1:9999/callback";
    config.policy = loopback_policy(server.origin());

    auto store = std::make_shared<mcp::auth::InMemoryTokenStore>();
    std::vector<std::string> requested_scopes;
    const std::string wire = R"({"jsonrpc":"2.0","id":1,"method":"tools/call"})";
    std::exception_ptr failure;

    asio::co_spawn(
        io_ctx,
        [&]() -> mcp::Task<void> {
            auto inner =
                std::make_shared<mcp::HttpClientTransport>(io_ctx.get_executor(), base + "/mcp");
            auto manager = std::make_shared<mcp::auth::OAuthAuthorizationManager>(
                io_ctx.get_executor(), store, config, recording_callback(&requested_scopes));
            mcp::auth::OAuthClientTransport transport(inner, manager);
            try {
                co_await transport.write_message(wire);
            } catch (...) {
                failure = std::current_exception();
            }
            transport.close();
            server.close();
        },
        asio::detached);

    io_ctx.run();

    ASSERT_EQ(failure, nullptr);
    ASSERT_EQ(requested_scopes.size(), 2U);

    // The challenge scope is authoritative even though it shares nothing with `scopes_supported`:
    // no set relationship between the two may be assumed in either direction.
    EXPECT_EQ(requested_scopes[0], "mcp:basic");
    EXPECT_FALSE(has_scopes(requested_scopes[0], {"prm:only"}));

    // Step-up re-authorizes on the union, so the scope already granted survives the escalation.
    EXPECT_TRUE(has_scopes(requested_scopes[1], {"mcp:basic", "mcp:write"}));
    EXPECT_FALSE(has_scopes(requested_scopes[1], {"prm:only"}));

    // The replay is the same request bytes, carrying the newly acquired token.
    EXPECT_EQ(mcp_calls, 3);
    ASSERT_FALSE(server.bodies().empty());
    EXPECT_EQ(server.bodies().back(), wire);
    EXPECT_EQ(server.authorizations().back(), "Bearer granted-access-token");
}

TEST(AuthScopeStepUpTest, StopsAfterThreeAuthorizationChallengesForOneRequest) {
    asio::io_context io_ctx;
    LoopbackServer server(io_ctx);
    const auto base = server.base_url();

    server.set_handler([&](const http::request<http::string_body>& request) {
        const std::string target(request.target());
        if (target == "/mcp") {
            // A scope escalation that will never succeed, exactly as the retry-limit fixture does.
            if (request[http::field::authorization].empty()) {
                http::response<http::string_body> challenge{http::status::unauthorized, 11};
                challenge.set(http::field::www_authenticate,
                              R"(Bearer scope="mcp:admin", resource_metadata=")" + base + R"(/prm")");
                return challenge;
            }
            http::response<http::string_body> challenge{http::status::forbidden, 11};
            challenge.set(http::field::www_authenticate,
                          R"(Bearer error="insufficient_scope", scope="mcp:admin", )"
                          R"(resource_metadata=")" +
                              base + R"(/prm")");
            return challenge;
        }
        if (target == "/prm") {
            return json_response({{"resource", base + "/mcp"},
                                  {"authorization_servers", json::array({base})},
                                  {"scopes_supported", json::array({"mcp:admin"})}});
        }
        if (target == "/.well-known/oauth-authorization-server") {
            return json_response(auth_server_metadata(base, true));
        }
        if (target == "/token") {
            return json_response(token_document());
        }
        return status_response(http::status::not_found);
    });
    asio::co_spawn(io_ctx, server.serve(40), asio::detached);

    mcp::auth::OAuthAuthorizationConfig config;
    config.server_url = base + "/mcp";
    config.client_id = "test-client";
    config.redirect_uri = "http://127.0.0.1:9999/callback";
    config.policy = loopback_policy(server.origin());

    auto store = std::make_shared<mcp::auth::InMemoryTokenStore>();
    std::vector<std::string> requested_scopes;
    std::exception_ptr failure;

    asio::co_spawn(
        io_ctx,
        [&]() -> mcp::Task<void> {
            auto inner =
                std::make_shared<mcp::HttpClientTransport>(io_ctx.get_executor(), base + "/mcp");
            auto manager = std::make_shared<mcp::auth::OAuthAuthorizationManager>(
                io_ctx.get_executor(), store, config, recording_callback(&requested_scopes));
            mcp::auth::OAuthClientTransport transport(inner, manager);
            try {
                co_await transport.write_message(R"({"jsonrpc":"2.0","id":1,"method":"tools/call"})");
            } catch (...) {
                failure = std::current_exception();
            }
            transport.close();
            server.close();
        },
        asio::detached);

    io_ctx.run();

    // The refusal is surfaced rather than retried forever, and the cap is three per request.
    EXPECT_NE(failure, nullptr);
    EXPECT_EQ(requested_scopes.size(), 3U);
}

TEST(AuthScopeStepUpTest, CoalescesConcurrentChallengesIntoASingleAuthorizationFlow) {
    asio::io_context io_ctx;
    LoopbackServer server(io_ctx);
    const auto base = server.base_url();

    server.set_handler([&base](const http::request<http::string_body>& request) {
        const std::string target(request.target());
        if (target == "/prm") {
            return json_response(
                {{"resource", base + "/mcp"}, {"authorization_servers", json::array({base})}});
        }
        if (target == "/.well-known/oauth-authorization-server") {
            return json_response(auth_server_metadata(base, true));
        }
        if (target == "/token") {
            return json_response(token_document());
        }
        return status_response(http::status::not_found);
    });
    asio::co_spawn(io_ctx, server.serve(10), asio::detached);

    mcp::auth::OAuthAuthorizationConfig config;
    config.server_url = base + "/mcp";
    config.client_id = "test-client";
    config.redirect_uri = "http://127.0.0.1:9999/callback";
    config.policy = loopback_policy(server.origin());

    auto store = std::make_shared<mcp::auth::InMemoryTokenStore>();
    std::vector<std::string> requested_scopes;
    const std::string header = R"(Bearer scope="mcp:basic", resource_metadata=")" + base + R"(/prm")";

    auto manager = std::make_shared<mcp::auth::OAuthAuthorizationManager>(
        io_ctx.get_executor(), store, config, recording_callback(&requested_scopes));

    int completed = 0;
    int succeeded = 0;
    const auto challenger = [&]() -> mcp::Task<void> {
        const auto authorized = co_await manager->try_handle_challenge(header);
        succeeded += authorized ? 1 : 0;
        if (++completed == 2) {
            server.close();
        }
    };
    asio::co_spawn(io_ctx, challenger(), asio::detached);
    asio::co_spawn(io_ctx, challenger(), asio::detached);

    io_ctx.run();

    // Both callers are authorized, but only one of them ran a flow.
    EXPECT_EQ(completed, 2);
    EXPECT_EQ(succeeded, 2);
    EXPECT_EQ(requested_scopes.size(), 1U);
    EXPECT_EQ(store->load(config.server_url).has_value(), true);
}

TEST(AuthIssuerBindingTest, RejectsAuthServerMetadataWhoseIssuerIsNotItsDiscoveryLocation) {
    asio::io_context io_ctx;
    LoopbackServer server(io_ctx);
    const auto base = server.base_url();

    server.set_handler([&base](const http::request<http::string_body>& request) {
        const std::string target(request.target());
        if (target == "/mcp") {
            http::response<http::string_body> challenge{http::status::unauthorized, 11};
            challenge.set(http::field::www_authenticate,
                          R"(Bearer resource_metadata=")" + base + R"(/prm")");
            return challenge;
        }
        if (target == "/prm") {
            return json_response(
                {{"resource", base + "/mcp"}, {"authorization_servers", json::array({base})}});
        }
        if (target == "/.well-known/oauth-authorization-server") {
            // The document claims to be a different authorization server than the one it was
            // fetched from -- RFC 8414 3.3 makes that a hard refusal, not a normalization problem.
            auto metadata = auth_server_metadata(base, true);
            metadata["issuer"] = "https://other.example.com";
            return json_response(metadata);
        }
        return status_response(http::status::not_found);
    });
    asio::co_spawn(io_ctx, server.serve(5), asio::detached);

    mcp::auth::OAuthAuthorizationConfig config;
    config.server_url = base + "/mcp";
    config.client_id = "test-client";
    config.redirect_uri = "http://127.0.0.1:9999/callback";
    config.policy = loopback_policy(server.origin());

    auto store = std::make_shared<mcp::auth::InMemoryTokenStore>();
    std::vector<std::string> requested_scopes;
    bool threw = false;

    asio::co_spawn(
        io_ctx,
        [&]() -> mcp::Task<void> {
            auto manager = std::make_shared<mcp::auth::OAuthAuthorizationManager>(
                io_ctx.get_executor(), store, config, recording_callback(&requested_scopes));
            try {
                (void)co_await manager->try_handle_challenge(R"(Bearer resource_metadata=")" + base +
                                                             R"(/prm")");
            } catch (const std::exception&) {
                threw = true;
            }
            server.close();
        },
        asio::detached);

    io_ctx.run();

    EXPECT_TRUE(threw);
    // Refused before any consent prompt or token exchange could bind a credential to the wrong AS.
    EXPECT_TRUE(requested_scopes.empty());
    EXPECT_FALSE(store->load(config.server_url).has_value());
}

TEST(AuthClientIdentityBindingTest, RefusesInjectedCredentialsBoundToADifferentIssuer) {
    mcp::auth::ClientIdentityConfig config;
    mcp::auth::OAuthClientInformation injected;
    injected.client_id = "client-for-as-one";
    injected.client_secret = "secret-for-as-one";
    injected.issuer = "https://as-one.example.com";
    config.pre_registered = injected;

    mcp::auth::ClientIdentityServerFacts other;
    other.issuer = "https://as-two.example.com";
    // A registration endpoint is on offer, and it must still not be taken: injected credentials are
    // terminal, so a misbound secret yields no identity rather than a silent dynamic registration.
    other.registration_endpoint = "https://as-two.example.com/register";

    EXPECT_EQ(mcp::auth::select_client_identity(config, other, std::nullopt),
              mcp::auth::ClientIdentityDecision::unavailable);

    mcp::auth::ClientIdentityServerFacts matching;
    matching.issuer = "https://as-one.example.com";
    EXPECT_EQ(mcp::auth::select_client_identity(config, matching, std::nullopt),
              mcp::auth::ClientIdentityDecision::use_pre_registered);

    // Credentials that never named an issuer keep their existing single-server behaviour.
    config.pre_registered->issuer.clear();
    EXPECT_EQ(mcp::auth::select_client_identity(config, other, std::nullopt),
              mcp::auth::ClientIdentityDecision::use_pre_registered);
}

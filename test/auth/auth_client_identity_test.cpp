/**
 * @file auth_client_identity_test.cpp
 * @brief Tests for client identity selection (Slice B): metadata documents, injected credentials,
 *        dynamic registration as the last resort, and the issuer binding that keeps one
 *        authorization server's credentials away from another.
 *
 * The loopback cases are the primary evidence for the slice; none of them involves a conformance
 * runner.
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
#include <mcp/auth/oauth.hpp>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using json = nlohmann::json;

namespace {

/// Loopback HTTP server that answers a scripted handler and records every request it served.
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

    /// @return The `Authorization` header of the first request served for a target.
    [[nodiscard]] std::string authorization_for(const std::string& target) const {
        for (std::size_t index = 0; index < targets_.size(); ++index) {
            if (targets_[index] == target) {
                return authorizations_[index];
            }
        }
        return {};
    }

    /// @return How many requests were served for a target.
    [[nodiscard]] std::size_t count(const std::string& target) const {
        return static_cast<std::size_t>(std::count(targets_.begin(), targets_.end(), target));
    }

    /// @return The body of the first request served for a target, or an empty string.
    [[nodiscard]] std::string body_for(const std::string& target) const {
        for (std::size_t index = 0; index < targets_.size(); ++index) {
            if (targets_[index] == target) {
                return bodies_[index];
            }
        }
        return {};
    }

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
};

http::response<http::string_body> json_response(const json& body,
                                                http::status status = http::status::ok) {
    http::response<http::string_body> response{status, 11};
    response.set(http::field::content_type, "application/json");
    response.body() = body.dump();
    return response;
}

http::response<http::string_body> not_found() {
    http::response<http::string_body> response{http::status::not_found, 11};
    response.body() = "{}";
    return response;
}

json token_document() {
    return {{"access_token", "granted-access-token"}, {"token_type", "Bearer"}, {"expires_in", 3600}};
}

mcp::auth::MetadataFetchPolicy loopback_policy(const std::string& origin) {
    mcp::auth::MetadataFetchPolicy policy;
    policy.allowed_origins.push_back(origin);
    policy.allow_plain_http_loopback = true;
    return policy;
}

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

mcp::auth::ClientIdentityServerFacts facts_with(std::string issuer, bool cimd_supported,
                                                std::optional<std::string> registration_endpoint) {
    mcp::auth::ClientIdentityServerFacts facts;
    facts.issuer = std::move(issuer);
    facts.client_id_metadata_document_supported = cimd_supported;
    facts.registration_endpoint = std::move(registration_endpoint);
    return facts;
}

mcp::auth::OAuthClientInformation credentials(std::string client_id, std::string issuer) {
    mcp::auth::OAuthClientInformation information;
    information.client_id = std::move(client_id);
    information.issuer = std::move(issuer);
    return information;
}

}  // namespace

TEST(AuthClientIdentitySelectionTest, InjectedCredentialsWinOverEveryOtherPath) {
    mcp::auth::ClientIdentityConfig config;
    config.pre_registered = credentials("injected-client", "https://issuer.example");
    config.client_metadata_url = "https://client.example/metadata.json";

    const auto decision = mcp::auth::select_client_identity(
        config, facts_with("https://issuer.example", true, "https://issuer.example/register"),
        credentials("stored-client", "https://issuer.example"));

    EXPECT_EQ(decision, mcp::auth::ClientIdentityDecision::use_pre_registered);
}

TEST(AuthClientIdentitySelectionTest, InjectedCredentialsNeverFallBackToRegistration) {
    mcp::auth::ClientIdentityConfig config;
    config.pre_registered = credentials("injected-client", "https://issuer.example");

    // Every other path is available and none of them is taken.
    const auto decision = mcp::auth::select_client_identity(
        config, facts_with("https://issuer.example", true, "https://issuer.example/register"),
        std::nullopt);

    EXPECT_EQ(decision, mcp::auth::ClientIdentityDecision::use_pre_registered);
}

TEST(AuthClientIdentitySelectionTest, PrefersTheMetadataDocumentWhenTheServerAdvertisesIt) {
    mcp::auth::ClientIdentityConfig config;
    config.client_metadata_url = "https://client.example/metadata.json";

    const auto decision = mcp::auth::select_client_identity(
        config, facts_with("https://issuer.example", true, "https://issuer.example/register"),
        std::nullopt);

    EXPECT_EQ(decision, mcp::auth::ClientIdentityDecision::use_client_id_metadata_document);
}

TEST(AuthClientIdentitySelectionTest, IgnoresTheMetadataDocumentWhenTheServerDoesNotAdvertiseIt) {
    mcp::auth::ClientIdentityConfig config;
    config.client_metadata_url = "https://client.example/metadata.json";

    const auto decision = mcp::auth::select_client_identity(
        config, facts_with("https://issuer.example", false, "https://issuer.example/register"),
        std::nullopt);

    EXPECT_EQ(decision, mcp::auth::ClientIdentityDecision::register_dynamically);
}

TEST(AuthClientIdentitySelectionTest, ReusesCredentialsRecordedAgainstTheSameIssuer) {
    const auto decision = mcp::auth::select_client_identity(
        {}, facts_with("https://issuer.example", false, "https://issuer.example/register"),
        credentials("stored-client", "https://issuer.example"));

    EXPECT_EQ(decision, mcp::auth::ClientIdentityDecision::reuse_stored_registration);
}

TEST(AuthClientIdentitySelectionTest, RegistersAfreshWhenTheStoredIssuerIsADifferentServer) {
    const auto decision = mcp::auth::select_client_identity(
        {}, facts_with("https://second.example", false, "https://second.example/register"),
        credentials("stored-client", "https://first.example"));

    EXPECT_EQ(decision, mcp::auth::ClientIdentityDecision::register_dynamically);
}

TEST(AuthClientIdentitySelectionTest, ReportsUnavailableWhenNoPathRemains) {
    const auto decision = mcp::auth::select_client_identity(
        {}, facts_with("https://issuer.example", true, std::nullopt), std::nullopt);

    EXPECT_EQ(decision, mcp::auth::ClientIdentityDecision::unavailable);
}

TEST(AuthClientRegistrationRequestTest, CarriesApplicationTypeAndTheRefreshTokenGrant) {
    mcp::auth::OAuthClientMetadata metadata;
    metadata.redirect_uris = {"http://127.0.0.1:9999/callback"};
    metadata.client_name = "conformance-client";

    const auto body = mcp::auth::build_registration_request(
        metadata, facts_with("https://issuer.example", false, "https://issuer.example/register"));

    EXPECT_EQ(body.at("application_type"), "native");
    EXPECT_EQ(body.at("grant_types"), json::array({"authorization_code", "refresh_token"}));
    EXPECT_EQ(body.at("response_types"), json::array({"code"}));
    EXPECT_EQ(body.at("redirect_uris"), json::array({"http://127.0.0.1:9999/callback"}));
    EXPECT_EQ(body.at("client_name"), "conformance-client");
    EXPECT_FALSE(body.contains("scope"));
}

TEST(AuthClientRegistrationRequestTest, RequestsOfflineAccessOnlyWhenTheServerAdvertisesIt) {
    mcp::auth::OAuthClientMetadata metadata;
    metadata.redirect_uris = {"http://127.0.0.1:9999/callback"};
    metadata.scope = "mcp:read";

    auto silent = facts_with("https://issuer.example", false, "https://issuer.example/register");
    silent.scopes_supported = {"mcp:read", "mcp:write"};
    EXPECT_EQ(mcp::auth::build_registration_request(metadata, silent).at("scope"), "mcp:read");

    auto advertising = silent;
    advertising.scopes_supported.emplace_back("offline_access");
    EXPECT_EQ(mcp::auth::build_registration_request(metadata, advertising).at("scope"),
              "mcp:read offline_access");
}

TEST(AuthClientRegistrationRequestTest, DoesNotRepeatOfflineAccessAlreadyRequested) {
    mcp::auth::OAuthClientMetadata metadata;
    metadata.scope = "mcp:read offline_access";

    auto facts = facts_with("https://issuer.example", false, "https://issuer.example/register");
    facts.scopes_supported = {"offline_access"};

    EXPECT_EQ(mcp::auth::build_registration_request(metadata, facts).at("scope"),
              "mcp:read offline_access");
}

TEST(AuthClientInformationTest, ReportsSecretExpiryOnlyForANonZeroExpiryInThePast) {
    auto information = credentials("client", "https://issuer.example");
    EXPECT_FALSE(information.secret_expired(1000));

    information.client_secret_expires_at = 0;  // RFC 7591: zero means the secret never expires.
    EXPECT_FALSE(information.secret_expired(1000));

    information.client_secret_expires_at = 2000;
    EXPECT_FALSE(information.secret_expired(1000));

    information.client_secret_expires_at = 999;
    EXPECT_TRUE(information.secret_expired(1000));
}

TEST(AuthClientInformationTest, SerializesTheIssuerBindingAlongsideTheCredentials) {
    auto information = credentials("client", "https://issuer.example");
    information.client_secret = "secret";
    information.client_secret_expires_at = 1234;

    const nlohmann::json serialized = information;
    EXPECT_EQ(serialized.at("client_id"), "client");
    EXPECT_EQ(serialized.at("client_secret"), "secret");
    EXPECT_EQ(serialized.at("client_secret_expires_at"), 1234);
    EXPECT_EQ(serialized.at("issuer"), "https://issuer.example");

    // The wire format has no issuer, so a round trip recovers everything the server sent and the
    // caller re-applies the binding.
    const auto parsed = serialized.get<mcp::auth::OAuthClientInformation>();
    EXPECT_EQ(parsed.client_id, "client");
    EXPECT_EQ(parsed.client_secret, "secret");
    EXPECT_EQ(parsed.client_secret_expires_at, 1234);
    EXPECT_TRUE(parsed.issuer.empty());
}

TEST(AuthClientIdentitySelectionTest, DescribesEverySourceAndDecision) {
    EXPECT_FALSE(mcp::auth::describe(mcp::auth::ClientIdentitySource::pre_registered).empty());
    EXPECT_FALSE(
        mcp::auth::describe(mcp::auth::ClientIdentitySource::client_id_metadata_document).empty());
    EXPECT_FALSE(mcp::auth::describe(mcp::auth::ClientIdentitySource::dynamic_registration).empty());
    EXPECT_FALSE(mcp::auth::describe(mcp::auth::ClientIdentityDecision::use_pre_registered).empty());
    EXPECT_FALSE(mcp::auth::describe(mcp::auth::ClientIdentityDecision::use_client_id_metadata_document)
                     .empty());
    EXPECT_FALSE(
        mcp::auth::describe(mcp::auth::ClientIdentityDecision::reuse_stored_registration).empty());
    EXPECT_FALSE(mcp::auth::describe(mcp::auth::ClientIdentityDecision::register_dynamically).empty());
    EXPECT_FALSE(mcp::auth::describe(mcp::auth::ClientIdentityDecision::unavailable).empty());
}

TEST(AuthMetadataOriginAllowanceTest, WidensTheAllowListWithoutOverridingTheDenyList) {
    mcp::auth::MetadataFetchPolicy policy;
    policy.allow_plain_http_loopback = true;
    policy.denied_origins.emplace_back("http://127.0.0.1:9");

    // With no hook, an unlisted origin is refused; the hook admits exactly what it says yes to.
    EXPECT_EQ(mcp::auth::validate_metadata_url(policy, "http://127.0.0.1:1234/prm"),
              mcp::auth::MetadataUrlDecision::origin_not_allowed);

    policy.origin_allowance = [](const std::string& origin) {
        return origin == "http://127.0.0.1:1234";
    };
    EXPECT_EQ(mcp::auth::validate_metadata_url(policy, "http://127.0.0.1:1234/prm"),
              mcp::auth::MetadataUrlDecision::allowed);
    EXPECT_EQ(mcp::auth::validate_metadata_url(policy, "http://127.0.0.1:5678/prm"),
              mcp::auth::MetadataUrlDecision::origin_not_allowed);

    // A denied origin stays denied even when the hook would admit it.
    policy.origin_allowance = [](const std::string&) { return true; };
    EXPECT_EQ(mcp::auth::validate_metadata_url(policy, "http://127.0.0.1:9/prm"),
              mcp::auth::MetadataUrlDecision::origin_denied);

    // Widening the origin list does not relax any other control: the scheme rule still refuses
    // plain HTTP to a non-loopback host, and the address rules still refuse the metadata service.
    EXPECT_EQ(mcp::auth::validate_metadata_url(policy, "http://169.254.169.254/prm"),
              mcp::auth::MetadataUrlDecision::scheme_not_allowed);
    EXPECT_EQ(mcp::auth::validate_metadata_url(policy, "https://169.254.169.254/prm"),
              mcp::auth::MetadataUrlDecision::address_link_local);
}

TEST(AuthClientCredentialStoreTest, KeepsEachIssuersCredentialsSeparate) {
    mcp::auth::InMemoryClientCredentialStore store;
    store.store("https://first.example", credentials("client-1", "https://first.example"));
    store.store("https://second.example", credentials("client-2", "https://second.example"));

    ASSERT_TRUE(store.load("https://first.example").has_value());
    EXPECT_EQ(store.load("https://first.example")->client_id, "client-1");
    EXPECT_EQ(store.load("https://second.example")->client_id, "client-2");
    EXPECT_FALSE(store.load("https://third.example").has_value());

    store.remove("https://first.example");
    EXPECT_FALSE(store.load("https://first.example").has_value());
    EXPECT_TRUE(store.load("https://second.example").has_value());
}

namespace {

/// Authorization-server metadata for a fixture server rooted at `base` with issuer `base + suffix`.
json server_metadata(const std::string& base, const std::string& suffix, bool cimd_supported,
                     bool registration_supported) {
    json metadata = {{"issuer", base + suffix},
                     {"authorization_endpoint", base + suffix + "/authorize"},
                     {"token_endpoint", base + suffix + "/token"},
                     {"response_types_supported", json::array({"code"})},
                     {"code_challenge_methods_supported", json::array({"S256"})}};
    if (cimd_supported) {
        metadata["client_id_metadata_document_supported"] = true;
    }
    if (registration_supported) {
        metadata["registration_endpoint"] = base + suffix + "/register";
    }
    return metadata;
}

struct IdentityFixture {
    std::shared_ptr<mcp::auth::InMemoryTokenStore> tokens =
        std::make_shared<mcp::auth::InMemoryTokenStore>();
    std::shared_ptr<mcp::auth::InMemoryClientCredentialStore> credentials =
        std::make_shared<mcp::auth::InMemoryClientCredentialStore>();
    mcp::auth::OAuthAuthorizationConfig config;
    std::string authorization_url;
    std::optional<mcp::auth::OAuthClientInformation> identity;
    std::exception_ptr failure;
};

/// Run one challenge against a single-origin fixture and capture what the manager chose.
void run_single_challenge(LoopbackServer& server, IdentityFixture& fixture, asio::io_context& io_ctx,
                          const std::string& challenge, int request_budget) {
    asio::co_spawn(io_ctx, server.serve(request_budget), asio::detached);
    asio::co_spawn(
        io_ctx,
        [&]() -> mcp::Task<void> {
            mcp::auth::OAuthAuthorizationManager manager(io_ctx.get_executor(), fixture.tokens,
                                                         fixture.config,
                                                         echoing_callback(&fixture.authorization_url));
            try {
                (void)co_await manager.try_handle_challenge(challenge);
                fixture.identity = manager.last_client_identity();
            } catch (...) {
                fixture.failure = std::current_exception();
            }
            server.close();
        },
        asio::detached);
    io_ctx.run();
}

}  // namespace

TEST(AuthClientIdentityLoopbackTest, UsesTheMetadataDocumentUrlAsTheClientIdAndSkipsRegistration) {
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
            return json_response(server_metadata(base, "", true, true));
        }
        if (target == "/token") {
            return json_response(token_document());
        }
        return not_found();
    });

    IdentityFixture fixture;
    fixture.config.server_url = base + "/mcp";
    fixture.config.redirect_uri = "http://127.0.0.1:9999/callback";
    fixture.config.client_identity.client_metadata_url = "https://client.example/metadata.json";
    fixture.config.credential_store = fixture.credentials;
    fixture.config.policy = loopback_policy(server.origin());

    run_single_challenge(server, fixture, io_ctx, R"(Bearer resource_metadata=")" + base + R"(/prm")",
                         3);

    ASSERT_EQ(fixture.failure, nullptr);
    ASSERT_TRUE(fixture.identity.has_value());
    EXPECT_EQ(fixture.identity->source, mcp::auth::ClientIdentitySource::client_id_metadata_document);
    EXPECT_EQ(fixture.identity->client_id, "https://client.example/metadata.json");
    EXPECT_FALSE(fixture.identity->client_secret.has_value());

    // The registration endpoint was advertised and deliberately not used.
    EXPECT_EQ(server.count("/register"), 0U);
    EXPECT_NE(fixture.authorization_url.find(
                  mcp::auth::detail::url_encode("https://client.example/metadata.json")),
              std::string::npos);
    EXPECT_NE(server.body_for("/token").find(
                  "client_id=" + mcp::auth::detail::url_encode("https://client.example/metadata.json")),
              std::string::npos);

    // Nothing is persisted: the document URL is the identity, so there is no credential to bind.
    EXPECT_FALSE(fixture.credentials->load(base).has_value());
}

TEST(AuthClientIdentityLoopbackTest, PreRegisteredCredentialsAreNeverExchangedForARegistration) {
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
            // Registration is available and the server also advertises metadata documents.
            return json_response(server_metadata(base, "", true, true));
        }
        if (target == "/token") {
            return json_response(token_document());
        }
        return not_found();
    });

    IdentityFixture fixture;
    fixture.config.server_url = base + "/mcp";
    fixture.config.redirect_uri = "http://127.0.0.1:9999/callback";
    fixture.config.client_identity.pre_registered =
        credentials("application-chosen-client", std::string{});
    fixture.config.client_identity.pre_registered->client_secret = "application-chosen-secret";
    fixture.config.client_identity.client_metadata_url = "https://client.example/metadata.json";
    fixture.config.credential_store = fixture.credentials;
    fixture.config.policy = loopback_policy(server.origin());

    run_single_challenge(server, fixture, io_ctx, R"(Bearer resource_metadata=")" + base + R"(/prm")",
                         3);

    ASSERT_EQ(fixture.failure, nullptr);
    ASSERT_TRUE(fixture.identity.has_value());
    EXPECT_EQ(fixture.identity->source, mcp::auth::ClientIdentitySource::pre_registered);
    EXPECT_EQ(fixture.identity->client_id, "application-chosen-client");

    EXPECT_EQ(server.count("/register"), 0U);
    EXPECT_NE(server.body_for("/token").find("client_id=application-chosen-client"), std::string::npos);
    EXPECT_NE(server.body_for("/token").find("client_secret=application-chosen-secret"),
              std::string::npos);
    // Injected credentials belong to the application; the SDK does not persist them.
    EXPECT_FALSE(fixture.credentials->load(base).has_value());
}

TEST(AuthClientIdentityLoopbackTest, FallsBackToDynamicRegistrationAndSendsApplicationType) {
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
            auto metadata = server_metadata(base, "", false, true);
            metadata["scopes_supported"] = json::array({"mcp:read", "offline_access"});
            return json_response(metadata);
        }
        if (target == "/register") {
            return json_response({{"client_id", "registered-client"},
                                  {"client_secret", "registered-secret"},
                                  {"client_secret_expires_at", 0}},
                                 http::status::created);
        }
        if (target == "/token") {
            return json_response(token_document());
        }
        return not_found();
    });

    IdentityFixture fixture;
    fixture.config.server_url = base + "/mcp";
    fixture.config.redirect_uri = "http://127.0.0.1:9999/callback";
    fixture.config.client_identity.metadata.client_name = "mcp-cpp-sdk";
    fixture.config.credential_store = fixture.credentials;
    fixture.config.policy = loopback_policy(server.origin());

    run_single_challenge(server, fixture, io_ctx, R"(Bearer resource_metadata=")" + base + R"(/prm")",
                         4);

    ASSERT_EQ(fixture.failure, nullptr);
    ASSERT_TRUE(fixture.identity.has_value());
    EXPECT_EQ(fixture.identity->source, mcp::auth::ClientIdentitySource::dynamic_registration);
    EXPECT_EQ(fixture.identity->client_id, "registered-client");

    ASSERT_EQ(server.count("/register"), 1U);
    const auto registration = json::parse(server.body_for("/register"));
    EXPECT_EQ(registration.at("application_type"), "native");
    EXPECT_EQ(registration.at("grant_types"), json::array({"authorization_code", "refresh_token"}));
    // The redirect URI configured for authorization is the one registered.
    EXPECT_EQ(registration.at("redirect_uris"), json::array({"http://127.0.0.1:9999/callback"}));
    EXPECT_EQ(registration.at("scope"), "offline_access");

    EXPECT_NE(server.body_for("/token").find("client_id=registered-client"), std::string::npos);

    const auto stored = fixture.credentials->load(base);
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(stored->client_id, "registered-client");
    EXPECT_EQ(stored->issuer, base);
}

TEST(AuthClientIdentityLoopbackTest, KeepsCredentialsBoundToTheIssuerThatGrantedThem) {
    asio::io_context io_ctx;
    LoopbackServer server(io_ctx);
    const auto base = server.base_url();

    server.set_handler([&base](const http::request<http::string_body>& request) {
        const std::string target(request.target());
        if (target == "/prm-one") {
            return json_response(
                {{"resource", base + "/mcp"}, {"authorization_servers", json::array({base + "/as1"})}});
        }
        if (target == "/prm-two") {
            return json_response(
                {{"resource", base + "/mcp"}, {"authorization_servers", json::array({base + "/as2"})}});
        }
        if (target == "/.well-known/oauth-authorization-server/as1") {
            return json_response(server_metadata(base, "/as1", false, true));
        }
        if (target == "/.well-known/oauth-authorization-server/as2") {
            return json_response(server_metadata(base, "/as2", false, true));
        }
        if (target == "/as1/register") {
            return json_response({{"client_id", "client-for-as-1"}, {"client_secret", "secret-one"}},
                                 http::status::created);
        }
        if (target == "/as2/register") {
            return json_response({{"client_id", "client-for-as-2"}, {"client_secret", "secret-two"}},
                                 http::status::created);
        }
        if (target == "/as1/token" || target == "/as2/token") {
            return json_response(token_document());
        }
        return not_found();
    });
    asio::co_spawn(io_ctx, server.serve(8), asio::detached);

    IdentityFixture fixture;
    fixture.config.server_url = base + "/mcp";
    fixture.config.redirect_uri = "http://127.0.0.1:9999/callback";
    fixture.config.credential_store = fixture.credentials;
    fixture.config.policy = loopback_policy(server.origin());

    std::optional<mcp::auth::OAuthClientInformation> first_identity;
    asio::co_spawn(
        io_ctx,
        [&]() -> mcp::Task<void> {
            mcp::auth::OAuthAuthorizationManager manager(io_ctx.get_executor(), fixture.tokens,
                                                         fixture.config, echoing_callback(nullptr));
            try {
                (void)co_await manager.try_handle_challenge(R"(Bearer resource_metadata=")" + base +
                                                            R"(/prm-one")");
                first_identity = manager.last_client_identity();
                // The protected resource now names a different authorization server.
                (void)co_await manager.try_handle_challenge(R"(Bearer resource_metadata=")" + base +
                                                            R"(/prm-two")");
                fixture.identity = manager.last_client_identity();
            } catch (...) {
                fixture.failure = std::current_exception();
            }
            server.close();
        },
        asio::detached);
    io_ctx.run();

    ASSERT_EQ(fixture.failure, nullptr);
    ASSERT_TRUE(first_identity.has_value());
    EXPECT_EQ(first_identity->client_id, "client-for-as-1");
    EXPECT_EQ(first_identity->issuer, base + "/as1");

    // The authorization server changed, so a fresh registration is performed rather than the first
    // server's credentials being reused.
    ASSERT_TRUE(fixture.identity.has_value());
    EXPECT_EQ(fixture.identity->client_id, "client-for-as-2");
    EXPECT_EQ(fixture.identity->issuer, base + "/as2");
    EXPECT_EQ(server.count("/as1/register"), 1U);
    EXPECT_EQ(server.count("/as2/register"), 1U);

    // Nothing the second server received mentions the first server's client.
    for (std::size_t index = 0; index < server.targets().size(); ++index) {
        if (server.targets()[index].rfind("/as2/", 0) == 0) {
            EXPECT_EQ(server.bodies()[index].find("client-for-as-1"), std::string::npos)
                << "AS-2 was sent AS-1's client identifier on " << server.targets()[index];
        }
    }

    const auto first = fixture.credentials->load(base + "/as1");
    const auto second = fixture.credentials->load(base + "/as2");
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(first->client_id, "client-for-as-1");
    EXPECT_EQ(second->client_id, "client-for-as-2");
}

TEST(AuthTokenEndpointAuthLoopbackTest, PutsTheSecretInHttpBasicWhenTheServerAsksForIt) {
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
            auto metadata = server_metadata(base, "", false, false);
            metadata["token_endpoint_auth_methods_supported"] = json::array({"client_secret_basic"});
            return json_response(metadata);
        }
        if (target == "/token") {
            return json_response(token_document());
        }
        return not_found();
    });

    IdentityFixture fixture;
    fixture.config.server_url = base + "/mcp";
    fixture.config.redirect_uri = "http://127.0.0.1:9999/callback";
    fixture.config.client_id = "confidential-client";
    fixture.config.client_secret = "confidential-secret";
    fixture.config.policy = loopback_policy(server.origin());

    run_single_challenge(server, fixture, io_ctx, R"(Bearer resource_metadata=")" + base + R"(/prm")",
                         3);

    ASSERT_EQ(fixture.failure, nullptr);
    // "confidential-client:confidential-secret" base64-encoded.
    EXPECT_EQ(server.authorization_for("/token"),
              "Basic Y29uZmlkZW50aWFsLWNsaWVudDpjb25maWRlbnRpYWwtc2VjcmV0");
    EXPECT_EQ(server.body_for("/token").find("client_secret"), std::string::npos);
}

TEST(AuthTokenEndpointAuthLoopbackTest, SendsNoSecretAtAllWhenTheServerAdvertisesOnlyNone) {
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
            auto metadata = server_metadata(base, "", false, true);
            metadata["token_endpoint_auth_methods_supported"] = json::array({"none"});
            return json_response(metadata);
        }
        if (target == "/register") {
            return json_response(
                {{"client_id", "registered-client"}, {"client_secret", "registered-secret"}},
                http::status::created);
        }
        if (target == "/token") {
            return json_response(token_document());
        }
        return not_found();
    });

    IdentityFixture fixture;
    fixture.config.server_url = base + "/mcp";
    fixture.config.redirect_uri = "http://127.0.0.1:9999/callback";
    fixture.config.credential_store = fixture.credentials;
    fixture.config.policy = loopback_policy(server.origin());

    run_single_challenge(server, fixture, io_ctx, R"(Bearer resource_metadata=")" + base + R"(/prm")",
                         4);

    ASSERT_EQ(fixture.failure, nullptr);
    ASSERT_TRUE(fixture.identity.has_value());
    // Registration handed the client a secret the server then refuses to accept anywhere.
    EXPECT_EQ(fixture.identity->client_secret, "registered-secret");
    EXPECT_TRUE(server.authorization_for("/token").empty());
    EXPECT_EQ(server.body_for("/token").find("client_secret"), std::string::npos);
    EXPECT_EQ(server.body_for("/token").find("registered-secret"), std::string::npos);
}

TEST(AuthClientIdentityLoopbackTest, ReusesTheRegistrationRecordedForTheSameIssuer) {
    asio::io_context io_ctx;
    LoopbackServer server(io_ctx);
    const auto base = server.base_url();

    server.set_handler([&base](const http::request<http::string_body>& request) {
        const std::string target(request.target());
        if (target == "/prm-one" || target == "/prm-two") {
            return json_response(
                {{"resource", base + "/mcp"}, {"authorization_servers", json::array({base})}});
        }
        if (target == "/.well-known/oauth-authorization-server") {
            return json_response(server_metadata(base, "", false, true));
        }
        if (target == "/register") {
            return json_response({{"client_id", "registered-client"}}, http::status::created);
        }
        if (target == "/token") {
            return json_response(token_document());
        }
        return not_found();
    });
    asio::co_spawn(io_ctx, server.serve(7), asio::detached);

    IdentityFixture fixture;
    fixture.config.server_url = base + "/mcp";
    fixture.config.redirect_uri = "http://127.0.0.1:9999/callback";
    fixture.config.credential_store = fixture.credentials;
    fixture.config.policy = loopback_policy(server.origin());

    asio::co_spawn(
        io_ctx,
        [&]() -> mcp::Task<void> {
            mcp::auth::OAuthAuthorizationManager manager(io_ctx.get_executor(), fixture.tokens,
                                                         fixture.config, echoing_callback(nullptr));
            try {
                (void)co_await manager.try_handle_challenge(R"(Bearer resource_metadata=")" + base +
                                                            R"(/prm-one")");
                (void)co_await manager.try_handle_challenge(R"(Bearer resource_metadata=")" + base +
                                                            R"(/prm-two")");
                fixture.identity = manager.last_client_identity();
            } catch (...) {
                fixture.failure = std::current_exception();
            }
            server.close();
        },
        asio::detached);
    io_ctx.run();

    ASSERT_EQ(fixture.failure, nullptr);
    ASSERT_TRUE(fixture.identity.has_value());
    EXPECT_EQ(fixture.identity->source, mcp::auth::ClientIdentitySource::dynamic_registration);
    EXPECT_EQ(fixture.identity->client_id, "registered-client");
    EXPECT_EQ(server.count("/register"), 1U);
}

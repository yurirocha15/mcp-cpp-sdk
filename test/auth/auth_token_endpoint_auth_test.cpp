/**
 * @file auth_token_endpoint_auth_test.cpp
 * @brief Wire-level and negative-log evidence for token endpoint client authentication (Slice D):
 *        the client secret goes exactly where the negotiated `token_endpoint_auth_method` says it
 *        belongs -- HTTP Basic header for `client_secret_basic`, the form body for
 *        `client_secret_post`, nowhere at all for `none` -- and it never surfaces in a diagnostic
 *        message when the token request fails.
 *
 * Slice B (client identity selection) and the decision function `select_token_endpoint_auth_method`
 * already have coverage elsewhere; this file exercises `OAuthHttpClient::exchange_code` directly
 * against a loopback token endpoint, one exchange per test, so each assertion is about exactly where
 * bytes travelled on the wire.
 */

#include <gtest/gtest.h>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <exception>
#include <functional>
#include <mcp/auth/oauth.hpp>
#include <nlohmann/json.hpp>
#include <string>

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using json = nlohmann::json;

namespace {

/// What a single served request looked like, as far as the client authentication contract cares.
struct RecordedRequest {
    std::string target;
    std::string body;
    std::string authorization;
};

/// The outcome of one `exchange_code()` call against a loopback server that serves exactly one
/// request.
struct ExchangeOutcome {
    mcp::auth::TokenResponse token;
    std::exception_ptr failure;
    RecordedRequest recorded;
};

http::response<http::string_body> json_response(const json& body,
                                                http::status status = http::status::ok) {
    http::response<http::string_body> response{status, 11};
    response.set(http::field::content_type, "application/json");
    response.body() = body.dump();
    return response;
}

json token_document() {
    return {{"access_token", "granted-access-token"}, {"token_type", "Bearer"}, {"expires_in", 3600}};
}

/// Accept one connection, read the request, record it, and reply with `reply`.
asio::awaitable<RecordedRequest> capture_one_request(asio::ip::tcp::acceptor& acceptor,
                                                     http::response<http::string_body> reply) {
    auto socket = co_await acceptor.async_accept(asio::use_awaitable);
    beast::tcp_stream stream(std::move(socket));

    beast::flat_buffer buffer;
    http::request<http::string_body> request;
    co_await http::async_read(stream, buffer, request, asio::use_awaitable);

    RecordedRequest recorded;
    recorded.target = std::string(request.target());
    recorded.body = request.body();
    recorded.authorization = std::string(request[http::field::authorization]);

    reply.version(request.version());
    reply.prepare_payload();
    co_await http::async_write(stream, reply, asio::use_awaitable);

    beast::error_code shutdown_error;
    (void)stream.socket().shutdown(asio::ip::tcp::socket::shutdown_both, shutdown_error);
    co_return recorded;
}

/// Run one `exchange_code()` call against a fresh loopback acceptor that replies with `reply`.
/// `make_config` receives the ephemeral port so it can point `token_endpoint` at it.
ExchangeOutcome run_exchange(const std::function<mcp::auth::OAuthConfig(unsigned short)>& make_config,
                             http::response<http::string_body> reply) {
    ExchangeOutcome outcome;
    asio::io_context io_ctx;
    asio::ip::tcp::acceptor acceptor(io_ctx, {asio::ip::make_address("127.0.0.1"), 0});
    const auto port = acceptor.local_endpoint().port();

    asio::co_spawn(
        io_ctx,
        [&]() -> asio::awaitable<void> {
            outcome.recorded = co_await capture_one_request(acceptor, std::move(reply));
        },
        asio::detached);

    asio::co_spawn(
        io_ctx,
        [&]() -> asio::awaitable<void> {
            mcp::auth::OAuthHttpClient client(io_ctx.get_executor());
            const auto config = make_config(port);
            try {
                outcome.token = co_await client.exchange_code(config, "test-authorization-code",
                                                              "test-code-verifier");
            } catch (...) {
                outcome.failure = std::current_exception();
            }
        },
        asio::detached);

    io_ctx.run();
    return outcome;
}

std::string token_endpoint_url(unsigned short port) {
    return "http://127.0.0.1:" + std::to_string(port) + "/token";
}

/// What `apply_client_authentication()` in src/auth/oauth.cpp builds for `client_secret_basic`,
/// reproduced here so the expected header is derived rather than hard-coded.
std::string expected_basic_header(const std::string& client_id, const std::string& client_secret) {
    const auto credentials =
        mcp::auth::detail::url_encode(client_id) + ":" + mcp::auth::detail::url_encode(client_secret);
    return "Basic " +
           mcp::auth::detail::base64_encode(reinterpret_cast<const unsigned char*>(credentials.data()),
                                            credentials.size());
}

/// Unwrap an `std::exception_ptr` captured from the exchange into its `what()` text.
std::string message_of(const std::exception_ptr& failure) {
    if (!failure) {
        return {};
    }
    try {
        std::rethrow_exception(failure);
    } catch (const std::exception& error) {
        return error.what();
    }
    return {};
}

}  // namespace

TEST(AuthTokenEndpointAuthWireTest, ClientSecretBasicPutsTheSecretOnlyInTheAuthorizationHeader) {
    const std::string client_id = "conf-client";
    const std::string client_secret = "sekrit-basic-value";

    const auto outcome = run_exchange(
        [&](unsigned short port) {
            mcp::auth::OAuthConfig config;
            config.client_id = client_id;
            config.client_secret = client_secret;
            config.token_endpoint = token_endpoint_url(port);
            config.redirect_uri = "http://127.0.0.1:9999/callback";
            config.token_endpoint_auth_method = "client_secret_basic";
            return config;
        },
        json_response(token_document()));

    ASSERT_EQ(outcome.failure, nullptr);
    EXPECT_EQ(outcome.token.access_token, "granted-access-token");

    // The secret travels in the Authorization header, correctly base64-encoded as `id:secret`.
    EXPECT_EQ(outcome.recorded.authorization, expected_basic_header(client_id, client_secret));

    // It appears nowhere else: not in the form body, not as a `client_secret` parameter, not in the
    // request target (path/query).
    EXPECT_EQ(outcome.recorded.body.find(client_secret), std::string::npos);
    EXPECT_EQ(outcome.recorded.body.find("client_secret"), std::string::npos);
    EXPECT_EQ(outcome.recorded.target.find(client_secret), std::string::npos);
}

TEST(AuthTokenEndpointAuthWireTest, ClientSecretPostPutsTheSecretOnlyInTheFormBody) {
    const std::string client_id = "conf-client";
    const std::string client_secret = "sekrit-post-value";

    const auto outcome = run_exchange(
        [&](unsigned short port) {
            mcp::auth::OAuthConfig config;
            config.client_id = client_id;
            config.client_secret = client_secret;
            config.token_endpoint = token_endpoint_url(port);
            config.redirect_uri = "http://127.0.0.1:9999/callback";
            config.token_endpoint_auth_method = "client_secret_post";
            return config;
        },
        json_response(token_document()));

    ASSERT_EQ(outcome.failure, nullptr);
    EXPECT_EQ(outcome.token.access_token, "granted-access-token");

    // No Authorization header at all: the secret did not also leak into HTTP Basic.
    EXPECT_TRUE(outcome.recorded.authorization.empty());

    // The secret is present exactly once, as the `client_secret` form parameter.
    EXPECT_NE(
        outcome.recorded.body.find("client_secret=" + mcp::auth::detail::url_encode(client_secret)),
        std::string::npos);
    EXPECT_EQ(outcome.recorded.target.find(client_secret), std::string::npos);
}

TEST(AuthTokenEndpointAuthWireTest, NoneMethodSendsNoSecretEvenWhenOneIsKnown) {
    const std::string client_id = "conf-client";
    // A secret the application happens to know (e.g. one a dynamic registration returned earlier),
    // but the negotiated method is `none`, so it must never be sent anywhere.
    const std::string client_secret = "dcr-issued-secret-that-must-not-travel";

    const auto outcome = run_exchange(
        [&](unsigned short port) {
            mcp::auth::OAuthConfig config;
            config.client_id = client_id;
            config.client_secret = client_secret;
            config.token_endpoint = token_endpoint_url(port);
            config.redirect_uri = "http://127.0.0.1:9999/callback";
            config.token_endpoint_auth_method = "none";
            return config;
        },
        json_response(token_document()));

    ASSERT_EQ(outcome.failure, nullptr);
    EXPECT_EQ(outcome.token.access_token, "granted-access-token");

    EXPECT_TRUE(outcome.recorded.authorization.empty());
    EXPECT_EQ(outcome.recorded.body.find("client_secret"), std::string::npos);
    EXPECT_EQ(outcome.recorded.body.find(client_secret), std::string::npos);
    EXPECT_EQ(outcome.recorded.target.find(client_secret), std::string::npos);
}

TEST(AuthTokenEndpointAuthNegativeLogTest,
     FailingTokenRequestWithClientSecretPostNeverEchoesTheSecretInTheExceptionMessage) {
    const std::string client_id = "conf-client";
    const std::string client_secret = "sekrit-post-value-that-must-stay-out-of-logs";

    // A realistic authorization-server error body: it names the problem, it does not echo the
    // request back.
    const auto outcome = run_exchange(
        [&](unsigned short port) {
            mcp::auth::OAuthConfig config;
            config.client_id = client_id;
            config.client_secret = client_secret;
            config.token_endpoint = token_endpoint_url(port);
            config.redirect_uri = "http://127.0.0.1:9999/callback";
            config.token_endpoint_auth_method = "client_secret_post";
            return config;
        },
        json_response(
            json{{"error", "invalid_client"}, {"error_description", "client authentication failed"}},
            http::status::unauthorized));

    ASSERT_NE(outcome.failure, nullptr);
    // The secret did travel in this request's body (client_secret_post); the assertion that matters
    // is that the *diagnostic surfaced to the caller* -- the thrown exception's message -- never
    // repeats it.
    ASSERT_NE(outcome.recorded.body.find(client_secret), std::string::npos)
        << "test setup error: the secret should have been in the request body for this scenario";

    const auto message = message_of(outcome.failure);
    EXPECT_FALSE(message.empty());
    EXPECT_EQ(message.find(client_secret), std::string::npos)
        << "exception message leaked the client secret: " << message;
}

TEST(AuthTokenEndpointAuthNegativeLogTest,
     FailingTokenRequestWithClientSecretBasicNeverEchoesTheSecretInTheExceptionMessage) {
    const std::string client_id = "conf-client";
    const std::string client_secret = "sekrit-basic-value-that-must-stay-out-of-logs";

    const auto outcome = run_exchange(
        [&](unsigned short port) {
            mcp::auth::OAuthConfig config;
            config.client_id = client_id;
            config.client_secret = client_secret;
            config.token_endpoint = token_endpoint_url(port);
            config.redirect_uri = "http://127.0.0.1:9999/callback";
            config.token_endpoint_auth_method = "client_secret_basic";
            return config;
        },
        json_response(json{{"error", "invalid_client"}}, http::status::unauthorized));

    ASSERT_NE(outcome.failure, nullptr);
    ASSERT_EQ(outcome.recorded.authorization, expected_basic_header(client_id, client_secret))
        << "test setup error: the secret should have travelled in the Authorization header";

    const auto message = message_of(outcome.failure);
    EXPECT_FALSE(message.empty());
    EXPECT_EQ(message.find(client_secret), std::string::npos)
        << "exception message leaked the client secret: " << message;
    // The raw Basic-auth header value must not appear either, since decoding it recovers the secret.
    EXPECT_EQ(message.find(expected_basic_header(client_id, client_secret)), std::string::npos);
}

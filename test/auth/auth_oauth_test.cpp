/**
 * @file auth_oauth_test.cpp
 * @brief Tests for OAuth 2.1 core (Task 17) and discovery (Task 18)
 */

#include <gtest/gtest.h>

#include <atomic>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <mcp/auth/oauth.hpp>
#include <mcp/core/constants.hpp>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using json = nlohmann::json;

// Known SHA-256 test vector: SHA-256("") =
// e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855
std::string hex_encode(const unsigned char* data, std::size_t len) {
    std::string result;
    result.reserve(len * 2);
    for (std::size_t i = 0; i < len; ++i) {
        result.push_back(mcp::constants::g_hex_digits[data[i] >> 4]);
        result.push_back(mcp::constants::g_hex_digits[data[i] & 0x0F]);
    }
    return result;
}

using AcceptorCallback =
    std::function<asio::awaitable<void>(asio::ip::tcp::socket, asio::ip::tcp::acceptor&)>;

asio::awaitable<void> run_mock_server(asio::ip::tcp::acceptor& acceptor, AcceptorCallback handler) {
    auto socket = co_await acceptor.async_accept(asio::use_awaitable);
    co_await handler(std::move(socket), acceptor);
}

/// A policy admitting exactly the plain-http loopback fixture these tests drive directly. A
/// policy-less `OAuthHttpClient` now defaults to deny-all, so every test that talks to a mock
/// server without going through `OAuthAuthorizationManager` (which always installs its own policy)
/// must opt in explicitly.
mcp::auth::MetadataFetchPolicy loopback_policy(unsigned short port) {
    mcp::auth::MetadataFetchPolicy policy;
    policy.allowed_origins.push_back("http://127.0.0.1:" + std::to_string(port));
    policy.allow_plain_http_loopback = true;
    return policy;
}

}  // namespace

TEST(AuthSha256Test, EmptyString) {
    auto hash = mcp::auth::detail::sha256("");
    auto hex = hex_encode(hash.data(), hash.size());
    EXPECT_EQ(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST(AuthSha256Test, Abc) {
    auto hash = mcp::auth::detail::sha256("abc");
    auto hex = hex_encode(hash.data(), hash.size());
    EXPECT_EQ(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(AuthSha256Test, LongerString) {
    auto hash = mcp::auth::detail::sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq");
    auto hex = hex_encode(hash.data(), hash.size());
    EXPECT_EQ(hex, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST(AuthBase64Test, BasicEncode) {
    std::string input = "Hello";
    auto encoded = mcp::auth::detail::base64_encode(
        reinterpret_cast<const unsigned char*>(input.data()), input.size());
    EXPECT_EQ(encoded, "SGVsbG8=");
}

TEST(AuthBase64Test, EmptyInput) {
    auto encoded = mcp::auth::detail::base64_encode(nullptr, 0);
    EXPECT_EQ(encoded, "");
}

TEST(AuthBase64UrlTest, NoPaddingAndCharReplacement) {
    unsigned char data[] = {0xfb, 0xff, 0xfe};
    auto encoded = mcp::auth::detail::base64url_encode(data, 3);
    EXPECT_EQ(encoded.find('+'), std::string::npos);
    EXPECT_EQ(encoded.find('/'), std::string::npos);
    EXPECT_EQ(encoded.find('='), std::string::npos);
}

TEST(AuthUrlEncodeTest, ReservedChars) {
    auto encoded = mcp::auth::detail::url_encode("hello world&foo=bar");
    EXPECT_EQ(encoded, "hello%20world%26foo%3Dbar");
}

TEST(AuthUrlEncodeTest, UnreservedCharsPassThrough) {
    auto encoded = mcp::auth::detail::url_encode("abc-._~123");
    EXPECT_EQ(encoded, "abc-._~123");
}

TEST(AuthFormBodyTest, BuildsCorrectly) {
    auto body = mcp::auth::detail::build_form_body({{"grant_type", "authorization_code"},
                                                    {"code", "abc123"},
                                                    {"redirect_uri", "http://localhost"}});
    EXPECT_EQ(body, "grant_type=authorization_code&code=abc123&redirect_uri=http%3A%2F%2Flocalhost");
}

TEST(AuthPkceTest, GeneratesPairWithCorrectProperties) {
    auto pair = mcp::auth::generate_pkce_pair();
    EXPECT_GE(pair.code_verifier.size(), 43);
    EXPECT_LE(pair.code_verifier.size(), 128);
    EXPECT_EQ(pair.challenge_method, "S256");
    EXPECT_FALSE(pair.code_challenge.empty());

    for (char ch : pair.code_verifier) {
        bool is_unreserved = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                             (ch >= '0' && ch <= '9') || ch == '-' || ch == '.' || ch == '_' ||
                             ch == '~';
        EXPECT_TRUE(is_unreserved) << "Invalid char in verifier: " << ch;
    }
}

TEST(AuthPkceTest, ChallengeMatchesSha256OfVerifier) {
    auto pair = mcp::auth::generate_pkce_pair(43);
    auto hash = mcp::auth::detail::sha256(pair.code_verifier);
    auto expected_challenge = mcp::auth::detail::base64url_encode(hash.data(), hash.size());
    EXPECT_EQ(pair.code_challenge, expected_challenge);
}

TEST(AuthPkceTest, RejectsInvalidLength) {
    EXPECT_THROW(mcp::auth::generate_pkce_pair(42), std::invalid_argument);
    EXPECT_THROW(mcp::auth::generate_pkce_pair(129), std::invalid_argument);
}

TEST(AuthPkceTest, CustomLengths) {
    auto pair43 = mcp::auth::generate_pkce_pair(43);
    EXPECT_EQ(pair43.code_verifier.size(), 43);

    auto pair128 = mcp::auth::generate_pkce_pair(128);
    EXPECT_EQ(pair128.code_verifier.size(), 128);
}

TEST(AuthPkceTest, TwoPairsAreDifferent) {
    auto pair1 = mcp::auth::generate_pkce_pair();
    auto pair2 = mcp::auth::generate_pkce_pair();
    EXPECT_NE(pair1.code_verifier, pair2.code_verifier);
    EXPECT_NE(pair1.code_challenge, pair2.code_challenge);
}

TEST(AuthTokenResponseTest, JsonDeserialization) {
    json j = {{"access_token", "at_123"},
              {"token_type", "Bearer"},
              {"refresh_token", "rt_456"},
              {"expires_in", 3600},
              {"scope", "read write"}};

    auto token = j.get<mcp::auth::TokenResponse>();
    EXPECT_EQ(token.access_token, "at_123");
    EXPECT_EQ(token.token_type, "Bearer");
    EXPECT_EQ(token.refresh_token.value(), "rt_456");
    EXPECT_EQ(token.expires_in.value(), 3600);
    EXPECT_EQ(token.scope.value(), "read write");
}

TEST(AuthTokenResponseTest, MinimalDeserialization) {
    json j = {{"access_token", "at_min"}};

    auto token = j.get<mcp::auth::TokenResponse>();
    EXPECT_EQ(token.access_token, "at_min");
    EXPECT_EQ(token.token_type, "Bearer");
    EXPECT_FALSE(token.refresh_token.has_value());
    EXPECT_FALSE(token.expires_in.has_value());
    EXPECT_FALSE(token.scope.has_value());
}

TEST(AuthTokenResponseTest, JsonSerialization) {
    mcp::auth::TokenResponse token;
    token.access_token = "at_ser";
    token.token_type = "Bearer";
    token.refresh_token = "rt_ser";
    token.expires_in = 1800;

    json j = token;
    EXPECT_EQ(j["access_token"], "at_ser");
    EXPECT_EQ(j["token_type"], "Bearer");
    EXPECT_EQ(j["refresh_token"], "rt_ser");
    EXPECT_EQ(j["expires_in"], 1800);
}

TEST(AuthTokenResponseTest, IsExpired) {
    mcp::auth::TokenResponse token;
    token.access_token = "test";
    token.expires_in = 1;
    token.received_at = std::chrono::steady_clock::now() - std::chrono::seconds(100);
    EXPECT_TRUE(token.is_expired());
}

TEST(AuthTokenResponseTest, IsNotExpiredWithoutExpiresIn) {
    mcp::auth::TokenResponse token;
    token.access_token = "test";
    EXPECT_FALSE(token.is_expired());
}

TEST(AuthTokenResponseTest, IsNotExpiredWhenFresh) {
    mcp::auth::TokenResponse token;
    token.access_token = "test";
    token.expires_in = 3600;
    token.received_at = std::chrono::steady_clock::now();
    EXPECT_FALSE(token.is_expired());
}

TEST(AuthTokenStoreTest, StoreAndLoad) {
    mcp::auth::InMemoryTokenStore store;
    mcp::auth::TokenResponse token;
    token.access_token = "stored_at";
    token.token_type = "Bearer";

    store.store("http://server1", token);
    auto loaded = store.load("http://server1");
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->access_token, "stored_at");
}

TEST(AuthTokenStoreTest, LoadMissing) {
    mcp::auth::InMemoryTokenStore store;
    auto loaded = store.load("http://nonexistent");
    EXPECT_FALSE(loaded.has_value());
}

TEST(AuthTokenStoreTest, Remove) {
    mcp::auth::InMemoryTokenStore store;
    mcp::auth::TokenResponse token;
    token.access_token = "to_remove";

    store.store("http://server1", token);
    store.remove("http://server1");
    auto loaded = store.load("http://server1");
    EXPECT_FALSE(loaded.has_value());
}

TEST(AuthTokenStoreTest, Overwrite) {
    mcp::auth::InMemoryTokenStore store;
    mcp::auth::TokenResponse token1;
    token1.access_token = "first";
    mcp::auth::TokenResponse token2;
    token2.access_token = "second";

    store.store("http://server1", token1);
    store.store("http://server1", token2);
    auto loaded = store.load("http://server1");
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->access_token, "second");
}

class MockTokenServer : public ::testing::Test {
   protected:
    asio::io_context io_ctx_;
};

TEST_F(MockTokenServer, ExchangeCodeProducesValidToken) {
    constexpr unsigned short port = 18095;

    asio::ip::tcp::acceptor acceptor(io_ctx_, {asio::ip::make_address("127.0.0.1"), port});

    std::string received_body;

    asio::co_spawn(
        io_ctx_,
        [&]() -> asio::awaitable<void> {
            auto socket = co_await acceptor.async_accept(asio::use_awaitable);
            beast::tcp_stream stream(std::move(socket));

            beast::flat_buffer buffer;
            http::request<http::string_body> req;
            co_await http::async_read(stream, buffer, req, asio::use_awaitable);

            received_body = req.body();

            json response_json = {{"access_token", "mock_access_token"},
                                  {"token_type", "Bearer"},
                                  {"refresh_token", "mock_refresh_token"},
                                  {"expires_in", 3600}};

            http::response<http::string_body> res{http::status::ok, req.version()};
            res.set(http::field::content_type, "application/json");
            res.body() = response_json.dump();
            res.prepare_payload();
            co_await http::async_write(stream, res, asio::use_awaitable);

            beast::error_code ec;
            stream.socket().shutdown(asio::ip::tcp::socket::shutdown_both, ec);
        },
        asio::detached);

    mcp::auth::TokenResponse result;

    asio::co_spawn(
        io_ctx_,
        [&]() -> asio::awaitable<void> {
            mcp::auth::OAuthHttpClient client(io_ctx_.get_executor());
            client.set_metadata_policy(loopback_policy(port));
            mcp::auth::OAuthConfig config;
            config.client_id = "test_client";
            config.token_endpoint = "http://127.0.0.1:" + std::to_string(port) + "/token";
            config.redirect_uri = "http://localhost/callback";

            result = co_await client.exchange_code(config, "auth_code_123", "verifier_abc");
        },
        asio::detached);

    io_ctx_.run();

    EXPECT_EQ(result.access_token, "mock_access_token");
    EXPECT_EQ(result.token_type, "Bearer");
    ASSERT_TRUE(result.refresh_token.has_value());
    EXPECT_EQ(result.refresh_token.value(), "mock_refresh_token");
    EXPECT_EQ(result.expires_in.value(), 3600);

    EXPECT_NE(received_body.find("grant_type=authorization_code"), std::string::npos);
    EXPECT_NE(received_body.find("code=auth_code_123"), std::string::npos);
    EXPECT_NE(received_body.find("code_verifier=verifier_abc"), std::string::npos);
    EXPECT_NE(received_body.find("client_id=test_client"), std::string::npos);
}

TEST_F(MockTokenServer, ExchangeCodeWithClientSecret) {
    constexpr unsigned short port = 18096;

    asio::ip::tcp::acceptor acceptor(io_ctx_, {asio::ip::make_address("127.0.0.1"), port});

    std::string received_body;

    asio::co_spawn(
        io_ctx_,
        [&]() -> asio::awaitable<void> {
            auto socket = co_await acceptor.async_accept(asio::use_awaitable);
            beast::tcp_stream stream(std::move(socket));

            beast::flat_buffer buffer;
            http::request<http::string_body> req;
            co_await http::async_read(stream, buffer, req, asio::use_awaitable);

            received_body = req.body();

            json response_json = {{"access_token", "secret_at"}, {"token_type", "Bearer"}};

            http::response<http::string_body> res{http::status::ok, req.version()};
            res.set(http::field::content_type, "application/json");
            res.body() = response_json.dump();
            res.prepare_payload();
            co_await http::async_write(stream, res, asio::use_awaitable);

            beast::error_code ec;
            stream.socket().shutdown(asio::ip::tcp::socket::shutdown_both, ec);
        },
        asio::detached);

    mcp::auth::TokenResponse result;

    asio::co_spawn(
        io_ctx_,
        [&]() -> asio::awaitable<void> {
            mcp::auth::OAuthHttpClient client(io_ctx_.get_executor());
            client.set_metadata_policy(loopback_policy(port));
            mcp::auth::OAuthConfig config;
            config.client_id = "test_client";
            config.client_secret = "super_secret";
            config.token_endpoint = "http://127.0.0.1:" + std::to_string(port) + "/token";
            config.redirect_uri = "http://localhost/callback";

            result = co_await client.exchange_code(config, "code_xyz", "verifier_xyz");
        },
        asio::detached);

    io_ctx_.run();

    EXPECT_EQ(result.access_token, "secret_at");
    EXPECT_NE(received_body.find("client_secret=super_secret"), std::string::npos);
}

TEST_F(MockTokenServer, RefreshTokenProducesNewToken) {
    constexpr unsigned short port = 18097;

    asio::ip::tcp::acceptor acceptor(io_ctx_, {asio::ip::make_address("127.0.0.1"), port});

    std::string received_body;

    asio::co_spawn(
        io_ctx_,
        [&]() -> asio::awaitable<void> {
            auto socket = co_await acceptor.async_accept(asio::use_awaitable);
            beast::tcp_stream stream(std::move(socket));

            beast::flat_buffer buffer;
            http::request<http::string_body> req;
            co_await http::async_read(stream, buffer, req, asio::use_awaitable);

            received_body = req.body();

            json response_json = {{"access_token", "refreshed_at"},
                                  {"token_type", "Bearer"},
                                  {"refresh_token", "new_rt"},
                                  {"expires_in", 7200}};

            http::response<http::string_body> res{http::status::ok, req.version()};
            res.set(http::field::content_type, "application/json");
            res.body() = response_json.dump();
            res.prepare_payload();
            co_await http::async_write(stream, res, asio::use_awaitable);

            beast::error_code ec;
            stream.socket().shutdown(asio::ip::tcp::socket::shutdown_both, ec);
        },
        asio::detached);

    mcp::auth::TokenResponse result;

    asio::co_spawn(
        io_ctx_,
        [&]() -> asio::awaitable<void> {
            mcp::auth::OAuthHttpClient client(io_ctx_.get_executor());
            client.set_metadata_policy(loopback_policy(port));
            mcp::auth::OAuthConfig config;
            config.client_id = "test_client";
            config.token_endpoint = "http://127.0.0.1:" + std::to_string(port) + "/token";
            config.redirect_uri = "http://localhost/callback";

            result = co_await client.refresh_token(config, "old_refresh_token");
        },
        asio::detached);

    io_ctx_.run();

    EXPECT_EQ(result.access_token, "refreshed_at");
    ASSERT_TRUE(result.refresh_token.has_value());
    EXPECT_EQ(result.refresh_token.value(), "new_rt");

    EXPECT_NE(received_body.find("grant_type=refresh_token"), std::string::npos);
    EXPECT_NE(received_body.find("refresh_token=old_refresh_token"), std::string::npos);
    EXPECT_NE(received_body.find("client_id=test_client"), std::string::npos);
}

TEST_F(MockTokenServer, TokenExchangeErrorThrows) {
    constexpr unsigned short port = 18098;

    asio::ip::tcp::acceptor acceptor(io_ctx_, {asio::ip::make_address("127.0.0.1"), port});

    asio::co_spawn(
        io_ctx_,
        [&]() -> asio::awaitable<void> {
            auto socket = co_await acceptor.async_accept(asio::use_awaitable);
            beast::tcp_stream stream(std::move(socket));

            beast::flat_buffer buffer;
            http::request<http::string_body> req;
            co_await http::async_read(stream, buffer, req, asio::use_awaitable);

            http::response<http::string_body> res{http::status::bad_request, req.version()};
            res.set(http::field::content_type, "application/json");
            res.body() = R"({"error":"invalid_grant"})";
            res.prepare_payload();
            co_await http::async_write(stream, res, asio::use_awaitable);

            beast::error_code ec;
            stream.socket().shutdown(asio::ip::tcp::socket::shutdown_both, ec);
        },
        asio::detached);

    bool threw = false;

    asio::co_spawn(
        io_ctx_,
        [&]() -> asio::awaitable<void> {
            mcp::auth::OAuthHttpClient client(io_ctx_.get_executor());
            client.set_metadata_policy(loopback_policy(port));
            mcp::auth::OAuthConfig config;
            config.client_id = "test_client";
            config.token_endpoint = "http://127.0.0.1:" + std::to_string(port) + "/token";
            config.redirect_uri = "http://localhost/callback";

            try {
                co_await client.exchange_code(config, "bad_code", "verifier");
            } catch (const std::runtime_error&) {
                threw = true;
            }
        },
        asio::detached);

    io_ctx_.run();

    EXPECT_TRUE(threw);
}

TEST_F(MockTokenServer, DoesNotReplayThePostAfterAnAmbiguousMidResponseFailure) {
    // The connection dies after the request is fully read but before any response is written: from
    // the client's point of view the server may or may not have acted on it, so this POST must never
    // be silently replayed within the same exchange_code() call. Redirects are already never
    // followed for a POST (src/auth/oauth.cpp: run_post_json) for the same reason; this proves there
    // is no other path that re-sends it.
    constexpr unsigned short port = 18111;

    asio::ip::tcp::acceptor acceptor(io_ctx_, {asio::ip::make_address("127.0.0.1"), port});
    int requests_seen = 0;

    asio::co_spawn(
        io_ctx_,
        [&]() -> asio::awaitable<void> {
            auto socket = co_await acceptor.async_accept(asio::use_awaitable);
            beast::tcp_stream stream(std::move(socket));
            beast::flat_buffer buffer;
            http::request<http::string_body> req;
            co_await http::async_read(stream, buffer, req, asio::use_awaitable);
            ++requests_seen;
            beast::error_code ec;
            stream.socket().close(ec);

            // Give a bounded window for a (forbidden) replay to arrive, then cancel the accept so
            // the test never hangs waiting for a connection that -- correctly -- never comes.
            asio::steady_timer cutoff(io_ctx_);
            cutoff.expires_after(std::chrono::milliseconds(300));
            cutoff.async_wait([&](boost::system::error_code) { acceptor.cancel(); });

            boost::system::error_code accept_error;
            (void)co_await acceptor.async_accept(
                asio::redirect_error(asio::use_awaitable, accept_error));
            if (!accept_error) {
                ++requests_seen;
            }
            cutoff.cancel();
        },
        asio::detached);

    bool threw = false;

    asio::co_spawn(
        io_ctx_,
        [&]() -> asio::awaitable<void> {
            mcp::auth::OAuthHttpClient client(io_ctx_.get_executor());
            client.set_metadata_policy(loopback_policy(port));
            mcp::auth::OAuthConfig config;
            config.client_id = "test_client";
            config.token_endpoint = "http://127.0.0.1:" + std::to_string(port) + "/token";
            config.redirect_uri = "http://localhost/callback";

            try {
                co_await client.exchange_code(config, "a-code", "verifier");
            } catch (const std::exception&) {
                threw = true;
            }
        },
        asio::detached);

    io_ctx_.run();

    EXPECT_TRUE(threw);
    EXPECT_EQ(requests_seen, 1);
}

TEST_F(MockTokenServer, GetJsonReturnsValidJson) {
    constexpr unsigned short port = 18099;

    asio::ip::tcp::acceptor acceptor(io_ctx_, {asio::ip::make_address("127.0.0.1"), port});

    asio::co_spawn(
        io_ctx_,
        [&]() -> asio::awaitable<void> {
            auto socket = co_await acceptor.async_accept(asio::use_awaitable);
            beast::tcp_stream stream(std::move(socket));

            beast::flat_buffer buffer;
            http::request<http::string_body> req;
            co_await http::async_read(stream, buffer, req, asio::use_awaitable);

            json response_json = {{"issuer", "https://auth.example.com"},
                                  {"token_endpoint", "https://auth.example.com/token"}};

            http::response<http::string_body> res{http::status::ok, req.version()};
            res.set(http::field::content_type, "application/json");
            res.body() = response_json.dump();
            res.prepare_payload();
            co_await http::async_write(stream, res, asio::use_awaitable);

            beast::error_code ec;
            stream.socket().shutdown(asio::ip::tcp::socket::shutdown_both, ec);
        },
        asio::detached);

    json result;

    asio::co_spawn(
        io_ctx_,
        [&]() -> asio::awaitable<void> {
            mcp::auth::OAuthHttpClient client(io_ctx_.get_executor());
            client.set_metadata_policy(loopback_policy(port));
            result =
                co_await client.get_json("http://127.0.0.1:" + std::to_string(port) + "/well-known");
        },
        asio::detached);

    io_ctx_.run();

    EXPECT_EQ(result["issuer"], "https://auth.example.com");
    EXPECT_EQ(result["token_endpoint"], "https://auth.example.com/token");
}

TEST_F(MockTokenServer, GetJsonErrorThrows) {
    constexpr unsigned short port = 18100;

    asio::ip::tcp::acceptor acceptor(io_ctx_, {asio::ip::make_address("127.0.0.1"), port});

    asio::co_spawn(
        io_ctx_,
        [&]() -> asio::awaitable<void> {
            auto socket = co_await acceptor.async_accept(asio::use_awaitable);
            beast::tcp_stream stream(std::move(socket));

            beast::flat_buffer buffer;
            http::request<http::string_body> req;
            co_await http::async_read(stream, buffer, req, asio::use_awaitable);

            http::response<http::string_body> res{http::status::not_found, req.version()};
            res.body() = "Not Found";
            res.prepare_payload();
            co_await http::async_write(stream, res, asio::use_awaitable);

            beast::error_code ec;
            stream.socket().shutdown(asio::ip::tcp::socket::shutdown_both, ec);
        },
        asio::detached);

    bool threw = false;

    asio::co_spawn(
        io_ctx_,
        [&]() -> asio::awaitable<void> {
            mcp::auth::OAuthHttpClient client(io_ctx_.get_executor());
            client.set_metadata_policy(loopback_policy(port));
            try {
                co_await client.get_json("http://127.0.0.1:" + std::to_string(port) + "/nope");
            } catch (const std::runtime_error&) {
                threw = true;
            }
        },
        asio::detached);

    io_ctx_.run();

    EXPECT_TRUE(threw);
}

TEST(AuthDiscoveryMetadataTest, ProtectedResourceParsesCorrectly) {
    json j = {{"resource", "https://mcp.example.com"},
              {"authorization_servers", {"https://auth.example.com"}},
              {"scopes_supported", {"mcp:read", "mcp:write"}}};

    auto metadata = j.get<mcp::auth::ProtectedResourceMetadata>();
    EXPECT_EQ(metadata.resource, "https://mcp.example.com");
    ASSERT_EQ(metadata.authorization_servers.size(), 1);
    EXPECT_EQ(metadata.authorization_servers[0], "https://auth.example.com");
    ASSERT_TRUE(metadata.scopes_supported.has_value());
    EXPECT_EQ(metadata.scopes_supported->size(), 2);
    EXPECT_EQ(metadata.raw, j);
}

TEST(AuthDiscoveryMetadataTest, ProtectedResourceMinimal) {
    json j = json::object();
    auto metadata = j.get<mcp::auth::ProtectedResourceMetadata>();
    EXPECT_TRUE(metadata.resource.empty());
    EXPECT_TRUE(metadata.authorization_servers.empty());
    EXPECT_FALSE(metadata.scopes_supported.has_value());
}

TEST(AuthDiscoveryMetadataTest, AuthServerParsesCorrectly) {
    json j = {{"issuer", "https://auth.example.com"},
              {"authorization_endpoint", "https://auth.example.com/authorize"},
              {"token_endpoint", "https://auth.example.com/token"},
              {"revocation_endpoint", "https://auth.example.com/revoke"},
              {"registration_endpoint", "https://auth.example.com/register"},
              {"scopes_supported", {"openid", "profile"}},
              {"response_types_supported", {"code"}},
              {"grant_types_supported", {"authorization_code", "refresh_token"}},
              {"code_challenge_methods_supported", {"S256"}}};

    auto metadata = j.get<mcp::auth::AuthServerMetadata>();
    EXPECT_EQ(metadata.issuer, "https://auth.example.com");
    EXPECT_EQ(metadata.authorization_endpoint, "https://auth.example.com/authorize");
    EXPECT_EQ(metadata.token_endpoint, "https://auth.example.com/token");
    ASSERT_TRUE(metadata.revocation_endpoint.has_value());
    EXPECT_EQ(metadata.revocation_endpoint.value(), "https://auth.example.com/revoke");
    ASSERT_TRUE(metadata.registration_endpoint.has_value());
    ASSERT_TRUE(metadata.code_challenge_methods_supported.has_value());
    EXPECT_EQ(metadata.code_challenge_methods_supported->at(0), "S256");
    EXPECT_EQ(metadata.raw, j);
}

TEST(AuthDiscoveryMetadataTest, AuthServerMinimalFields) {
    json j = {{"issuer", "https://auth.example.com"},
              {"authorization_endpoint", "https://auth.example.com/authorize"},
              {"token_endpoint", "https://auth.example.com/token"}};

    auto metadata = j.get<mcp::auth::AuthServerMetadata>();
    EXPECT_EQ(metadata.issuer, "https://auth.example.com");
    EXPECT_FALSE(metadata.revocation_endpoint.has_value());
    EXPECT_FALSE(metadata.registration_endpoint.has_value());
    EXPECT_FALSE(metadata.scopes_supported.has_value());
}

TEST(AuthDiscoveryMetadataTest, AuthServerW5ExtensionFieldsParsedWhenPresent) {
    json j = {{"issuer", "https://auth.example.com"},
              {"authorization_endpoint", "https://auth.example.com/authorize"},
              {"token_endpoint", "https://auth.example.com/token"},
              {"authorization_response_iss_parameter_supported", true},
              {"client_id_metadata_document_supported", true},
              {"token_endpoint_auth_methods_supported", {"client_secret_basic", "none"}}};

    auto metadata = j.get<mcp::auth::AuthServerMetadata>();
    ASSERT_TRUE(metadata.authorization_response_iss_parameter_supported.has_value());
    EXPECT_TRUE(metadata.authorization_response_iss_parameter_supported.value());
    ASSERT_TRUE(metadata.client_id_metadata_document_supported.has_value());
    EXPECT_TRUE(metadata.client_id_metadata_document_supported.value());
    ASSERT_TRUE(metadata.token_endpoint_auth_methods_supported.has_value());
    ASSERT_EQ(metadata.token_endpoint_auth_methods_supported->size(), 2);
    EXPECT_EQ(metadata.token_endpoint_auth_methods_supported->at(0), "client_secret_basic");
    EXPECT_EQ(metadata.token_endpoint_auth_methods_supported->at(1), "none");
}

TEST(AuthDiscoveryMetadataTest, AuthServerW5ExtensionFieldsAbsentByDefault) {
    json j = {{"issuer", "https://auth.example.com"},
              {"authorization_endpoint", "https://auth.example.com/authorize"},
              {"token_endpoint", "https://auth.example.com/token"}};

    auto metadata = j.get<mcp::auth::AuthServerMetadata>();
    EXPECT_FALSE(metadata.authorization_response_iss_parameter_supported.has_value());
    EXPECT_FALSE(metadata.client_id_metadata_document_supported.has_value());
    EXPECT_FALSE(metadata.token_endpoint_auth_methods_supported.has_value());
}

TEST(AuthCacheTest, CachedEntryNotExpired) {
    mcp::auth::CachedEntry<int> entry{42, std::chrono::steady_clock::now() + std::chrono::hours(1)};
    EXPECT_FALSE(entry.is_expired());
}

TEST(AuthCacheTest, CachedEntryExpired) {
    mcp::auth::CachedEntry<int> entry{42, std::chrono::steady_clock::now() - std::chrono::seconds(1)};
    EXPECT_TRUE(entry.is_expired());
}

class DiscoveryTest : public ::testing::Test {
   protected:
    asio::io_context io_ctx_;
};

TEST_F(DiscoveryTest, DiscoverProtectedResource) {
    constexpr unsigned short port = 18101;

    asio::ip::tcp::acceptor acceptor(io_ctx_, {asio::ip::make_address("127.0.0.1"), port});

    asio::co_spawn(
        io_ctx_,
        [&]() -> asio::awaitable<void> {
            auto socket = co_await acceptor.async_accept(asio::use_awaitable);
            beast::tcp_stream stream(std::move(socket));

            beast::flat_buffer buffer;
            http::request<http::string_body> req;
            co_await http::async_read(stream, buffer, req, asio::use_awaitable);

            EXPECT_EQ(std::string(req.target()), "/.well-known/oauth-protected-resource");

            json response_json = {{"resource", "http://127.0.0.1:" + std::to_string(port)},
                                  {"authorization_servers", {"http://auth.local"}}};

            http::response<http::string_body> res{http::status::ok, req.version()};
            res.set(http::field::content_type, "application/json");
            res.body() = response_json.dump();
            res.prepare_payload();
            co_await http::async_write(stream, res, asio::use_awaitable);

            beast::error_code ec;
            stream.socket().shutdown(asio::ip::tcp::socket::shutdown_both, ec);
        },
        asio::detached);

    mcp::auth::ProtectedResourceMetadata result;

    asio::co_spawn(
        io_ctx_,
        [&]() -> asio::awaitable<void> {
            auto http_client = std::make_shared<mcp::auth::OAuthHttpClient>(io_ctx_.get_executor());
            http_client->set_metadata_policy(loopback_policy(port));
            mcp::auth::OAuthDiscoveryClient discovery(http_client, std::chrono::seconds(60));

            result = co_await discovery.discover_protected_resource("http://127.0.0.1:" +
                                                                    std::to_string(port));
        },
        asio::detached);

    io_ctx_.run();

    EXPECT_EQ(result.resource, "http://127.0.0.1:" + std::to_string(port));
    ASSERT_EQ(result.authorization_servers.size(), 1);
    EXPECT_EQ(result.authorization_servers[0], "http://auth.local");
}

TEST_F(DiscoveryTest, DiscoverProtectedResourceWithPath) {
    constexpr unsigned short port = 18102;

    asio::ip::tcp::acceptor acceptor(io_ctx_, {asio::ip::make_address("127.0.0.1"), port});

    std::string requested_path;

    asio::co_spawn(
        io_ctx_,
        [&]() -> asio::awaitable<void> {
            auto socket = co_await acceptor.async_accept(asio::use_awaitable);
            beast::tcp_stream stream(std::move(socket));

            beast::flat_buffer buffer;
            http::request<http::string_body> req;
            co_await http::async_read(stream, buffer, req, asio::use_awaitable);

            requested_path = std::string(req.target());

            json response_json = {{"resource", "http://127.0.0.1:" + std::to_string(port) + "/v1/mcp"},
                                  {"authorization_servers", {"http://auth.local"}}};

            http::response<http::string_body> res{http::status::ok, req.version()};
            res.set(http::field::content_type, "application/json");
            res.body() = response_json.dump();
            res.prepare_payload();
            co_await http::async_write(stream, res, asio::use_awaitable);

            beast::error_code ec;
            stream.socket().shutdown(asio::ip::tcp::socket::shutdown_both, ec);
        },
        asio::detached);

    mcp::auth::ProtectedResourceMetadata result;

    asio::co_spawn(
        io_ctx_,
        [&]() -> asio::awaitable<void> {
            auto http_client = std::make_shared<mcp::auth::OAuthHttpClient>(io_ctx_.get_executor());
            http_client->set_metadata_policy(loopback_policy(port));
            mcp::auth::OAuthDiscoveryClient discovery(http_client, std::chrono::seconds(60));

            result = co_await discovery.discover_protected_resource(
                "http://127.0.0.1:" + std::to_string(port) + "/v1/mcp");
        },
        asio::detached);

    io_ctx_.run();

    EXPECT_EQ(requested_path, "/.well-known/oauth-protected-resource/v1/mcp");
    ASSERT_EQ(result.authorization_servers.size(), 1);
}

TEST_F(DiscoveryTest, DiscoverAuthServer) {
    constexpr unsigned short port = 18103;

    asio::ip::tcp::acceptor acceptor(io_ctx_, {asio::ip::make_address("127.0.0.1"), port});

    asio::co_spawn(
        io_ctx_,
        [&]() -> asio::awaitable<void> {
            auto socket = co_await acceptor.async_accept(asio::use_awaitable);
            beast::tcp_stream stream(std::move(socket));

            beast::flat_buffer buffer;
            http::request<http::string_body> req;
            co_await http::async_read(stream, buffer, req, asio::use_awaitable);

            EXPECT_EQ(std::string(req.target()), "/.well-known/oauth-authorization-server");

            json response_json = {
                {"issuer", "http://127.0.0.1:" + std::to_string(port)},
                {"authorization_endpoint", "http://127.0.0.1:" + std::to_string(port) + "/authorize"},
                {"token_endpoint", "http://127.0.0.1:" + std::to_string(port) + "/token"}};

            http::response<http::string_body> res{http::status::ok, req.version()};
            res.set(http::field::content_type, "application/json");
            res.body() = response_json.dump();
            res.prepare_payload();
            co_await http::async_write(stream, res, asio::use_awaitable);

            beast::error_code ec;
            stream.socket().shutdown(asio::ip::tcp::socket::shutdown_both, ec);
        },
        asio::detached);

    mcp::auth::AuthServerMetadata result;

    asio::co_spawn(
        io_ctx_,
        [&]() -> asio::awaitable<void> {
            auto http_client = std::make_shared<mcp::auth::OAuthHttpClient>(io_ctx_.get_executor());
            http_client->set_metadata_policy(loopback_policy(port));
            mcp::auth::OAuthDiscoveryClient discovery(http_client, std::chrono::seconds(60));

            result =
                co_await discovery.discover_auth_server("http://127.0.0.1:" + std::to_string(port));
        },
        asio::detached);

    io_ctx_.run();

    EXPECT_EQ(result.issuer, "http://127.0.0.1:" + std::to_string(port));
    EXPECT_FALSE(result.token_endpoint.empty());
}

TEST_F(DiscoveryTest, DiscoverAuthServerFallsBackToOIDC) {
    constexpr unsigned short port = 18104;

    asio::ip::tcp::acceptor acceptor(io_ctx_, {asio::ip::make_address("127.0.0.1"), port});

    int request_count = 0;

    asio::co_spawn(
        io_ctx_,
        [&]() -> asio::awaitable<void> {
            // First request: oauth-authorization-server → 404
            {
                auto socket = co_await acceptor.async_accept(asio::use_awaitable);
                beast::tcp_stream stream(std::move(socket));
                beast::flat_buffer buffer;
                http::request<http::string_body> req;
                co_await http::async_read(stream, buffer, req, asio::use_awaitable);
                ++request_count;

                http::response<http::string_body> res{http::status::not_found, req.version()};
                res.body() = "Not Found";
                res.prepare_payload();
                co_await http::async_write(stream, res, asio::use_awaitable);

                beast::error_code ec;
                stream.socket().shutdown(asio::ip::tcp::socket::shutdown_both, ec);
            }

            // Second request: openid-configuration → 200
            {
                auto socket = co_await acceptor.async_accept(asio::use_awaitable);
                beast::tcp_stream stream(std::move(socket));
                beast::flat_buffer buffer;
                http::request<http::string_body> req;
                co_await http::async_read(stream, buffer, req, asio::use_awaitable);
                ++request_count;

                EXPECT_EQ(std::string(req.target()), "/.well-known/openid-configuration");

                json response_json = {
                    {"issuer", "http://127.0.0.1:" + std::to_string(port)},
                    {"authorization_endpoint",
                     "http://127.0.0.1:" + std::to_string(port) + "/authorize"},
                    {"token_endpoint", "http://127.0.0.1:" + std::to_string(port) + "/token"}};

                http::response<http::string_body> res{http::status::ok, req.version()};
                res.set(http::field::content_type, "application/json");
                res.body() = response_json.dump();
                res.prepare_payload();
                co_await http::async_write(stream, res, asio::use_awaitable);

                beast::error_code ec;
                stream.socket().shutdown(asio::ip::tcp::socket::shutdown_both, ec);
            }
        },
        asio::detached);

    mcp::auth::AuthServerMetadata result;

    asio::co_spawn(
        io_ctx_,
        [&]() -> asio::awaitable<void> {
            auto http_client = std::make_shared<mcp::auth::OAuthHttpClient>(io_ctx_.get_executor());
            http_client->set_metadata_policy(loopback_policy(port));
            mcp::auth::OAuthDiscoveryClient discovery(http_client, std::chrono::seconds(60));

            result =
                co_await discovery.discover_auth_server("http://127.0.0.1:" + std::to_string(port));
        },
        asio::detached);

    io_ctx_.run();

    EXPECT_EQ(request_count, 2);
    EXPECT_EQ(result.issuer, "http://127.0.0.1:" + std::to_string(port));
}

TEST_F(DiscoveryTest, CacheHitSkipsNetworkCall) {
    constexpr unsigned short port = 18105;

    asio::ip::tcp::acceptor acceptor(io_ctx_, {asio::ip::make_address("127.0.0.1"), port});

    int request_count = 0;

    asio::co_spawn(
        io_ctx_,
        [&]() -> asio::awaitable<void> {
            auto socket = co_await acceptor.async_accept(asio::use_awaitable);
            beast::tcp_stream stream(std::move(socket));
            beast::flat_buffer buffer;
            http::request<http::string_body> req;
            co_await http::async_read(stream, buffer, req, asio::use_awaitable);
            ++request_count;

            json response_json = {
                {"issuer", "http://127.0.0.1:" + std::to_string(port)},
                {"authorization_endpoint", "http://127.0.0.1:" + std::to_string(port) + "/authorize"},
                {"token_endpoint", "http://127.0.0.1:" + std::to_string(port) + "/token"}};

            http::response<http::string_body> res{http::status::ok, req.version()};
            res.set(http::field::content_type, "application/json");
            res.body() = response_json.dump();
            res.prepare_payload();
            co_await http::async_write(stream, res, asio::use_awaitable);

            beast::error_code ec;
            stream.socket().shutdown(asio::ip::tcp::socket::shutdown_both, ec);
        },
        asio::detached);

    mcp::auth::AuthServerMetadata result1;
    mcp::auth::AuthServerMetadata result2;

    asio::co_spawn(
        io_ctx_,
        [&]() -> asio::awaitable<void> {
            auto http_client = std::make_shared<mcp::auth::OAuthHttpClient>(io_ctx_.get_executor());
            http_client->set_metadata_policy(loopback_policy(port));
            mcp::auth::OAuthDiscoveryClient discovery(http_client, std::chrono::seconds(300));

            result1 =
                co_await discovery.discover_auth_server("http://127.0.0.1:" + std::to_string(port));
            result2 =
                co_await discovery.discover_auth_server("http://127.0.0.1:" + std::to_string(port));
        },
        asio::detached);

    io_ctx_.run();

    EXPECT_EQ(request_count, 1);
    EXPECT_EQ(result1.issuer, result2.issuer);
}

TEST_F(DiscoveryTest, ClearCacheInvalidatesEntries) {
    constexpr unsigned short port = 18106;

    asio::ip::tcp::acceptor acceptor(io_ctx_, {asio::ip::make_address("127.0.0.1"), port});

    int request_count = 0;

    asio::co_spawn(
        io_ctx_,
        [&]() -> asio::awaitable<void> {
            for (int i = 0; i < 2; ++i) {
                auto socket = co_await acceptor.async_accept(asio::use_awaitable);
                beast::tcp_stream stream(std::move(socket));
                beast::flat_buffer buffer;
                http::request<http::string_body> req;
                co_await http::async_read(stream, buffer, req, asio::use_awaitable);
                ++request_count;

                json response_json = {
                    {"issuer", "http://127.0.0.1:" + std::to_string(port)},
                    {"authorization_endpoint",
                     "http://127.0.0.1:" + std::to_string(port) + "/authorize"},
                    {"token_endpoint", "http://127.0.0.1:" + std::to_string(port) + "/token"}};

                http::response<http::string_body> res{http::status::ok, req.version()};
                res.set(http::field::content_type, "application/json");
                res.body() = response_json.dump();
                res.prepare_payload();
                co_await http::async_write(stream, res, asio::use_awaitable);

                beast::error_code ec;
                stream.socket().shutdown(asio::ip::tcp::socket::shutdown_both, ec);
            }
        },
        asio::detached);

    asio::co_spawn(
        io_ctx_,
        [&]() -> asio::awaitable<void> {
            auto http_client = std::make_shared<mcp::auth::OAuthHttpClient>(io_ctx_.get_executor());
            http_client->set_metadata_policy(loopback_policy(port));
            mcp::auth::OAuthDiscoveryClient discovery(http_client, std::chrono::seconds(300));

            co_await discovery.discover_auth_server("http://127.0.0.1:" + std::to_string(port));
            discovery.clear_cache();
            co_await discovery.discover_auth_server("http://127.0.0.1:" + std::to_string(port));
        },
        asio::detached);

    io_ctx_.run();

    EXPECT_EQ(request_count, 2);
}

// A policy-less client now defaults to deny-all rather than allow-all: no origin is on the (empty)
// allow list, and plain http is refused outright. Refusal happens in `enforce_url_policy` before
// any lookup, so the host resolver this test installs must never run.
TEST(AuthOAuthHttpClientDefaultPolicyTest, PolicyLessClientRefusesAnHttpLoopbackUrlWithoutResolving) {
    asio::io_context io_ctx;
    mcp::auth::OAuthHttpClient client(io_ctx.get_executor());

    int resolver_calls = 0;
    client.set_host_resolver(
        [&resolver_calls](const std::string&, const std::string&) -> std::vector<std::string> {
            ++resolver_calls;
            return {"127.0.0.1"};
        });

    bool threw = false;
    auto decision = mcp::auth::MetadataUrlDecision::allowed;

    asio::co_spawn(
        io_ctx,
        [&]() -> asio::awaitable<void> {
            try {
                (void)co_await client.get_json("http://127.0.0.1:18199/probe");
            } catch (const mcp::auth::MetadataPolicyError& error) {
                threw = true;
                decision = error.decision();
            }
        },
        asio::detached);

    io_ctx.run();

    EXPECT_TRUE(threw);
    EXPECT_TRUE(decision == mcp::auth::MetadataUrlDecision::origin_not_allowed ||
                decision == mcp::auth::MetadataUrlDecision::scheme_not_allowed)
        << "unexpected decision: " << mcp::auth::describe(decision);
    EXPECT_EQ(resolver_calls, 0);
}

// -------------------------------------------------------------------------------------------
// F3: the metadata policy and the host resolver must be installable as ONE change.
//
// The fixture is two HTTP servers sharing a port on two loopback addresses, each naming itself
// in its body. Every request targets `http://localhost:<port>/doc`, so the installed resolver
// alone decides which server answers, and the body IS the classification: a body of "new" can
// only be produced by the NEW resolver under the OLD, wider allow list, which is F3 on the wire.
// -------------------------------------------------------------------------------------------
namespace {

/// One loopback HTTP server that answers every request with its own name.
class NamedLoopbackServer {
   public:
    NamedLoopbackServer(asio::io_context& ctx, const std::string& address, unsigned short port,
                        std::string name)
        : acceptor_(ctx, {asio::ip::make_address(address), port}), name_(std::move(name)) {
        asio::co_spawn(ctx, accept_loop(), asio::detached);
    }

    void close() {
        boost::system::error_code ec;
        acceptor_.close(ec);
    }

   private:
    asio::awaitable<void> accept_loop() {
        for (;;) {
            boost::system::error_code ec;
            auto socket =
                co_await acceptor_.async_accept(asio::redirect_error(asio::use_awaitable, ec));
            if (ec) {
                co_return;
            }
            asio::co_spawn(socket.get_executor(), serve(std::move(socket)), asio::detached);
        }
    }

    asio::awaitable<void> serve(asio::ip::tcp::socket socket) {
        beast::tcp_stream stream(std::move(socket));
        beast::flat_buffer buffer;
        http::request<http::string_body> request;
        boost::system::error_code ec;
        co_await http::async_read(stream, buffer, request,
                                  asio::redirect_error(asio::use_awaitable, ec));
        if (ec) {
            co_return;
        }
        http::response<http::string_body> response{http::status::ok, request.version()};
        response.set(http::field::content_type, "application/json");
        response.body() = json{{"server", name_}}.dump();
        response.prepare_payload();
        co_await http::async_write(stream, response, asio::redirect_error(asio::use_awaitable, ec));
        stream.socket().shutdown(asio::ip::tcp::socket::shutdown_both, ec);
    }

    asio::ip::tcp::acceptor acceptor_;
    std::string name_;
};

/// Whether this environment has promised a second loopback address.
///
/// Every OAuthSetterPairAtomicity test needs a port free on both 127.0.0.1 and 127.0.0.2, and
/// skips when there is none. On a host or container without 127.0.0.2 that silently deletes the
/// entire regression evidence for the setter-pair atomicity fix while the suite still reports
/// green -- the failure mode where a guard disappears and nothing says so. Where the second
/// address is known to exist (Linux, where 127.0.0.0/8 is bound whole), set
/// MCP_REQUIRE_TWIN_LOOPBACK=1 and a missing port fails the run instead of skipping it. CI sets it
/// on Linux; the default stays a skip so the suite remains runnable anywhere.
[[nodiscard]] bool twin_loopback_is_required() {
    const char* const flag = std::getenv("MCP_REQUIRE_TWIN_LOOPBACK");
    return flag != nullptr && std::string_view(flag) == "1";
}

/// Skip the calling test when no twin loopback port is available -- or fail it, when the
/// environment declared that one must be. Declares `port_name` as the port to use.
#define MCP_TWIN_LOOPBACK_PORT_OR_SKIP(port_name)                                                      \
    const unsigned short port_name = find_twin_loopback_port();                                        \
    if ((port_name) == 0) {                                                                            \
        if (twin_loopback_is_required()) {                                                             \
            FAIL() << "MCP_REQUIRE_TWIN_LOOPBACK=1, but no port in [18140, 18200) is free on "         \
                      "both 127.0.0.1 and 127.0.0.2, so this test would have skipped and the "         \
                      "setter-pair atomicity evidence would have vanished silently";                   \
        }                                                                                              \
        GTEST_SKIP() << "no port free on both 127.0.0.1 and 127.0.0.2";                                \
    }                                                                                                  \
    static_cast<void>(0)

/// A port free on BOTH loopback addresses, or 0 when the second address is unavailable.
unsigned short find_twin_loopback_port() {
    asio::io_context probe_ctx;
    for (unsigned short port = 18140; port < 18200; ++port) {
        try {
            asio::ip::tcp::acceptor first(probe_ctx, {asio::ip::make_address("127.0.0.1"), port});
            asio::ip::tcp::acceptor second(probe_ctx, {asio::ip::make_address("127.0.0.2"), port});
            return port;
        } catch (const boost::system::system_error&) {
            continue;
        }
    }
    return 0;
}

mcp::auth::MetadataFetchPolicy origin_policy(const std::string& origin) {
    mcp::auth::MetadataFetchPolicy policy;
    policy.allowed_origins.push_back(origin);
    policy.allow_plain_http_loopback = true;
    return policy;
}

/// What one completed exchange was observed to do.
enum class Observed {
    old_server,
    new_server,
    refused_origin,
    other
};

Observed classify_exchange(const std::exception_ptr& failure, const json& body) {
    if (failure == nullptr) {
        const auto name = body.value("server", std::string{});
        if (name == "old") {
            return Observed::old_server;
        }
        if (name == "new") {
            return Observed::new_server;
        }
        return Observed::other;
    }
    try {
        std::rethrow_exception(failure);
    } catch (const mcp::auth::MetadataPolicyError& error) {
        return error.decision() == mcp::auth::MetadataUrlDecision::origin_not_allowed
                   ? Observed::refused_origin
                   : Observed::other;
    } catch (const std::exception&) {
        return Observed::other;
    }
}

void busy_wait_micros(int micros) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::microseconds(micros);
    while (std::chrono::steady_clock::now() < deadline) {
    }
}

/// The two servers plus the two configurations the application moves between.
struct TwinFixture {
    explicit TwinFixture(unsigned short port)
        : old_server(ctx, "127.0.0.1", port, "old"),
          new_server(ctx, "127.0.0.2", port, "new"),
          url("http://localhost:" + std::to_string(port) + "/doc"),
          wide(origin_policy("http://localhost:" + std::to_string(port))),
          narrow(origin_policy("http://127.0.0.9:1")) {
        thread = std::thread([this]() { ctx.run(); });
    }

    ~TwinFixture() {
        asio::post(ctx, [this]() {
            old_server.close();
            new_server.close();
        });
        work.reset();
        if (thread.joinable()) {
            thread.join();
        }
    }

    asio::io_context ctx;
    asio::executor_work_guard<asio::io_context::executor_type> work{asio::make_work_guard(ctx)};
    NamedLoopbackServer old_server;
    NamedLoopbackServer new_server;
    std::thread thread;
    std::string url;
    mcp::auth::MetadataFetchPolicy wide;
    mcp::auth::MetadataFetchPolicy narrow;
    mcp::auth::HostResolver resolver_old = [](const std::string&, const std::string&) {
        return std::vector<std::string>{"127.0.0.1"};
    };
    mcp::auth::HostResolver resolver_new = [](const std::string&, const std::string&) {
        return std::vector<std::string>{"127.0.0.2"};
    };
};

}  // namespace

// The hazard itself, with the race taken out of it: the exchange is started at a point the test
// chooses, inside the gap between the two setter calls. It sees the new resolver under the old,
// wider allow list every time, because that is simply what the client's state is at that instant.
TEST(OAuthSetterPairAtomicity, TheTwoSingleSettersLeaveAWindowAnExchangeCanFallInto) {
    MCP_TWIN_LOOPBACK_PORT_OR_SKIP(port);
    TwinFixture fixture(port);

    asio::io_context client_ctx;
    auto client = std::make_shared<mcp::auth::OAuthHttpClient>(client_ctx.get_executor());
    client->set_host_resolver(fixture.resolver_old);
    client->set_metadata_policy(fixture.wide);

    std::promise<void> resolver_installed;
    std::promise<void> exchange_finished;
    auto installed = resolver_installed.get_future();
    auto finished = exchange_finished.get_future();

    std::thread reconfigurer([&]() {
        client->set_host_resolver(fixture.resolver_new);
        resolver_installed.set_value();
        finished.wait();
        client->set_metadata_policy(fixture.narrow);
    });

    installed.wait();

    std::exception_ptr failure;
    json body;
    asio::co_spawn(
        client_ctx,
        [&]() -> asio::awaitable<void> {
            try {
                body = co_await client->get_json(fixture.url);
            } catch (...) {
                failure = std::current_exception();
            }
        },
        asio::detached);
    client_ctx.run();
    exchange_finished.set_value();
    reconfigurer.join();

    EXPECT_EQ(classify_exchange(failure, body), Observed::new_server) << "body was " << body.dump();
}

// The regression. An application that moves between two whole configurations with a millisecond
// of its own work between the two calls admits, on nearly every narrowing, an exchange directed
// by the new resolver and validated against the old allow list. Applied as one unit there is no
// instant at which that state exists, so the count is zero rather than small.
TEST(OAuthSetterPairAtomicity, ReconfiguringAsOnePairNeverExposesTheNewResolverUnderTheOldPolicy) {
    MCP_TWIN_LOOPBACK_PORT_OR_SKIP(port);
    TwinFixture fixture(port);

    asio::io_context client_ctx;
    auto work = asio::make_work_guard(client_ctx);
    auto client = std::make_shared<mcp::auth::OAuthHttpClient>(client_ctx.get_executor());

    auto reconfigure = [&](const mcp::auth::MetadataFetchPolicy& policy,
                           const mcp::auth::HostResolver& resolver) {
        client->configure(policy, resolver);
    };

    reconfigure(fixture.wide, fixture.resolver_old);

    std::atomic<std::uint64_t> old_server{0};
    std::atomic<std::uint64_t> new_server{0};
    std::atomic<std::uint64_t> refused{0};
    std::atomic<std::uint64_t> other{0};
    std::atomic<std::uint64_t> in_flight{0};
    std::atomic<std::uint64_t> transitions{0};
    std::atomic<bool> stop{false};

    std::vector<std::thread> io_threads;
    io_threads.reserve(2);
    for (int index = 0; index < 2; ++index) {
        io_threads.emplace_back([&client_ctx]() { client_ctx.run(); });
    }

    std::thread writer([&]() {
        while (!stop.load(std::memory_order_relaxed)) {
            reconfigure(fixture.narrow, fixture.resolver_new);
            transitions.fetch_add(1, std::memory_order_relaxed);
            busy_wait_micros(1000);
            reconfigure(fixture.wide, fixture.resolver_old);
            busy_wait_micros(1000);
        }
    });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (std::chrono::steady_clock::now() < deadline) {
        if (transitions.load() >= 25 && old_server.load() >= 25 && refused.load() >= 25) {
            break;
        }
        if (in_flight.load(std::memory_order_relaxed) >= 2) {
            std::this_thread::yield();
            continue;
        }
        in_flight.fetch_add(1, std::memory_order_relaxed);
        asio::co_spawn(
            client_ctx,
            [&]() -> asio::awaitable<void> {
                std::exception_ptr failure;
                json body;
                try {
                    body = co_await client->get_json(fixture.url);
                } catch (...) {
                    failure = std::current_exception();
                }
                switch (classify_exchange(failure, body)) {
                    case Observed::old_server:
                        old_server.fetch_add(1, std::memory_order_relaxed);
                        break;
                    case Observed::new_server:
                        new_server.fetch_add(1, std::memory_order_relaxed);
                        break;
                    case Observed::refused_origin:
                        refused.fetch_add(1, std::memory_order_relaxed);
                        break;
                    case Observed::other:
                        other.fetch_add(1, std::memory_order_relaxed);
                        break;
                }
                in_flight.fetch_sub(1, std::memory_order_relaxed);
            },
            asio::detached);
    }

    stop.store(true);
    writer.join();
    const auto drain_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (in_flight.load() > 0 && std::chrono::steady_clock::now() < drain_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_EQ(in_flight.load(), 0u) << "an exchange never completed; the run proves nothing";
    work.reset();
    client_ctx.stop();
    for (auto& thread : io_threads) {
        thread.join();
    }

    // Non-vacuity: the traffic must have straddled BOTH whole configurations, or a zero F3 count
    // would only mean the requests all landed in one steady state.
    EXPECT_GT(old_server.load(), 0u) << "no exchange ever ran under the wide configuration";
    EXPECT_GT(refused.load(), 0u) << "no exchange ever ran under the narrow configuration";
    EXPECT_GE(transitions.load(), 25u) << "the run did not reach enough narrowings to mean much";
    EXPECT_EQ(new_server.load(), 0u)
        << "F3: " << new_server.load() << " of "
        << (old_server.load() + new_server.load() + refused.load() + other.load())
        << " exchanges were directed by the NEW resolver while validated against the OLD, wider "
           "allow list, across "
        << transitions.load() << " narrowings";
}

// Guards the regression above from passing for the wrong reason: a configure() that quietly
// dropped its resolver argument would also never produce a "new" body. Each call here installs a
// configuration and the exchange that follows must show BOTH halves of it.
TEST(OAuthSetterPairAtomicity, ConfigureInstallsBothOfItsArguments) {
    MCP_TWIN_LOOPBACK_PORT_OR_SKIP(port);
    TwinFixture fixture(port);

    asio::io_context client_ctx;
    mcp::auth::OAuthHttpClient client(client_ctx.get_executor());

    auto observe = [&]() {
        std::exception_ptr failure;
        json body;
        client_ctx.restart();
        asio::co_spawn(
            client_ctx,
            [&]() -> asio::awaitable<void> {
                try {
                    body = co_await client.get_json(fixture.url);
                } catch (...) {
                    failure = std::current_exception();
                }
            },
            asio::detached);
        client_ctx.run();
        return classify_exchange(failure, body);
    };

    client.configure(fixture.wide, fixture.resolver_old);
    EXPECT_EQ(observe(), Observed::old_server);

    // Only the resolver changes: the policy half must still be the wide one, or this would be a
    // refusal rather than a body from the second server.
    client.configure(fixture.wide, fixture.resolver_new);
    EXPECT_EQ(observe(), Observed::new_server);

    // Only the policy changes: the narrowing must take effect on the very next exchange.
    client.configure(fixture.narrow, fixture.resolver_new);
    EXPECT_EQ(observe(), Observed::refused_origin);
}

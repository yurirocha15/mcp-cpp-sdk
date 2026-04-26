/**
 * @file auth_oauth_test.cpp
 * @brief Tests for OAuth 2.1 core (Task 17) and discovery (Task 18)
 */

#include <gtest/gtest.h>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <mcp/auth/oauth.hpp>
#include <mcp/constants.hpp>
#include <nlohmann/json.hpp>
#include <string>
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
            mcp::auth::OAuthDiscoveryClient discovery(http_client, std::chrono::seconds(300));

            co_await discovery.discover_auth_server("http://127.0.0.1:" + std::to_string(port));
            discovery.clear_cache();
            co_await discovery.discover_auth_server("http://127.0.0.1:" + std::to_string(port));
        },
        asio::detached);

    io_ctx_.run();

    EXPECT_EQ(request_count, 2);
}

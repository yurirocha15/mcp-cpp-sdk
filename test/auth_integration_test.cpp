/**
 * @file auth_integration_test.cpp
 * @brief Tests for OAuth 2.1 integration (Task 19): middleware, bearer extraction, transport wrapper
 */

#include <gtest/gtest.h>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <mcp/auth/oauth.hpp>
#include <mcp/protocol.hpp>
#include <mcp/server.hpp>
#include <nlohmann/json.hpp>
#include <queue>
#include <string>
#include <vector>

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using json = nlohmann::json;

class ScriptedTransport : public mcp::ITransport {
   public:
    explicit ScriptedTransport(const asio::any_io_executor& executor)
        : strand_(asio::make_strand(executor)), timer_(strand_) {
        timer_.expires_at(std::chrono::steady_clock::time_point::max());
    }

    mcp::Task<std::string> read_message() override {
        for (;;) {
            if (closed_) {
                throw std::runtime_error("transport closed");
            }
            if (!incoming_.empty()) {
                auto msg = std::move(incoming_.front());
                incoming_.pop();
                co_return msg;
            }
            timer_.expires_at(std::chrono::steady_clock::time_point::max());
            try {
                co_await timer_.async_wait(asio::use_awaitable);
            } catch (const boost::system::system_error& err) {
                if (err.code() != boost::asio::error::operation_aborted) {
                    throw;
                }
            }
        }
    }

    mcp::Task<void> write_message(std::string_view message) override {
        co_await asio::post(strand_, asio::use_awaitable);
        written_.emplace_back(message);
        if (on_write_) {
            on_write_(written_.back());
        }
        co_return;
    }

    void close() override {
        closed_ = true;
        timer_.cancel();
    }

    void enqueue_message(std::string msg) {
        asio::post(strand_, [this, m = std::move(msg)]() mutable {
            incoming_.push(std::move(m));
            timer_.cancel();
        });
    }

    void set_on_write(std::function<void(std::string_view)> callback) {
        on_write_ = std::move(callback);
    }

    [[nodiscard]] const std::vector<std::string>& written() const { return written_; }

   private:
    asio::strand<asio::any_io_executor> strand_;
    asio::steady_timer timer_;
    std::queue<std::string> incoming_;
    std::vector<std::string> written_;
    std::function<void(std::string_view)> on_write_;
    bool closed_ = false;
};

json make_initialize_request(std::string_view req_id) {
    return {{"jsonrpc", "2.0"},
            {"id", req_id},
            {"method", "initialize"},
            {"params",
             {{"protocolVersion", mcp::LATEST_PROTOCOL_VERSION},
              {"clientInfo", {{"name", "test-client"}, {"version", "0.1"}}},
              {"capabilities", json::object()}}}};
}

}  // namespace

TEST(AuthBearerExtractionTest, ValidBearerToken) {
    auto token = mcp::auth::extract_bearer_token("Bearer my_access_token");
    EXPECT_EQ(token, "my_access_token");
}

TEST(AuthBearerExtractionTest, EmptyHeader) {
    auto token = mcp::auth::extract_bearer_token("");
    EXPECT_TRUE(token.empty());
}

TEST(AuthBearerExtractionTest, WrongScheme) {
    auto token = mcp::auth::extract_bearer_token("Basic dXNlcjpwYXNz");
    EXPECT_TRUE(token.empty());
}

TEST(AuthBearerExtractionTest, BearerWithoutSpace) {
    auto token = mcp::auth::extract_bearer_token("Bearertoken123");
    EXPECT_TRUE(token.empty());
}

TEST(AuthBearerExtractionTest, BearerOnly) {
    auto token = mcp::auth::extract_bearer_token("Bearer ");
    EXPECT_TRUE(token.empty());
}

TEST(AuthBearerExtractionTest, TokenWithSpecialChars) {
    auto token =
        mcp::auth::extract_bearer_token("Bearer eyJhbGciOiJSUzI1NiJ9.eyJzdWIiOiIxMjM0NTY3ODkw");
    EXPECT_EQ(token, "eyJhbGciOiJSUzI1NiJ9.eyJzdWIiOiIxMjM0NTY3ODkw");
}

TEST(AuthMiddlewareTest, AcceptsValidToken) {
    asio::io_context io;

    mcp::ServerCapabilities caps;
    mcp::ServerCapabilities::ToolsCapability tools_cap;
    caps.tools = std::move(tools_cap);

    mcp::Implementation info;
    info.name = "test-server";
    info.version = "1.0";

    mcp::Server server(std::move(info), std::move(caps));

    server.use(mcp::auth::make_auth_middleware(
        [](const std::string& token) -> mcp::Task<bool> { co_return token == "valid_token"; }));

    server.add_tool<json, json>("echo", "Echoes input", json{{"type", "object"}},
                                [](const json& args) -> json { return args; });

    auto transport = std::make_unique<ScriptedTransport>(io.get_executor());
    auto* transport_ptr = transport.get();

    std::vector<json> responses;
    transport_ptr->set_on_write([&responses, transport_ptr](std::string_view msg) {
        responses.push_back(json::parse(msg));
        if (responses.size() >= 2) {
            transport_ptr->close();
        }
    });

    transport_ptr->enqueue_message(make_initialize_request("1").dump());

    json call_req{{"jsonrpc", "2.0"},
                  {"id", "2"},
                  {"method", "tools/call"},
                  {"params",
                   {{"name", "echo"},
                    {"arguments", {{"hello", "world"}}},
                    {"_meta", {{"auth_token", "valid_token"}}}}}};
    transport_ptr->enqueue_message(call_req.dump());

    asio::co_spawn(io, server.run(std::move(transport), io.get_executor()), asio::detached);
    io.run();

    ASSERT_GE(responses.size(), 2);
    auto it = std::find_if(responses.begin(), responses.end(),
                           [](const json& msg) { return msg.contains("id") && msg["id"] == "2"; });
    ASSERT_NE(it, responses.end());
    EXPECT_FALSE((*it)["result"].contains("isError"));
}

TEST(AuthMiddlewareTest, RejectsInvalidToken) {
    asio::io_context io;

    mcp::ServerCapabilities caps;
    mcp::ServerCapabilities::ToolsCapability tools_cap;
    caps.tools = std::move(tools_cap);

    mcp::Implementation info;
    info.name = "test-server";
    info.version = "1.0";

    mcp::Server server(std::move(info), std::move(caps));

    server.use(mcp::auth::make_auth_middleware(
        [](const std::string& token) -> mcp::Task<bool> { co_return token == "valid_token"; }));

    server.add_tool<json, json>("echo", "Echoes input", json{{"type", "object"}},
                                [](const json& args) -> json { return args; });

    auto transport = std::make_unique<ScriptedTransport>(io.get_executor());
    auto* transport_ptr = transport.get();

    std::vector<json> responses;
    transport_ptr->set_on_write([&responses, transport_ptr](std::string_view msg) {
        responses.push_back(json::parse(msg));
        if (responses.size() >= 2) {
            transport_ptr->close();
        }
    });

    transport_ptr->enqueue_message(make_initialize_request("1").dump());

    json call_req{{"jsonrpc", "2.0"},
                  {"id", "2"},
                  {"method", "tools/call"},
                  {"params",
                   {{"name", "echo"},
                    {"arguments", {{"hello", "world"}}},
                    {"_meta", {{"auth_token", "wrong_token"}}}}}};
    transport_ptr->enqueue_message(call_req.dump());

    asio::co_spawn(io, server.run(std::move(transport), io.get_executor()), asio::detached);
    io.run();

    ASSERT_GE(responses.size(), 2);
    auto it = std::find_if(responses.begin(), responses.end(),
                           [](const json& msg) { return msg.contains("id") && msg["id"] == "2"; });
    ASSERT_NE(it, responses.end());
    EXPECT_TRUE((*it)["result"]["isError"].get<bool>());
    auto text = (*it)["result"]["content"][0]["text"].get<std::string>();
    EXPECT_NE(text.find("invalid"), std::string::npos);
}

TEST(AuthMiddlewareTest, RejectsMissingToken) {
    asio::io_context io;

    mcp::ServerCapabilities caps;
    mcp::ServerCapabilities::ToolsCapability tools_cap;
    caps.tools = std::move(tools_cap);

    mcp::Implementation info;
    info.name = "test-server";
    info.version = "1.0";

    mcp::Server server(std::move(info), std::move(caps));

    server.use(
        mcp::auth::make_auth_middleware([](const std::string&) -> mcp::Task<bool> { co_return true; }));

    server.add_tool<json, json>("echo", "Echoes input", json{{"type", "object"}},
                                [](const json& args) -> json { return args; });

    auto transport = std::make_unique<ScriptedTransport>(io.get_executor());
    auto* transport_ptr = transport.get();

    std::vector<json> responses;
    transport_ptr->set_on_write([&responses, transport_ptr](std::string_view msg) {
        responses.push_back(json::parse(msg));
        if (responses.size() >= 2) {
            transport_ptr->close();
        }
    });

    transport_ptr->enqueue_message(make_initialize_request("1").dump());

    json call_req{{"jsonrpc", "2.0"},
                  {"id", "2"},
                  {"method", "tools/call"},
                  {"params", {{"name", "echo"}, {"arguments", {{"hello", "world"}}}}}};
    transport_ptr->enqueue_message(call_req.dump());

    asio::co_spawn(io, server.run(std::move(transport), io.get_executor()), asio::detached);
    io.run();

    ASSERT_GE(responses.size(), 2);
    auto it = std::find_if(responses.begin(), responses.end(),
                           [](const json& msg) { return msg.contains("id") && msg["id"] == "2"; });
    ASSERT_NE(it, responses.end());
    EXPECT_TRUE((*it)["result"]["isError"].get<bool>());
    auto text = (*it)["result"]["content"][0]["text"].get<std::string>();
    EXPECT_NE(text.find("missing"), std::string::npos);
}

TEST(AuthClientTransportTest, StoreAndRetrieveToken) {
    asio::io_context io;

    auto inner = std::make_unique<ScriptedTransport>(io.get_executor());
    auto store = std::make_shared<mcp::auth::InMemoryTokenStore>();
    auto oauth_client = std::make_shared<mcp::auth::OAuthHttpClient>(io.get_executor());

    mcp::auth::OAuthConfig config;
    config.client_id = "test";
    config.token_endpoint = "http://localhost/token";
    config.redirect_uri = "http://localhost/callback";

    auto authenticator =
        std::make_shared<mcp::auth::OAuthAuthenticator>(store, oauth_client, config, "http://server1");
    mcp::auth::OAuthClientTransport transport(std::move(inner), authenticator);

    EXPECT_TRUE(authenticator->get_access_token().empty());

    mcp::auth::TokenResponse token;
    token.access_token = "my_token";
    token.token_type = "Bearer";
    authenticator->store_token(std::move(token));

    EXPECT_EQ(authenticator->get_access_token(), "my_token");
}

TEST(AuthClientTransportTest, ReadWritePassThrough) {
    asio::io_context io;

    auto inner = std::make_unique<ScriptedTransport>(io.get_executor());
    auto* inner_ptr = inner.get();
    auto store = std::make_shared<mcp::auth::InMemoryTokenStore>();
    auto oauth_client = std::make_shared<mcp::auth::OAuthHttpClient>(io.get_executor());

    mcp::auth::OAuthConfig config;
    config.client_id = "test";
    config.token_endpoint = "http://localhost/token";
    config.redirect_uri = "http://localhost/callback";

    auto authenticator =
        std::make_shared<mcp::auth::OAuthAuthenticator>(store, oauth_client, config, "http://server1");
    mcp::auth::OAuthClientTransport transport(std::move(inner), authenticator);

    inner_ptr->enqueue_message("hello from server");

    std::string received;
    std::string written;

    asio::co_spawn(
        io,
        [&]() -> mcp::Task<void> {
            received = co_await transport.read_message();
            co_await transport.write_message("hello from client");
        },
        asio::detached);

    io.run();

    EXPECT_EQ(received, "hello from server");
    ASSERT_EQ(inner_ptr->written().size(), 1);
    EXPECT_EQ(inner_ptr->written()[0], "hello from client");

    transport.close();
}

TEST(AuthClientTransportTest, RefreshTokenReturnsTrue) {
    constexpr unsigned short port = 18107;
    asio::io_context io;

    asio::ip::tcp::acceptor acceptor(io, {asio::ip::make_address("127.0.0.1"), port});

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto socket = co_await acceptor.async_accept(asio::use_awaitable);
            beast::tcp_stream stream(std::move(socket));

            beast::flat_buffer buffer;
            http::request<http::string_body> req;
            co_await http::async_read(stream, buffer, req, asio::use_awaitable);

            json response_json = {{"access_token", "new_access_token"},
                                  {"token_type", "Bearer"},
                                  {"refresh_token", "new_refresh_token"},
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

    auto inner = std::make_unique<ScriptedTransport>(io.get_executor());
    auto store = std::make_shared<mcp::auth::InMemoryTokenStore>();
    auto oauth_client = std::make_shared<mcp::auth::OAuthHttpClient>(io.get_executor());

    mcp::auth::OAuthConfig config;
    config.client_id = "test";
    config.token_endpoint = "http://127.0.0.1:" + std::to_string(port) + "/token";
    config.redirect_uri = "http://localhost/callback";

    mcp::auth::TokenResponse old_token;
    old_token.access_token = "expired_token";
    old_token.refresh_token = "old_refresh_token";
    store->store("http://server1", old_token);

    auto authenticator =
        std::make_shared<mcp::auth::OAuthAuthenticator>(store, oauth_client, config, "http://server1");
    mcp::auth::OAuthClientTransport transport(std::move(inner), authenticator);

    bool refresh_result = false;

    asio::co_spawn(
        io, [&]() -> mcp::Task<void> { refresh_result = co_await authenticator->try_refresh_token(); },
        asio::detached);

    io.run();

    EXPECT_TRUE(refresh_result);
    EXPECT_EQ(authenticator->get_access_token(), "new_access_token");
}

TEST(AuthClientTransportTest, RefreshTokenReturnsFalseWithoutRefreshToken) {
    asio::io_context io;

    auto inner = std::make_unique<ScriptedTransport>(io.get_executor());
    auto store = std::make_shared<mcp::auth::InMemoryTokenStore>();
    auto oauth_client = std::make_shared<mcp::auth::OAuthHttpClient>(io.get_executor());

    mcp::auth::OAuthConfig config;
    config.client_id = "test";
    config.token_endpoint = "http://localhost/token";
    config.redirect_uri = "http://localhost/callback";

    mcp::auth::TokenResponse token_without_refresh;
    token_without_refresh.access_token = "expired_token";
    store->store("http://server1", token_without_refresh);

    auto authenticator =
        std::make_shared<mcp::auth::OAuthAuthenticator>(store, oauth_client, config, "http://server1");
    mcp::auth::OAuthClientTransport transport(std::move(inner), authenticator);

    bool refresh_result = true;

    asio::co_spawn(
        io, [&]() -> mcp::Task<void> { refresh_result = co_await authenticator->try_refresh_token(); },
        asio::detached);

    io.run();

    EXPECT_FALSE(refresh_result);
}

TEST(AuthClientTransportTest, RefreshTokenReturnsFalseWithNoStoredToken) {
    asio::io_context io;

    auto inner = std::make_unique<ScriptedTransport>(io.get_executor());
    auto store = std::make_shared<mcp::auth::InMemoryTokenStore>();
    auto oauth_client = std::make_shared<mcp::auth::OAuthHttpClient>(io.get_executor());

    mcp::auth::OAuthConfig config;
    config.client_id = "test";
    config.token_endpoint = "http://localhost/token";
    config.redirect_uri = "http://localhost/callback";

    auto authenticator =
        std::make_shared<mcp::auth::OAuthAuthenticator>(store, oauth_client, config, "http://server1");
    mcp::auth::OAuthClientTransport transport(std::move(inner), authenticator);

    bool refresh_result = true;

    asio::co_spawn(
        io, [&]() -> mcp::Task<void> { refresh_result = co_await authenticator->try_refresh_token(); },
        asio::detached);

    io.run();

    EXPECT_FALSE(refresh_result);
}

TEST(AuthClientTransportTest, RefreshPreservesOldRefreshTokenIfNewOneMissing) {
    constexpr unsigned short port = 18108;
    asio::io_context io;

    asio::ip::tcp::acceptor acceptor(io, {asio::ip::make_address("127.0.0.1"), port});

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto socket = co_await acceptor.async_accept(asio::use_awaitable);
            beast::tcp_stream stream(std::move(socket));

            beast::flat_buffer buffer;
            http::request<http::string_body> req;
            co_await http::async_read(stream, buffer, req, asio::use_awaitable);

            json response_json = {{"access_token", "refreshed_at"}, {"token_type", "Bearer"}};

            http::response<http::string_body> res{http::status::ok, req.version()};
            res.set(http::field::content_type, "application/json");
            res.body() = response_json.dump();
            res.prepare_payload();
            co_await http::async_write(stream, res, asio::use_awaitable);

            beast::error_code ec;
            stream.socket().shutdown(asio::ip::tcp::socket::shutdown_both, ec);
        },
        asio::detached);

    auto inner = std::make_unique<ScriptedTransport>(io.get_executor());
    auto store = std::make_shared<mcp::auth::InMemoryTokenStore>();
    auto oauth_client = std::make_shared<mcp::auth::OAuthHttpClient>(io.get_executor());

    mcp::auth::OAuthConfig config;
    config.client_id = "test";
    config.token_endpoint = "http://127.0.0.1:" + std::to_string(port) + "/token";
    config.redirect_uri = "http://localhost/callback";

    mcp::auth::TokenResponse old_token;
    old_token.access_token = "old_at";
    old_token.refresh_token = "keep_this_rt";
    store->store("http://server1", old_token);

    auto authenticator =
        std::make_shared<mcp::auth::OAuthAuthenticator>(store, oauth_client, config, "http://server1");
    mcp::auth::OAuthClientTransport transport(std::move(inner), authenticator);

    asio::co_spawn(
        io, [&]() -> mcp::Task<void> { co_await authenticator->try_refresh_token(); }, asio::detached);

    io.run();

    EXPECT_EQ(authenticator->get_access_token(), "refreshed_at");
    auto stored = store->load("http://server1");
    ASSERT_TRUE(stored.has_value());
    ASSERT_TRUE(stored->refresh_token.has_value());
    EXPECT_EQ(stored->refresh_token.value(), "keep_this_rt");
}

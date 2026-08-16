/**
 * @file auth_integration_test.cpp
 * @brief Tests for OAuth 2.1 integration (Task 19): middleware, bearer extraction, transport wrapper
 */

#include "../test_utils.hpp"

#include <gtest/gtest.h>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <exception>
#include <mcp/auth/oauth.hpp>
#include <mcp/protocol/protocol.hpp>
#include <mcp/server/server.hpp>
#include <mcp/transport/http_client.hpp>
#include <memory>
#include <nlohmann/json.hpp>
#include <queue>
#include <string>
#include <vector>

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using json = nlohmann::json;

namespace {

class RotatingAuthenticator final : public mcp::auth::Authenticator {
   public:
    [[nodiscard]] std::string get_access_token() const override { return access_token_; }

    mcp::Task<bool> try_refresh_token() override {
        ++refresh_count_;
        access_token_ = "refreshed-token";
        co_return true;
    }

    [[nodiscard]] int refresh_count() const { return refresh_count_; }

   private:
    std::string access_token_{"initial-token"};
    int refresh_count_{0};
};

class CloseTransparentTransport final : public mcp::ITransport {
   public:
    mcp::Task<std::string> read_message() override {
        if (incoming_.empty()) {
            throw std::runtime_error("no scripted response");
        }
        auto message = std::move(incoming_.front());
        incoming_.pop();
        co_return message;
    }

    mcp::Task<void> write_message(std::string_view message) override {
        written_.emplace_back(message);
        co_return;
    }

    void close() override { ++close_count_; }

    void enqueue_message(std::string message) { incoming_.push(std::move(message)); }

    [[nodiscard]] const std::vector<std::string>& written() const { return written_; }
    [[nodiscard]] int close_count() const { return close_count_; }

   private:
    std::queue<std::string> incoming_;
    std::vector<std::string> written_;
    int close_count_{0};
};

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

    auto transport = std::make_shared<ScriptedTransport>(io.get_executor());
    auto* transport_ptr = transport.get();

    std::vector<json> responses;
    transport_ptr->set_on_write([&responses, transport_ptr](std::string_view msg) {
        responses.push_back(json::parse(msg));
        if (responses.size() >= 2) {
            transport_ptr->close();
        }
    });

    transport_ptr->enqueue_message(make_initialize_request("1").dump());
    transport_ptr->enqueue_message(make_initialized_notification().dump());

    json call_req{{"jsonrpc", "2.0"},
                  {"id", "2"},
                  {"method", "tools/call"},
                  {"params",
                   {{"name", "echo"},
                    {"arguments", {{"hello", "world"}}},
                    {"_meta", {{"auth_token", "valid_token"}}}}}};
    transport_ptr->enqueue_message(call_req.dump());

    asio::co_spawn(io, server.run(transport, io.get_executor()), asio::detached);
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

    auto transport = std::make_shared<ScriptedTransport>(io.get_executor());
    auto* transport_ptr = transport.get();

    std::vector<json> responses;
    transport_ptr->set_on_write([&responses, transport_ptr](std::string_view msg) {
        responses.push_back(json::parse(msg));
        if (responses.size() >= 2) {
            transport_ptr->close();
        }
    });

    transport_ptr->enqueue_message(make_initialize_request("1").dump());
    transport_ptr->enqueue_message(make_initialized_notification().dump());

    json call_req{{"jsonrpc", "2.0"},
                  {"id", "2"},
                  {"method", "tools/call"},
                  {"params",
                   {{"name", "echo"},
                    {"arguments", {{"hello", "world"}}},
                    {"_meta", {{"auth_token", "wrong_token"}}}}}};
    transport_ptr->enqueue_message(call_req.dump());

    asio::co_spawn(io, server.run(transport, io.get_executor()), asio::detached);
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

    auto transport = std::make_shared<ScriptedTransport>(io.get_executor());
    auto* transport_ptr = transport.get();

    std::vector<json> responses;
    transport_ptr->set_on_write([&responses, transport_ptr](std::string_view msg) {
        responses.push_back(json::parse(msg));
        if (responses.size() >= 2) {
            transport_ptr->close();
        }
    });

    transport_ptr->enqueue_message(make_initialize_request("1").dump());
    transport_ptr->enqueue_message(make_initialized_notification().dump());

    json call_req{{"jsonrpc", "2.0"},
                  {"id", "2"},
                  {"method", "tools/call"},
                  {"params", {{"name", "echo"}, {"arguments", {{"hello", "world"}}}}}};
    transport_ptr->enqueue_message(call_req.dump());

    asio::co_spawn(io, server.run(transport, io.get_executor()), asio::detached);
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

    auto inner = std::make_shared<ScriptedTransport>(io.get_executor());
    auto store = std::make_shared<mcp::auth::InMemoryTokenStore>();
    auto oauth_client = std::make_shared<mcp::auth::OAuthHttpClient>(io.get_executor());

    mcp::auth::OAuthConfig config;
    config.client_id = "test";
    config.token_endpoint = "http://localhost/token";
    config.redirect_uri = "http://localhost/callback";

    auto authenticator =
        std::make_shared<mcp::auth::OAuthAuthenticator>(store, oauth_client, config, "http://server1");
    mcp::auth::OAuthClientTransport transport(inner, authenticator);

    EXPECT_TRUE(authenticator->get_access_token().empty());

    mcp::auth::TokenResponse token;
    token.access_token = "my_token";
    token.token_type = "Bearer";
    authenticator->store_token(std::move(token));

    EXPECT_EQ(authenticator->get_access_token(), "my_token");
}

TEST(AuthClientTransportTest, ReadWritePassThrough) {
    asio::io_context io;

    auto inner = std::make_shared<ScriptedTransport>(io.get_executor());
    auto* inner_ptr = inner.get();
    auto store = std::make_shared<mcp::auth::InMemoryTokenStore>();
    auto oauth_client = std::make_shared<mcp::auth::OAuthHttpClient>(io.get_executor());

    mcp::auth::OAuthConfig config;
    config.client_id = "test";
    config.token_endpoint = "http://localhost/token";
    config.redirect_uri = "http://localhost/callback";

    auto authenticator =
        std::make_shared<mcp::auth::OAuthAuthenticator>(store, oauth_client, config, "http://server1");
    mcp::auth::OAuthClientTransport transport(inner, authenticator);

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

TEST(AuthClientTransportTest, LegacyRetryReplaysTheRequestMatchingTheErrorId) {
    asio::io_context io;
    auto inner = std::make_shared<ScriptedTransport>(io.get_executor());
    auto authenticator = std::make_shared<RotatingAuthenticator>();
    mcp::auth::OAuthClientTransport transport(inner, authenticator);

    const auto request_a =
        json{{"jsonrpc", "2.0"}, {"id", "A"}, {"method", "tools/call"}, {"params", json::object()}}
            .dump();
    const auto request_b =
        json{{"jsonrpc", "2.0"}, {"id", "B"}, {"method", "tools/call"}, {"params", json::object()}}
            .dump();
    inner->enqueue_message(make_error_response("A", mcp::g_UNAUTHORIZED, "Unauthorized").dump());
    inner->enqueue_message(make_result_response("A", json{{"ok", true}}).dump());

    std::string response;
    asio::co_spawn(
        io,
        [&]() -> mcp::Task<void> {
            co_await transport.write_message(request_a);
            co_await transport.write_message(request_b);
            response = co_await transport.read_message();
        },
        asio::detached);
    io.run();

    ASSERT_EQ(inner->written().size(), 3);
    const auto first_request = json::parse(inner->written()[0]);
    const auto second_request = json::parse(inner->written()[1]);
    const auto retried_request = json::parse(inner->written()[2]);
    EXPECT_EQ(first_request["id"], "A");
    EXPECT_EQ(second_request["id"], "B");
    EXPECT_EQ(retried_request["id"], "A");
    EXPECT_EQ(first_request["params"]["_meta"]["auth_token"], "initial-token");
    EXPECT_EQ(retried_request["params"]["_meta"]["auth_token"], "refreshed-token");
    EXPECT_EQ(authenticator->refresh_count(), 1);
    EXPECT_EQ(json::parse(response)["id"], "A");
}

TEST(AuthClientTransportTest, NoResponseRequestsAreEvictedAtConfiguredBound) {
    asio::io_context io;
    auto inner = std::make_shared<ScriptedTransport>(io.get_executor());
    auto authenticator = std::make_shared<RotatingAuthenticator>();
    mcp::auth::OAuthClientTransportOptions options;
    options.max_pending_requests = 2;
    options.pending_request_ttl = std::chrono::hours(1);
    mcp::auth::OAuthClientTransport transport(inner, authenticator, options);

    const auto make_request = [](std::string_view id) {
        return json{{"jsonrpc", "2.0"}, {"id", id}, {"method", "tools/list"}}.dump();
    };
    inner->enqueue_message(make_error_response("A", mcp::g_UNAUTHORIZED, "Unauthorized").dump());
    inner->enqueue_message(make_error_response("C", mcp::g_UNAUTHORIZED, "Unauthorized").dump());
    inner->enqueue_message(make_result_response("C", json{{"ok", true}}).dump());

    std::string evicted_response;
    std::string retained_response;
    asio::co_spawn(
        io,
        [&]() -> mcp::Task<void> {
            co_await transport.write_message(make_request("A"));
            co_await transport.write_message(make_request("B"));
            co_await transport.write_message(make_request("C"));
            evicted_response = co_await transport.read_message();
            retained_response = co_await transport.read_message();
        },
        asio::detached);
    io.run();

    ASSERT_EQ(inner->written().size(), 4);
    EXPECT_EQ(json::parse(evicted_response).at("id"), "A");
    EXPECT_EQ(json::parse(retained_response).at("id"), "C");
    EXPECT_EQ(json::parse(inner->written().back()).at("id"), "C");
    EXPECT_EQ(authenticator->refresh_count(), 1);
}

TEST(AuthClientTransportTest, PendingReplayCorrelationExpires) {
    asio::io_context io;
    auto inner = std::make_shared<ScriptedTransport>(io.get_executor());
    auto authenticator = std::make_shared<RotatingAuthenticator>();
    mcp::auth::OAuthClientTransportOptions options;
    options.pending_request_ttl = std::chrono::milliseconds(5);
    mcp::auth::OAuthClientTransport transport(inner, authenticator, options);

    const auto request = json{{"jsonrpc", "2.0"}, {"id", "expired"}, {"method", "tools/list"}}.dump();
    inner->enqueue_message(make_error_response("expired", mcp::g_UNAUTHORIZED, "Unauthorized").dump());

    std::string response;
    asio::co_spawn(
        io,
        [&]() -> mcp::Task<void> {
            co_await transport.write_message(request);
            asio::steady_timer expiry_wait(io);
            expiry_wait.expires_after(std::chrono::milliseconds(25));
            co_await expiry_wait.async_wait(asio::use_awaitable);
            response = co_await transport.read_message();
        },
        asio::detached);
    io.run();

    ASSERT_EQ(inner->written().size(), 1);
    EXPECT_EQ(json::parse(response).at("id"), "expired");
    EXPECT_EQ(authenticator->refresh_count(), 0);
}

TEST(AuthClientTransportTest, CloseClearsPendingReplayCorrelationAndIsIdempotent) {
    asio::io_context io;
    auto inner = std::make_shared<CloseTransparentTransport>();
    auto authenticator = std::make_shared<RotatingAuthenticator>();
    mcp::auth::OAuthClientTransport transport(inner, authenticator);

    const auto request = json{{"jsonrpc", "2.0"}, {"id", "closed"}, {"method", "tools/list"}}.dump();
    inner->enqueue_message(make_error_response("closed", mcp::g_UNAUTHORIZED, "Unauthorized").dump());

    std::string response;
    asio::co_spawn(
        io,
        [&]() -> mcp::Task<void> {
            co_await transport.write_message(request);
            transport.close();
            transport.close();
            response = co_await transport.read_message();
        },
        asio::detached);
    io.run();

    ASSERT_EQ(inner->written().size(), 1);
    EXPECT_EQ(inner->close_count(), 1);
    EXPECT_EQ(json::parse(response).at("id"), "closed");
    EXPECT_EQ(authenticator->refresh_count(), 0);
}

TEST(AuthClientTransportTest, HttpTransportUsesAuthorizationHeader) {
    constexpr unsigned short port = 18109;
    asio::io_context io;
    asio::ip::tcp::acceptor acceptor(io, {asio::ip::make_address("127.0.0.1"), port});

    std::string authorization_header;
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto socket = co_await acceptor.async_accept(asio::use_awaitable);
            beast::tcp_stream stream(std::move(socket));
            beast::flat_buffer buffer;
            http::request<http::string_body> request;
            co_await http::async_read(stream, buffer, request, asio::use_awaitable);
            authorization_header = std::string(request[http::field::authorization]);

            http::response<http::string_body> response{http::status::accepted, request.version()};
            response.content_length(0);
            co_await http::async_write(stream, response, asio::use_awaitable);
        },
        asio::detached);

    auto inner = std::make_shared<mcp::HttpClientTransport>(
        io.get_executor(), "http://127.0.0.1:" + std::to_string(port) + "/mcp");
    auto store = std::make_shared<mcp::auth::InMemoryTokenStore>();
    auto oauth_client = std::make_shared<mcp::auth::OAuthHttpClient>(io.get_executor());

    mcp::auth::OAuthConfig config;
    config.client_id = "test";
    config.token_endpoint = "http://localhost/token";
    config.redirect_uri = "http://localhost/callback";

    mcp::auth::TokenResponse token;
    token.access_token = "header-token";
    store->store("http://server1", token);

    auto authenticator =
        std::make_shared<mcp::auth::OAuthAuthenticator>(store, oauth_client, config, "http://server1");
    mcp::auth::OAuthClientTransport transport(inner, authenticator);

    asio::co_spawn(
        io,
        [&]() -> mcp::Task<void> {
            co_await transport.write_message(R"({"jsonrpc":"2.0","id":1,"method":"ping"})");
            transport.close();
        },
        asio::detached);

    io.run();

    EXPECT_EQ(authorization_header, "Bearer header-token");
}

TEST(AuthClientTransportTest, FailedHttpRetryDoesNotLeaveRequestEligibleForReplay) {
    asio::io_context io;
    asio::ip::tcp::acceptor initial_acceptor(io);
    initial_acceptor.open(asio::ip::tcp::v4());
    initial_acceptor.set_option(asio::socket_base::reuse_address(true));
    initial_acceptor.bind({asio::ip::make_address("127.0.0.1"), 0});
    initial_acceptor.listen();
    const auto port = initial_acceptor.local_endpoint().port();

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto socket = co_await initial_acceptor.async_accept(asio::use_awaitable);
            initial_acceptor.close();

            beast::tcp_stream stream(std::move(socket));
            beast::flat_buffer buffer;
            http::request<http::string_body> request;
            co_await http::async_read(stream, buffer, request, asio::use_awaitable);

            http::response<http::empty_body> response{http::status::unauthorized, request.version()};
            response.set(http::field::www_authenticate, "Bearer");
            response.keep_alive(false);
            response.content_length(0);
            co_await http::async_write(stream, response, asio::use_awaitable);
        },
        asio::detached);

    auto inner = std::make_shared<mcp::HttpClientTransport>(
        io.get_executor(), "http://127.0.0.1:" + std::to_string(port) + "/mcp");
    auto authenticator = std::make_shared<RotatingAuthenticator>();
    mcp::auth::OAuthClientTransport transport(inner, authenticator);

    const auto original_request =
        json{{"jsonrpc", "2.0"}, {"id", "A"}, {"method", "tools/list"}}.dump();
    const auto probe_notification = json{{"jsonrpc", "2.0"}, {"method", "notifications/probe"}}.dump();

    bool retry_failed = false;
    std::string response_wire;
    std::vector<json> replay_server_requests;
    std::exception_ptr controller_error;
    asio::co_spawn(
        io,
        [&]() -> mcp::Task<void> {
            try {
                co_await transport.write_message(original_request);
            } catch (const std::exception&) {
                retry_failed = true;
            }

            auto replay_acceptor = std::make_shared<asio::ip::tcp::acceptor>(io);
            replay_acceptor->open(asio::ip::tcp::v4());
            replay_acceptor->set_option(asio::socket_base::reuse_address(true));
            replay_acceptor->bind({asio::ip::make_address("127.0.0.1"), port});
            replay_acceptor->listen();

            asio::co_spawn(
                io,
                [replay_acceptor, &replay_server_requests]() -> asio::awaitable<void> {
                    for (int request_index = 0; request_index < 2; ++request_index) {
                        boost::system::error_code accept_error;
                        auto socket = co_await replay_acceptor->async_accept(
                            asio::redirect_error(asio::use_awaitable, accept_error));
                        if (accept_error == asio::error::operation_aborted) {
                            co_return;
                        }
                        if (accept_error) {
                            throw boost::system::system_error(accept_error);
                        }

                        beast::tcp_stream stream(std::move(socket));
                        beast::flat_buffer buffer;
                        http::request<http::string_body> request;
                        co_await http::async_read(stream, buffer, request, asio::use_awaitable);
                        replay_server_requests.push_back(json::parse(request.body()));

                        const auto response_body =
                            request_index == 0
                                ? make_error_response("A", mcp::g_UNAUTHORIZED, "Unauthorized").dump()
                                : make_result_response("A", json{{"unexpectedReplay", true}}).dump();
                        http::response<http::string_body> response{http::status::ok, request.version()};
                        response.set(http::field::content_type, "application/json");
                        response.keep_alive(false);
                        response.body() = response_body;
                        response.prepare_payload();
                        co_await http::async_write(stream, response, asio::use_awaitable);
                    }
                },
                asio::detached);

            co_await transport.write_message(probe_notification);
            response_wire = co_await transport.read_message();
            replay_acceptor->close();
            transport.close();
        },
        [&controller_error](std::exception_ptr error) { controller_error = std::move(error); });

    io.run();

    EXPECT_EQ(controller_error, nullptr);
    EXPECT_TRUE(retry_failed);
    EXPECT_EQ(authenticator->refresh_count(), 1);
    ASSERT_EQ(replay_server_requests.size(), 1);
    EXPECT_EQ(replay_server_requests.front().at("method"), "notifications/probe");
    ASSERT_FALSE(response_wire.empty());
    EXPECT_EQ(json::parse(response_wire).at("error").at("code"), mcp::g_UNAUTHORIZED);
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

    auto inner = std::make_shared<ScriptedTransport>(io.get_executor());
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
    mcp::auth::OAuthClientTransport transport(inner, authenticator);

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

    auto inner = std::make_shared<ScriptedTransport>(io.get_executor());
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
    mcp::auth::OAuthClientTransport transport(inner, authenticator);

    bool refresh_result = true;

    asio::co_spawn(
        io, [&]() -> mcp::Task<void> { refresh_result = co_await authenticator->try_refresh_token(); },
        asio::detached);

    io.run();

    EXPECT_FALSE(refresh_result);
}

TEST(AuthClientTransportTest, RefreshTokenReturnsFalseWithNoStoredToken) {
    asio::io_context io;

    auto inner = std::make_shared<ScriptedTransport>(io.get_executor());
    auto store = std::make_shared<mcp::auth::InMemoryTokenStore>();
    auto oauth_client = std::make_shared<mcp::auth::OAuthHttpClient>(io.get_executor());

    mcp::auth::OAuthConfig config;
    config.client_id = "test";
    config.token_endpoint = "http://localhost/token";
    config.redirect_uri = "http://localhost/callback";

    auto authenticator =
        std::make_shared<mcp::auth::OAuthAuthenticator>(store, oauth_client, config, "http://server1");
    mcp::auth::OAuthClientTransport transport(inner, authenticator);

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

    auto inner = std::make_shared<ScriptedTransport>(io.get_executor());
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
    mcp::auth::OAuthClientTransport transport(inner, authenticator);

    asio::co_spawn(
        io, [&]() -> mcp::Task<void> { co_await authenticator->try_refresh_token(); }, asio::detached);

    io.run();

    EXPECT_EQ(authenticator->get_access_token(), "refreshed_at");
    auto stored = store->load("http://server1");
    ASSERT_TRUE(stored.has_value());
    ASSERT_TRUE(stored->refresh_token.has_value());
    EXPECT_EQ(stored->refresh_token.value(), "keep_this_rt");
}

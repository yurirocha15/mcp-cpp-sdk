#include "../support/hang_guard.hpp"
#include "../test_utils.hpp"

#include "../../src/detail/principal.hpp"

#include "mcp/auth/oauth.hpp"
#include "mcp/server/server.hpp"
#include "mcp/transport/http_server.hpp"
#include "mcp/transport/http_session_manager.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using nlohmann::json;
using Headers = std::vector<std::pair<std::string, std::string>>;

struct Reply {
    unsigned int status{0};
    std::string body;
    std::string www_authenticate;
    std::optional<std::string> session_id;
};

// Everything is taken by value: this is a coroutine, and callers pass temporaries.
mcp::Task<Reply> send(asio::any_io_executor executor, unsigned short port, std::string body,
                      Headers headers, http::verb verb = http::verb::post) {
    beast::tcp_stream stream(executor);
    asio::ip::tcp::resolver resolver(executor);
    auto endpoints =
        co_await resolver.async_resolve("127.0.0.1", std::to_string(port), asio::use_awaitable);
    co_await stream.async_connect(*endpoints.begin(), asio::use_awaitable);

    http::request<http::string_body> request{verb, "/mcp", 11};
    request.set(http::field::host, "127.0.0.1");
    request.set(http::field::content_type, "application/json");
    request.set(http::field::accept, "application/json, text/event-stream");
    for (const auto& [name, value] : headers) {
        request.insert(name, value);
    }
    request.body() = std::move(body);
    request.prepare_payload();
    co_await http::async_write(stream, request, asio::use_awaitable);

    beast::flat_buffer buffer;
    http::response<http::string_body> response;
    co_await http::async_read(stream, buffer, response, asio::use_awaitable);

    Reply reply;
    reply.status = response.result_int();
    reply.body = response.body();
    reply.www_authenticate = std::string(response[http::field::www_authenticate]);
    if (auto session = response.find("MCP-Session-Id"); session != response.end()) {
        reply.session_id = std::string(session->value());
    }
    beast::error_code ignored;
    stream.socket().shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
    co_return reply;
}

json whoami(const json& id) {
    return make_modern_request(id, "tools/call", {{"name", "whoami"}, {"arguments", json::object()}});
}

/// The headers a 2026-07-28 client sends with a tools/call of `whoami`, plus @p authorization.
Headers modern_headers(std::optional<std::string> authorization) {
    Headers headers{
        {"MCP-Protocol-Version", "2026-07-28"}, {"Mcp-Method", "tools/call"}, {"Mcp-Name", "whoami"}};
    if (authorization) {
        headers.emplace_back("Authorization", std::move(*authorization));
    }
    return headers;
}

/// The principal a tools/call of `whoami` reported, or the reply itself when it has none.
std::string principal_of(const Reply& reply) {
    // A legacy session's answer may come as an SSE event; its data line is the JSON-RPC message.
    std::string_view text = reply.body;
    if (const auto data = text.find("data: "); data != std::string_view::npos) {
        text.remove_prefix(data + 6);
        text = text.substr(0, text.find('\n'));
    }
    const auto body = json::parse(text, nullptr, false);
    if (body.is_discarded() || !body.contains("result")) {
        return "<no result: " + reply.body + ">";
    }
    return body["result"]["structuredContent"]["principal"].get<std::string>();
}

std::string expected_digest(std::string_view authorization) {
    constexpr std::string_view hex_digits = "0123456789abcdef";
    std::string principal = "h:";
    authorization.remove_prefix(std::string_view("Bearer ").size());
    for (const unsigned char byte : mcp::auth::detail::sha256(std::string(authorization))) {
        principal += hex_digits[byte >> 4U];
        principal += hex_digits[byte & 0x0FU];
    }
    return principal;
}

/// Waits, a millisecond at a time, until @p condition holds. Gives up after g_hang_guard so a broken
/// rendezvous fails the test instead of hanging the run.
template <typename Condition>
mcp::Task<bool> wait_until(Condition condition) {
    auto executor = co_await asio::this_coro::executor;
    const auto give_up = std::chrono::steady_clock::now() + g_hang_guard;
    while (!condition() && std::chrono::steady_clock::now() < give_up) {
        asio::steady_timer timer(executor, std::chrono::milliseconds(1));
        co_await timer.async_wait(asio::use_awaitable);
    }
    co_return condition();
}

/// A server whose `whoami` tool answers with Context::principal(). `gathered` holds the calls back
/// until that many are in flight, so their messages wait in the transport's queue together.
std::unique_ptr<mcp::Server> make_server(std::shared_ptr<std::atomic<int>> arrivals = nullptr,
                                         int gathered = 1) {
    mcp::ServerCapabilities caps;
    caps.tools = mcp::ServerCapabilities::ToolsCapability{};
    auto server = std::make_unique<mcp::Server>(mcp::Implementation{"principal", "1.0"}, caps);
    server->add_tool<json, json>(
        "whoami", "Answers with the caller's principal", json{{"type", "object"}},
        [arrivals, gathered](mcp::Context& ctx, json /*args*/) -> mcp::Task<json> {
            if (arrivals) {
                arrivals->fetch_add(1);
                const auto all_arrived = [arrivals, gathered] { return arrivals->load() >= gathered; };
                co_await wait_until(all_arrived);
            }
            co_return json{{"principal", std::string(ctx.principal())}};
        });
    return server;
}

/// Forwards everything to an HttpServerTransport, read_incoming() included.
class ForwardingTransport final : public mcp::ITransport {
   public:
    explicit ForwardingTransport(std::shared_ptr<mcp::HttpServerTransport> inner)
        : inner_(std::move(inner)) {}

    mcp::Task<std::string> read_message() override { return inner_->read_message(); }
    mcp::Task<mcp::IncomingMessage> read_incoming() override { return inner_->read_incoming(); }
    mcp::Task<void> write_message(std::string_view message) override {
        return inner_->write_message(message);
    }
    void close() override { inner_->close(); }
    mcp::HttpServerTransport* http_server_transport() noexcept override { return inner_.get(); }

   private:
    std::shared_ptr<mcp::HttpServerTransport> inner_;
};

class HttpPrincipalTest : public ::testing::Test {
   protected:
    asio::io_context io_ctx_;
    std::shared_ptr<mcp::HttpServerTransport> transport_ =
        std::make_shared<mcp::HttpServerTransport>(io_ctx_.get_executor(), "127.0.0.1", 0);
    std::unique_ptr<mcp::Server> server_ = make_server();

    /// Runs the server on the transport (or on @p served), then @p exchange, then closes it.
    template <typename Exchange>
    void run(Exchange exchange, std::shared_ptr<mcp::ITransport> served = nullptr) {
        if (!served) {
            served = transport_;
        }
        asio::co_spawn(io_ctx_, transport_->listen(), asio::detached);
        asio::co_spawn(io_ctx_, serve(server_.get(), std::move(served), io_ctx_.get_executor()),
                       asio::detached);
        asio::co_spawn(
            io_ctx_,
            [this, exchange = std::move(exchange)]() mutable -> mcp::Task<void> {
                co_await exchange();
                transport_->close();
            },
            asio::detached);
        io_ctx_.run();
    }

    static mcp::Task<void> serve(mcp::Server* server, std::shared_ptr<mcp::ITransport> transport,
                                 asio::any_io_executor executor) {
        co_await server->run(std::move(transport), std::move(executor));
    }

    mcp::Task<Reply> call(std::optional<std::string> authorization, json id = 1) {
        return send(io_ctx_.get_executor(), transport_->port(), whoami(id).dump(),
                    modern_headers(std::move(authorization)));
    }
};

namespace {

constexpr const char* g_legacy_version = "2025-11-25";

Headers legacy_headers(const std::string& authorization, std::optional<std::string> session) {
    Headers headers{{"MCP-Protocol-Version", g_legacy_version}, {"Authorization", authorization}};
    if (session) {
        headers.emplace_back("MCP-Session-Id", std::move(*session));
    }
    return headers;
}

std::string legacy_initialize() {
    return json{{"jsonrpc", "2.0"},
                {"id", 1},
                {"method", "initialize"},
                {"params",
                 {{"protocolVersion", g_legacy_version},
                  {"capabilities", json::object()},
                  {"clientInfo", {{"name", "t"}, {"version", "1"}}}}}}
        .dump();
}

std::string legacy_ping() { return json{{"jsonrpc", "2.0"}, {"id", 2}, {"method", "ping"}}.dump(); }

/// What each kind of request got when it named a session of another principal, then what its owner
/// got afterwards.
struct SessionReplies {
    Reply post;
    Reply get;
    Reply remove;
    Reply owner;
};

mcp::Task<SessionReplies> use_someone_elses_session(asio::any_io_executor executor,
                                                    unsigned short port) {
    auto created = co_await send(executor, port, legacy_initialize(),
                                 legacy_headers("Bearer alice", std::nullopt));
    EXPECT_EQ(created.status, 200u) << created.body;
    EXPECT_TRUE(created.session_id.has_value());
    const auto session = created.session_id.value_or("");
    SessionReplies replies;
    replies.post = co_await send(executor, port, legacy_ping(), legacy_headers("Bearer bob", session));
    replies.get =
        co_await send(executor, port, "", legacy_headers("Bearer bob", session), http::verb::get);
    replies.remove =
        co_await send(executor, port, "", legacy_headers("Bearer bob", session), http::verb::delete_);
    replies.owner =
        co_await send(executor, port, legacy_ping(), legacy_headers("Bearer alice", session));
    co_return replies;
}

}  // namespace

TEST_F(HttpPrincipalTest, ASessionServesOnlyThePrincipalThatCreatedIt) {
    transport_->set_bearer_token_validator([](std::string_view /*token*/) { return true; });
    SessionReplies replies;
    run([&]() -> mcp::Task<void> {
        replies = co_await use_someone_elses_session(io_ctx_.get_executor(), transport_->port());
    });

    EXPECT_EQ(replies.post.status, 404u) << replies.post.body;
    EXPECT_EQ(replies.get.status, 404u) << replies.get.body;
    EXPECT_EQ(replies.remove.status, 404u) << replies.remove.body;
    EXPECT_EQ(replies.owner.status, 200u) << replies.owner.body;
}

TEST(SessionManagerPrincipal, ASessionServesOnlyThePrincipalThatCreatedIt) {
    asio::io_context io;
    mcp::StreamableHttpSessionManager manager(
        io.get_executor(), "127.0.0.1", 19341,
        [](const asio::any_io_executor&) { return make_server(); });
    manager.set_bearer_token_validator([](std::string_view /*token*/) { return true; });
    asio::co_spawn(io, manager.listen(), asio::detached);

    SessionReplies replies;
    asio::co_spawn(
        io,
        [&]() -> mcp::Task<void> {
            replies = co_await use_someone_elses_session(io.get_executor(), 19341);
            manager.close();
        },
        asio::detached);
    io.run();

    EXPECT_EQ(replies.post.status, 404u) << replies.post.body;
    EXPECT_EQ(replies.get.status, 404u) << replies.get.body;
    EXPECT_EQ(replies.remove.status, 404u) << replies.remove.body;
    EXPECT_EQ(replies.owner.status, 200u) << replies.owner.body;
}

TEST(SessionManagerPrincipal, ANamedPrincipalKeepsItsSessionAcrossATokenRefresh) {
    asio::io_context io;
    mcp::StreamableHttpSessionManager manager(
        io.get_executor(), "127.0.0.1", 19342,
        [](const asio::any_io_executor&) { return make_server(); });
    manager.set_bearer_token_validator(
        [](std::string_view /*token*/) -> mcp::TokenValidation { return {true, "alice"}; });
    asio::co_spawn(io, manager.listen(), asio::detached);

    Reply refreshed;
    asio::co_spawn(
        io,
        [&]() -> mcp::Task<void> {
            auto created = co_await send(io.get_executor(), 19342, legacy_initialize(),
                                         legacy_headers("Bearer first", std::nullopt));
            refreshed = co_await send(io.get_executor(), 19342, legacy_ping(),
                                      legacy_headers("Bearer second", created.session_id));
            manager.close();
        },
        asio::detached);
    io.run();

    EXPECT_EQ(refreshed.status, 200u) << refreshed.body;
}

TEST_F(HttpPrincipalTest, ValidatorIdIsUsed) {
    transport_->set_bearer_token_validator([](std::string_view token) -> mcp::TokenValidation {
        if (token == "alice-token") {
            return {true, "alice"};
        }
        return false;
    });
    Reply reply;
    run([&]() -> mcp::Task<void> { reply = co_await call("Bearer alice-token"); });

    EXPECT_EQ(reply.status, 200u) << reply.body;
    EXPECT_EQ(principal_of(reply), "alice");
}

TEST_F(HttpPrincipalTest, AsyncValidatorIdIsUsed) {
    transport_->set_async_bearer_token_validator(
        [](std::string token) -> mcp::Task<mcp::TokenValidation> {
            co_return mcp::TokenValidation(token == "alice-token", "alice");
        });
    Reply reply;
    run([&]() -> mcp::Task<void> { reply = co_await call("Bearer alice-token"); });

    EXPECT_EQ(reply.status, 200u) << reply.body;
    EXPECT_EQ(principal_of(reply), "alice");
}

TEST_F(HttpPrincipalTest, FallbackHashOfAuthorizationHeader) {
    transport_->set_bearer_token_validator([](std::string_view /*token*/) { return true; });
    Reply first;
    Reply refreshed;
    run([&]() -> mcp::Task<void> {
        first = co_await call("Bearer first-token");
        refreshed = co_await call("Bearer second-token");
    });

    EXPECT_EQ(principal_of(first), expected_digest("Bearer first-token"));
    // Without an id from the validator, a new token is a new principal.
    EXPECT_EQ(principal_of(refreshed), expected_digest("Bearer second-token"));
    EXPECT_NE(principal_of(first), principal_of(refreshed));
}

TEST_F(HttpPrincipalTest, FallbackHashWithoutAValidator) {
    Reply reply;
    run([&]() -> mcp::Task<void> { reply = co_await call("Bearer unchecked"); });

    EXPECT_EQ(principal_of(reply), expected_digest("Bearer unchecked"));
}

TEST_F(HttpPrincipalTest, AnonymousWhenNoHeader) {
    Reply reply;
    run([&]() -> mcp::Task<void> { reply = co_await call(std::nullopt); });

    EXPECT_EQ(reply.status, 200u) << reply.body;
    EXPECT_EQ(principal_of(reply), "");
}

TEST_F(HttpPrincipalTest, RawTokenNeverInDiagnostics) {
    const std::string secret = "s3cr3t-T0KEN-value";
    transport_->set_bearer_token_validator(
        [secret](std::string_view token) { return token == secret; });
    Reply accepted;
    Reply refused;
    run([&]() -> mcp::Task<void> {
        accepted = co_await call("Bearer " + secret);
        refused = co_await call("Bearer " + secret + "-wrong");
    });

    EXPECT_EQ(accepted.status, 200u) << accepted.body;
    EXPECT_EQ(accepted.body.find(secret), std::string::npos) << accepted.body;
    EXPECT_EQ(refused.status, 401u) << refused.body;
    EXPECT_EQ(refused.body.find(secret), std::string::npos) << refused.body;
    EXPECT_EQ(refused.www_authenticate.find(secret), std::string::npos) << refused.www_authenticate;
}

constexpr int g_calls = 8;

TEST_F(HttpPrincipalTest, InterleavedPrincipalsStayBoundToTheirMessages) {
    auto arrivals = std::make_shared<std::atomic<int>>(0);
    server_ = make_server(arrivals, g_calls);
    transport_->set_bearer_token_validator([](std::string_view token) -> mcp::TokenValidation {
        return {true, "user-" + std::string(token)};
    });
    std::vector<Reply> replies(g_calls);
    run([&]() -> mcp::Task<void> {
        int finished = 0;
        auto client = [&](int index) -> mcp::Task<void> {
            replies[index] = co_await call("Bearer " + std::to_string(index % 2), index);
            ++finished;
        };
        for (int index = 0; index < g_calls; ++index) {
            asio::co_spawn(io_ctx_, client(index), asio::detached);
        }
        co_await wait_until([&finished] { return finished == g_calls; });
    });

    ASSERT_EQ(arrivals->load(), g_calls);
    for (int index = 0; index < g_calls; ++index) {
        EXPECT_EQ(principal_of(replies[index]), "user-" + std::to_string(index % 2)) << index;
    }
}

TEST_F(HttpPrincipalTest, SyncBoolValidatorStillCompiles) {
    transport_->set_bearer_token_validator([](std::string_view token) { return token == "ok"; });
    Reply accepted;
    Reply refused;
    run([&]() -> mcp::Task<void> {
        accepted = co_await call("Bearer ok");
        refused = co_await call("Bearer no");
    });

    EXPECT_EQ(accepted.status, 200u) << accepted.body;
    EXPECT_EQ(refused.status, 401u) << refused.body;
}

TEST_F(HttpPrincipalTest, AsyncValidatorReturningBoolStillWorks) {
    transport_->set_async_bearer_token_validator(
        [](std::string token) -> mcp::Task<mcp::TokenValidation> { co_return token == "ok"; });
    Reply accepted;
    Reply refused;
    run([&]() -> mcp::Task<void> {
        accepted = co_await call("Bearer ok");
        refused = co_await call("Bearer no");
    });

    EXPECT_EQ(principal_of(accepted), expected_digest("Bearer ok"));
    EXPECT_EQ(refused.status, 401u) << refused.body;
}

TEST_F(HttpPrincipalTest, WrapperForwardingReadIncomingKeepsThePrincipal) {
    transport_->set_bearer_token_validator(
        [](std::string_view /*token*/) -> mcp::TokenValidation { return {true, "alice"}; });
    Reply reply;
    run([&]() -> mcp::Task<void> { reply = co_await call("Bearer any"); },
        std::make_shared<ForwardingTransport>(transport_));

    EXPECT_EQ(principal_of(reply), "alice");
}

TEST_F(HttpPrincipalTest, LegacySessionRequestsCarryThePrincipal) {
    transport_->set_bearer_token_validator(
        [](std::string_view /*token*/) -> mcp::TokenValidation { return {true, "alice"}; });
    Reply reply;
    run([&]() -> mcp::Task<void> {
        const Headers authorization{{"Authorization", "Bearer any"}};
        const auto initialized = co_await send(io_ctx_.get_executor(), transport_->port(),
                                               make_initialize_request("init").dump(), authorization);
        auto headers = authorization;
        headers.emplace_back("MCP-Session-Id", initialized.session_id.value_or(""));
        headers.emplace_back("MCP-Protocol-Version",
                             std::string(mcp::g_LATEST_LEGACY_PROTOCOL_VERSION));
        co_await send(io_ctx_.get_executor(), transport_->port(),
                      make_initialized_notification().dump(), headers);
        const json call_json = {{"jsonrpc", "2.0"},
                                {"id", 2},
                                {"method", "tools/call"},
                                {"params", {{"name", "whoami"}, {"arguments", json::object()}}}};
        reply = co_await send(io_ctx_.get_executor(), transport_->port(), call_json.dump(), headers);
    });

    EXPECT_EQ(principal_of(reply), "alice");
}

TEST(HttpPrincipalDirect, RequestContextPrincipalReachesTheHandler) {
    asio::io_context io_ctx;
    auto server = make_server();
    std::string wire;
    asio::co_spawn(
        io_ctx,
        [&]() -> mcp::Task<void> {
            mcp::RequestContext request_context;
            request_context.principal = "bob";
            wire = co_await server->dispatch_request_direct(whoami(1), std::move(request_context));
        },
        asio::detached);
    io_ctx.run();

    const auto body = json::parse(wire);
    EXPECT_EQ(body["result"]["structuredContent"]["principal"], "bob") << wire;
}

class HttpPrincipalManagerTest : public ::testing::Test {
   protected:
    asio::io_context io_ctx_;

    static mcp::StreamableHttpSessionManager::ServerFactory factory() {
        return [](const asio::any_io_executor&) { return make_server(); };
    }

    template <typename Exchange>
    void run(mcp::StreamableHttpSessionManager& manager, Exchange exchange) {
        asio::co_spawn(io_ctx_, manager.listen(), asio::detached);
        asio::co_spawn(
            io_ctx_,
            [&manager, exchange = std::move(exchange)]() mutable -> mcp::Task<void> {
                co_await exchange();
                manager.close();
            },
            asio::detached);
        io_ctx_.run();
    }
};

TEST_F(HttpPrincipalManagerTest, ValidatorIdIsUsedForAModernRequest) {
    constexpr unsigned short g_port = 20451;
    mcp::StreamableHttpSessionManager manager(io_ctx_.get_executor(), "127.0.0.1", g_port, factory());
    manager.set_async_bearer_token_validator([](std::string token) -> mcp::Task<mcp::TokenValidation> {
        co_return mcp::TokenValidation(token == "alice-token", "alice");
    });
    Reply reply;
    run(manager, [&]() -> mcp::Task<void> {
        reply = co_await send(io_ctx_.get_executor(), g_port, whoami(1).dump(),
                              modern_headers("Bearer alice-token"));
    });

    EXPECT_EQ(reply.status, 200u) << reply.body;
    EXPECT_EQ(principal_of(reply), "alice");
}

TEST_F(HttpPrincipalManagerTest, FallbackHashOfAuthorizationHeader) {
    constexpr unsigned short g_port = 20452;
    mcp::StreamableHttpSessionManager manager(io_ctx_.get_executor(), "127.0.0.1", g_port, factory());
    manager.set_bearer_token_validator([](std::string_view /*token*/) { return true; });
    Reply reply;
    run(manager, [&]() -> mcp::Task<void> {
        reply = co_await send(io_ctx_.get_executor(), g_port, whoami(1).dump(),
                              modern_headers("Bearer some-token"));
    });

    EXPECT_EQ(principal_of(reply), expected_digest("Bearer some-token"));
}

TEST_F(HttpPrincipalManagerTest, AnonymousWhenNoHeader) {
    constexpr unsigned short g_port = 20453;
    mcp::StreamableHttpSessionManager manager(io_ctx_.get_executor(), "127.0.0.1", g_port, factory());
    Reply reply;
    run(manager, [&]() -> mcp::Task<void> {
        reply = co_await send(io_ctx_.get_executor(), g_port, whoami(1).dump(),
                              modern_headers(std::nullopt));
    });

    EXPECT_EQ(reply.status, 200u) << reply.body;
    EXPECT_EQ(principal_of(reply), "");
}

TEST_F(HttpPrincipalManagerTest, StatelessJsonRequestsCarryThePrincipal) {
    constexpr unsigned short g_port = 20454;
    mcp::StreamableHttpSessionManager manager(io_ctx_.get_executor(), "127.0.0.1", g_port, factory());
    manager.set_stateless_json_mode(true);
    manager.set_bearer_token_validator(
        [](std::string_view /*token*/) -> mcp::TokenValidation { return {true, "alice"}; });
    Reply reply;
    run(manager, [&]() -> mcp::Task<void> {
        const json call_json = {{"jsonrpc", "2.0"},
                                {"id", 1},
                                {"method", "tools/call"},
                                {"params", {{"name", "whoami"}, {"arguments", json::object()}}}};
        Headers headers{{"Authorization", "Bearer any"}};
        reply = co_await send(io_ctx_.get_executor(), g_port, call_json.dump(), std::move(headers));
    });

    EXPECT_EQ(reply.status, 200u) << reply.body;
    EXPECT_EQ(principal_of(reply), "alice");
}

TEST_F(HttpPrincipalManagerTest, SessionRequestsCarryNoPrincipal) {
    constexpr unsigned short g_port = 20455;
    mcp::StreamableHttpSessionManager manager(io_ctx_.get_executor(), "127.0.0.1", g_port, factory());
    manager.set_bearer_token_validator(
        [](std::string_view /*token*/) -> mcp::TokenValidation { return {true, "alice"}; });
    Reply reply;
    run(manager, [&]() -> mcp::Task<void> {
        const Headers authorization{{"Authorization", "Bearer any"}};
        const auto initialized = co_await send(io_ctx_.get_executor(), g_port,
                                               make_initialize_request("init").dump(), authorization);
        auto headers = authorization;
        headers.emplace_back("MCP-Session-Id", initialized.session_id.value_or(""));
        headers.emplace_back("MCP-Protocol-Version",
                             std::string(mcp::g_LATEST_LEGACY_PROTOCOL_VERSION));
        co_await send(io_ctx_.get_executor(), g_port, make_initialized_notification().dump(), headers);
        const json call_json = {{"jsonrpc", "2.0"},
                                {"id", 2},
                                {"method", "tools/call"},
                                {"params", {{"name", "whoami"}, {"arguments", json::object()}}}};
        reply = co_await send(io_ctx_.get_executor(), g_port, call_json.dump(), headers);
    });

    EXPECT_EQ(reply.status, 200u) << reply.body;
    EXPECT_EQ(principal_of(reply), "");
}

}  // namespace

// The name of the scheme is case-insensitive, so these are one token and one principal.
TEST(HttpPrincipal, OneTokenIsOnePrincipalHoweverTheHeaderSpellsIt) {
    const auto principal = mcp::detail::request_principal(std::nullopt, "Bearer tok");
    EXPECT_EQ(mcp::detail::request_principal(std::nullopt, "bearer tok"), principal);
    EXPECT_EQ(mcp::detail::request_principal(std::nullopt, "BEARER tok"), principal);
    EXPECT_EQ(mcp::detail::request_principal(std::nullopt, "Bearer   tok"), principal);
    EXPECT_NE(mcp::detail::request_principal(std::nullopt, "Bearer other"), principal);
    EXPECT_NE(mcp::detail::request_principal(std::nullopt, "Basic dG9rOg=="), principal);
}

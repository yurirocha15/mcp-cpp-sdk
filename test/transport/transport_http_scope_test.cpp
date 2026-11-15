#include "../support/hang_guard.hpp"
#include "../test_utils.hpp"

#include "mcp/auth/challenge.hpp"
#include "mcp/server/server.hpp"
#include "mcp/transport/http_server.hpp"
#include "mcp/transport/http_session_manager.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <memory>
#include <optional>
#include <stdexcept>
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
};

// Everything is taken by value: this is a coroutine, and callers pass temporaries.
mcp::Task<Reply> post(asio::any_io_executor executor, unsigned short port, std::string body,
                      Headers headers) {
    beast::tcp_stream stream(executor);
    asio::ip::tcp::resolver resolver(executor);
    auto endpoints =
        co_await resolver.async_resolve("127.0.0.1", std::to_string(port), asio::use_awaitable);
    co_await stream.async_connect(*endpoints.begin(), asio::use_awaitable);

    http::request<http::string_body> request{http::verb::post, "/mcp", 11};
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
    beast::error_code ignored;
    stream.socket().shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
    co_return reply;
}

/// The scopes a request needs: `files:write` for the `write_file` tool, `files:read` for every
/// other tool call, and none for anything else.
std::vector<std::string> files_scopes(const mcp::ScopeRequestContext& context) {
    if (context.method != "tools/call") {
        return {};
    }
    if (context.params.value("name", "") == "write_file") {
        return {"files:read", "files:write"};
    }
    return {"files:read"};
}

/// A validator that accepts the token `<scopes separated by commas>` as holding those scopes.
mcp::TokenValidation scopes_from_token(std::string_view token) {
    mcp::TokenValidation validation(true);
    std::vector<std::string> scopes;
    std::string text(token);
    for (std::size_t start = 0; start < text.size();) {
        const auto end = text.find(',', start);
        scopes.push_back(text.substr(start, end == std::string::npos ? end : end - start));
        start = end == std::string::npos ? text.size() : end + 1;
    }
    validation.scopes = std::move(scopes);
    return validation;
}

std::unique_ptr<mcp::Server> make_server(std::shared_ptr<std::atomic<int>> handled) {
    mcp::ServerCapabilities caps;
    caps.tools = mcp::ServerCapabilities::ToolsCapability{};
    auto server = std::make_unique<mcp::Server>(mcp::Implementation{"scopes", "1.0"}, caps);
    for (const char* name : {"read_file", "write_file"}) {
        server->add_tool<json, json>(name, "A file tool", json{{"type", "object"}},
                                     [handled](mcp::Context&, json) -> mcp::Task<json> {
                                         handled->fetch_add(1);
                                         co_return json{{"done", true}};
                                     });
    }
    return server;
}

Headers modern_headers(const std::string& token, std::string_view method, std::string_view name) {
    Headers headers{{"MCP-Protocol-Version", "2026-07-28"},
                    {"Mcp-Method", std::string(method)},
                    {"Authorization", "Bearer " + token}};
    if (!name.empty()) {
        headers.emplace_back("Mcp-Name", std::string(name));
    }
    return headers;
}

std::string call_tool(const std::string& name) {
    return make_modern_request(1, "tools/call", {{"name", name}, {"arguments", json::object()}}).dump();
}

/// What a client sending each request got back.
struct Replies {
    Reply read_with_read;
    Reply write_with_read;
    Reply write_with_both;
    Reply discover_with_unrelated;
    Reply write_with_no_scopes_reported;
    Reply legacy_initialize;
};

mcp::Task<Replies> exchange(asio::any_io_executor executor, unsigned short port) {
    Replies replies;
    replies.read_with_read = co_await post(executor, port, call_tool("read_file"),
                                           modern_headers("files:read", "tools/call", "read_file"));
    replies.write_with_read = co_await post(executor, port, call_tool("write_file"),
                                            modern_headers("files:read", "tools/call", "write_file"));
    replies.write_with_both =
        co_await post(executor, port, call_tool("write_file"),
                      modern_headers("files:read,files:write", "tools/call", "write_file"));
    replies.discover_with_unrelated = co_await post(executor, port, make_discover_request(2).dump(),
                                                    modern_headers("unrelated", "server/discover", ""));
    replies.write_with_no_scopes_reported = co_await post(
        executor, port, call_tool("write_file"), modern_headers("-", "tools/call", "write_file"));
    co_return replies;
}

void expect_scope_behaviour(const Replies& replies, int handled_calls,
                            const std::string& metadata_url) {
    EXPECT_EQ(replies.read_with_read.status, 200u) << replies.read_with_read.body;
    EXPECT_EQ(replies.write_with_both.status, 200u) << replies.write_with_both.body;
    EXPECT_EQ(replies.discover_with_unrelated.status, 200u) << replies.discover_with_unrelated.body;
    // Only the two calls the token covers reached a handler.
    EXPECT_EQ(handled_calls, 2);

    // A token short of a scope is refused with every scope the operation needs, in one challenge.
    EXPECT_EQ(replies.write_with_read.status, 403u) << replies.write_with_read.body;
    EXPECT_EQ(replies.write_with_read.www_authenticate,
              R"(Bearer realm="files", error="insufficient_scope", scope="files:read files:write", )"
              R"(resource_metadata=")" +
                  metadata_url + R"(")");
    const auto challenges = mcp::auth::parse_www_authenticate(replies.write_with_read.www_authenticate);
    ASSERT_EQ(challenges.size(), 1U);
    EXPECT_EQ(challenges[0].error, "insufficient_scope");
    EXPECT_EQ(challenges[0].scope, std::optional<std::string>("files:read files:write"));
    EXPECT_EQ(challenges[0].resource_metadata, std::optional<std::string>(metadata_url));

    // A validator that reports no scopes grants none.
    EXPECT_EQ(replies.write_with_no_scopes_reported.status, 403u);
}

mcp::ProtectedResourceMetadataConfig files_metadata(unsigned short port) {
    mcp::ProtectedResourceMetadataConfig metadata;
    metadata.resource = "http://127.0.0.1:" + std::to_string(port) + "/mcp";
    metadata.authorization_servers = {"http://127.0.0.1:9000"};
    return metadata;
}

template <typename Host>
void configure_scopes(Host& host, unsigned short port) {
    host.set_bearer_token_validator([](std::string_view token) -> mcp::TokenValidation {
        if (token == "-") {
            return true;
        }
        return scopes_from_token(token);
    });
    host.set_scope_requirements(files_scopes);
    mcp::BearerChallengeConfig challenge;
    challenge.realm = "files";
    host.set_bearer_challenge(challenge);
    host.set_protected_resource_metadata(files_metadata(port));
}

std::string metadata_url_for(unsigned short port) {
    return "http://127.0.0.1:" + std::to_string(port) + "/.well-known/oauth-protected-resource/mcp";
}

}  // namespace

TEST(HttpInsufficientScope, HttpServerTransportAnswers403BeforeDispatch) {
    asio::io_context io;
    auto transport = std::make_shared<mcp::HttpServerTransport>(io.get_executor(), "127.0.0.1", 0);
    const auto port = transport->port();
    configure_scopes(*transport, port);

    auto handled = std::make_shared<std::atomic<int>>(0);
    auto server = make_server(handled);
    Replies replies;
    asio::co_spawn(io, transport->listen(), asio::detached);
    asio::co_spawn(io, server->run(transport, io.get_executor()), asio::detached);
    asio::co_spawn(
        io,
        [&]() -> mcp::Task<void> {
            replies = co_await exchange(io.get_executor(), port);
            transport->close();
        },
        asio::detached);
    io.run();

    expect_scope_behaviour(replies, handled->load(), metadata_url_for(port));
}

TEST(HttpInsufficientScope, SessionManagerAnswers403BeforeDispatch) {
    constexpr unsigned short port = 19871;
    asio::io_context io;
    auto handled = std::make_shared<std::atomic<int>>(0);
    mcp::StreamableHttpSessionManager manager(
        io.get_executor(), "127.0.0.1", port,
        [handled](const asio::any_io_executor&) { return make_server(handled); });
    configure_scopes(manager, port);

    Replies replies;
    asio::co_spawn(io, manager.listen(), asio::detached);
    asio::co_spawn(
        io,
        [&]() -> mcp::Task<void> {
            replies = co_await exchange(io.get_executor(), port);
            manager.close();
        },
        asio::detached);
    io.run();

    expect_scope_behaviour(replies, handled->load(), metadata_url_for(port));
}

namespace {

std::string legacy_initialize() {
    return json{{"jsonrpc", "2.0"},
                {"id", 1},
                {"method", "initialize"},
                {"params",
                 {{"protocolVersion", "2025-11-25"},
                  {"capabilities", json::object()},
                  {"clientInfo", {{"name", "t"}, {"version", "1"}}}}}}
        .dump();
}

Headers legacy_headers(const std::string& token) {
    return {{"MCP-Protocol-Version", "2025-11-25"}, {"Authorization", "Bearer " + token}};
}

/// A requirement that every JSON-RPC request needs `files:read`.
std::vector<std::string> everything_needs_read(const mcp::ScopeRequestContext&) {
    return {"files:read"};
}

}  // namespace

TEST(HttpInsufficientScope, LegacyRequestsAreCheckedAndOpenNoSession) {
    constexpr unsigned short port = 19872;
    asio::io_context io;
    auto handled = std::make_shared<std::atomic<int>>(0);
    mcp::StreamableHttpSessionManager manager(
        io.get_executor(), "127.0.0.1", port,
        [handled](const asio::any_io_executor&) { return make_server(handled); });
    manager.set_bearer_token_validator(
        [](std::string_view token) -> mcp::TokenValidation { return scopes_from_token(token); });
    manager.set_scope_requirements(everything_needs_read);

    Reply refused;
    Reply allowed;
    asio::co_spawn(io, manager.listen(), asio::detached);
    asio::co_spawn(
        io,
        [&]() -> mcp::Task<void> {
            refused =
                co_await post(io.get_executor(), port, legacy_initialize(), legacy_headers("other"));
            allowed = co_await post(io.get_executor(), port, legacy_initialize(),
                                    legacy_headers("files:read"));
            manager.close();
        },
        asio::detached);
    io.run();

    EXPECT_EQ(refused.status, 403u) << refused.body;
    EXPECT_EQ(refused.www_authenticate, R"(Bearer error="insufficient_scope", scope="files:read")");
    EXPECT_EQ(allowed.status, 200u) << allowed.body;
}

TEST(HttpInsufficientScope, HttpServerTransportChecksLegacyRequests) {
    asio::io_context io;
    auto transport = std::make_shared<mcp::HttpServerTransport>(io.get_executor(), "127.0.0.1", 0);
    const auto port = transport->port();
    transport->set_bearer_token_validator(
        [](std::string_view token) -> mcp::TokenValidation { return scopes_from_token(token); });
    transport->set_scope_requirements(everything_needs_read);

    auto handled = std::make_shared<std::atomic<int>>(0);
    auto server = make_server(handled);
    Reply refused;
    Reply allowed;
    asio::co_spawn(io, transport->listen(), asio::detached);
    asio::co_spawn(io, server->run(transport, io.get_executor()), asio::detached);
    asio::co_spawn(
        io,
        [&]() -> mcp::Task<void> {
            refused =
                co_await post(io.get_executor(), port, legacy_initialize(), legacy_headers("other"));
            allowed = co_await post(io.get_executor(), port, legacy_initialize(),
                                    legacy_headers("files:read"));
            transport->close();
        },
        asio::detached);
    io.run();

    EXPECT_EQ(refused.status, 403u) << refused.body;
    EXPECT_EQ(refused.www_authenticate, R"(Bearer error="insufficient_scope", scope="files:read")");
    EXPECT_EQ(allowed.status, 200u) << allowed.body;
}

TEST(HttpInsufficientScope, AProviderThatThrowsIsAnswered500WithoutAChallenge) {
    constexpr unsigned short port = 19873;
    asio::io_context io;
    auto handled = std::make_shared<std::atomic<int>>(0);
    mcp::StreamableHttpSessionManager manager(
        io.get_executor(), "127.0.0.1", port,
        [handled](const asio::any_io_executor&) { return make_server(handled); });
    manager.set_bearer_token_validator(
        [](std::string_view token) -> mcp::TokenValidation { return scopes_from_token(token); });
    manager.set_scope_requirements([](const mcp::ScopeRequestContext&) -> std::vector<std::string> {
        throw std::runtime_error("policy unavailable");
    });

    Reply reply;
    asio::co_spawn(io, manager.listen(), asio::detached);
    asio::co_spawn(
        io,
        [&]() -> mcp::Task<void> {
            reply = co_await post(io.get_executor(), port, call_tool("read_file"),
                                  modern_headers("files:read", "tools/call", "read_file"));
            manager.close();
        },
        asio::detached);
    io.run();

    EXPECT_EQ(reply.status, 500u) << reply.body;
    EXPECT_TRUE(reply.www_authenticate.empty());
    EXPECT_EQ(handled->load(), 0);
}

TEST(HttpInsufficientScope, ConfigurationIsRefusedOnceListening) {
    asio::io_context io;
    mcp::HttpServerTransport transport(io.get_executor(), "127.0.0.1", 0);
    transport.close();
    EXPECT_THROW(transport.set_scope_requirements(files_scopes), std::logic_error);
}

namespace {

/// A provider that names a scope containing a byte a header cannot carry.
std::vector<std::string> unquotable_scope(const mcp::ScopeRequestContext&) {
    return {std::string("files:\x01read")};
}

}  // namespace

TEST(HttpInsufficientScope, ASessionManagerScopeThatCannotBeQuotedIsAnswered500) {
    constexpr unsigned short port = 19874;
    asio::io_context io;
    auto handled = std::make_shared<std::atomic<int>>(0);
    mcp::StreamableHttpSessionManager manager(
        io.get_executor(), "127.0.0.1", port,
        [handled](const asio::any_io_executor&) { return make_server(handled); });
    manager.set_bearer_token_validator(
        [](std::string_view token) -> mcp::TokenValidation { return scopes_from_token(token); });
    manager.set_scope_requirements(unquotable_scope);

    Reply reply;
    asio::co_spawn(io, manager.listen(), asio::detached);
    asio::co_spawn(
        io,
        [&]() -> mcp::Task<void> {
            reply = co_await post(io.get_executor(), port, call_tool("read_file"),
                                  modern_headers("other", "tools/call", "read_file"));
            manager.close();
        },
        asio::detached);
    io.run();

    EXPECT_EQ(reply.status, 500u) << reply.body;
    EXPECT_TRUE(reply.www_authenticate.empty());
    EXPECT_EQ(handled->load(), 0);
}

TEST(HttpInsufficientScope, AnHttpServerTransportScopeThatCannotBeQuotedIsAnswered500) {
    asio::io_context io;
    auto transport = std::make_shared<mcp::HttpServerTransport>(io.get_executor(), "127.0.0.1", 0);
    const auto port = transport->port();
    transport->set_bearer_token_validator(
        [](std::string_view token) -> mcp::TokenValidation { return scopes_from_token(token); });
    transport->set_scope_requirements(unquotable_scope);

    auto handled = std::make_shared<std::atomic<int>>(0);
    auto server = make_server(handled);
    Reply reply;
    asio::co_spawn(io, transport->listen(), asio::detached);
    asio::co_spawn(io, server->run(transport, io.get_executor()), asio::detached);
    asio::co_spawn(
        io,
        [&]() -> mcp::Task<void> {
            reply = co_await post(io.get_executor(), port, call_tool("read_file"),
                                  modern_headers("other", "tools/call", "read_file"));
            transport->close();
        },
        asio::detached);
    io.run();

    EXPECT_EQ(reply.status, 500u) << reply.body;
    EXPECT_TRUE(reply.www_authenticate.empty());
    EXPECT_EQ(handled->load(), 0);
}

TEST(HttpInsufficientScope, ScopeRequirementsWithoutAValidatorAreRefusedWhenListening) {
    asio::io_context io;
    auto validator = [](std::string_view token) -> mcp::TokenValidation {
        return scopes_from_token(token);
    };
    {
        mcp::HttpServerTransport transport(io.get_executor(), "127.0.0.1", 0);
        transport.set_scope_requirements(files_scopes);
        EXPECT_THROW((void)transport.listen(), std::logic_error);
        // The refusal does not depend on setter order.
        transport.set_bearer_token_validator(validator);
        EXPECT_NO_THROW((void)transport.listen());
        transport.close();
    }
    {
        mcp::StreamableHttpSessionManager manager(
            io.get_executor(), "127.0.0.1", 0, [](const asio::any_io_executor&) {
                return make_server(std::make_shared<std::atomic<int>>(0));
            });
        manager.set_scope_requirements(files_scopes);
        EXPECT_THROW((void)manager.listen(), std::logic_error);
        manager.set_bearer_token_validator(validator);
        EXPECT_NO_THROW((void)manager.listen());
        manager.close();
    }
}

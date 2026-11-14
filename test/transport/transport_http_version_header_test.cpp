#include "../support/hang_guard.hpp"
#include "../test_utils.hpp"

#include "mcp/server/server.hpp"
#include "mcp/transport/http_server.hpp"
#include "mcp/transport/http_session_manager.hpp"

#include <gtest/gtest.h>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
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
    std::optional<std::string> session_id;
};

mcp::Task<Reply> send(asio::any_io_executor executor, unsigned short port, http::verb verb,
                      std::string body, Headers headers) {
    beast::tcp_stream stream(executor);
    // A bound for reporting a server that never answers, not a measure of speed.
    stream.expires_after(g_hang_guard);
    co_await stream.async_connect(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), port),
                                  asio::use_awaitable);
    http::request<http::string_body> request{verb, "/mcp", 11};
    request.set(http::field::host, "127.0.0.1");
    request.set(http::field::content_type, "application/json");
    request.set(http::field::accept, "application/json, text/event-stream");
    for (const auto& [name, value] : headers) {
        request.insert(name, value);
    }
    request.keep_alive(false);
    request.body() = std::move(body);
    request.prepare_payload();
    co_await http::async_write(stream, request, asio::use_awaitable);
    beast::flat_buffer buffer;
    http::response<http::string_body> response;
    co_await http::async_read(stream, buffer, response, asio::use_awaitable);
    Reply reply;
    reply.status = response.result_int();
    if (auto session = response.find("Mcp-Session-Id"); session != response.end()) {
        reply.session_id = std::string(session->value());
    }
    co_return reply;
}

std::unique_ptr<mcp::Server> make_server() {
    return std::make_unique<mcp::Server>(mcp::Implementation{"versions", "1.0"},
                                         mcp::ServerCapabilities{});
}

unsigned short free_port() {
    asio::io_context io;
    asio::ip::tcp::acceptor acceptor(io,
                                     asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    return acceptor.local_endpoint().port();
}

json ping(int id) { return {{"jsonrpc", "2.0"}, {"id", id}, {"method", "ping"}}; }

/// What a session answers to requests whose MCP-Protocol-Version differs from the one it
/// negotiated.
struct Answers {
    unsigned int other_legacy_version{0};
    unsigned int unknown_version{0};
    unsigned int delete_with_other_legacy_version{0};
};

/// Starts a session that negotiates the latest legacy version, then sends a ping naming 2025-03-26,
/// a ping naming a version no revision has, and a DELETE naming 2025-03-26.
mcp::Task<Answers> exchange(asio::any_io_executor executor, unsigned short port) {
    const auto initialized =
        co_await send(executor, port, http::verb::post, make_initialize_request("i").dump(), {});
    const auto session_id = initialized.session_id.value_or("");
    EXPECT_FALSE(session_id.empty());
    const Headers negotiated{
        {"Mcp-Session-Id", session_id},
        {"MCP-Protocol-Version", std::string(mcp::g_LATEST_LEGACY_PROTOCOL_VERSION)}};
    co_await send(executor, port, http::verb::post, make_initialized_notification().dump(), negotiated);

    const Headers older{{"Mcp-Session-Id", session_id}, {"MCP-Protocol-Version", "2025-03-26"}};
    const Headers unknown{{"Mcp-Session-Id", session_id}, {"MCP-Protocol-Version", "1999-01-01"}};
    Answers answers;
    answers.other_legacy_version =
        (co_await send(executor, port, http::verb::post, ping(1).dump(), older)).status;
    answers.unknown_version =
        (co_await send(executor, port, http::verb::post, ping(2).dump(), unknown)).status;
    answers.delete_with_other_legacy_version =
        (co_await send(executor, port, http::verb::delete_, {}, older)).status;
    co_return answers;
}

}  // namespace

TEST(VersionHeader, SessionManagerAcceptsAnyLegacyVersionOnASession) {
    asio::io_context io;
    const auto port = free_port();
    mcp::StreamableHttpSessionManager manager(
        io.get_executor(), "127.0.0.1", port,
        [](const asio::any_io_executor&) { return make_server(); });
    manager.set_json_only(true);
    Answers answers;
    asio::co_spawn(io, manager.listen(), asio::detached);
    asio::co_spawn(
        io,
        [&]() -> mcp::Task<void> {
            try {
                answers = co_await exchange(io.get_executor(), port);
            } catch (const std::exception& error) {
                ADD_FAILURE() << error.what();
            }
            manager.close();
        },
        asio::detached);
    io.run();

    EXPECT_EQ(answers.other_legacy_version, 200U);
    EXPECT_EQ(answers.unknown_version, 400U);
    EXPECT_EQ(answers.delete_with_other_legacy_version, 200U);
}

TEST(VersionHeader, HttpServerTransportAcceptsAnyLegacyVersionOnASession) {
    asio::io_context io;
    auto transport = std::make_shared<mcp::HttpServerTransport>(io.get_executor(), "127.0.0.1", 0);
    transport->set_json_only(true);
    auto server = make_server();
    Answers answers;
    asio::co_spawn(io, transport->listen(), asio::detached);
    asio::co_spawn(
        io, [&]() -> mcp::Task<void> { co_await server->run(transport, io.get_executor()); },
        asio::detached);
    asio::co_spawn(
        io,
        [&]() -> mcp::Task<void> {
            try {
                answers = co_await exchange(io.get_executor(), transport->port());
            } catch (const std::exception& error) {
                ADD_FAILURE() << error.what();
            }
            transport->close();
        },
        asio::detached);
    io.run();

    EXPECT_EQ(answers.other_legacy_version, 200U);
    EXPECT_EQ(answers.unknown_version, 400U);
    EXPECT_EQ(answers.delete_with_other_legacy_version, 200U);
}

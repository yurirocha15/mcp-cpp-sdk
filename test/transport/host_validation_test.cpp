// The Host check of the HTTP and WebSocket servers. The policy is internal, so the test includes its
// header; the test program builds src/transport/proxy.cpp, which holds the loopback rule.
#include "../../src/detail/host_policy.hpp"

#include "mcp/core/async.hpp"
#include "mcp/transport/http_server.hpp"
#include "mcp/transport/http_session_manager.hpp"
#include "mcp/transport/websocket.hpp"

#include <gtest/gtest.h>

#include "../support/hang_guard.hpp"
#include "../support/mcp_server_hosts.hpp"
#include "../support/spawn.hpp"
#include "../support/tls_test_support.hpp"
#include "../test_utils.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace host_validation_test {

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using mcp::detail::HostPolicy;
using mcp::detail::normalized_host;
using mcp_test::ManagerHost;
using mcp_test::TransportHost;
using tcp = asio::ip::tcp;

tcp::endpoint endpoint_of(const std::string& address) {
    return {asio::ip::make_address(address), 8080};
}

/// The status of an initialize POST to the server on @p port, reached through @p address, with
/// @p host as its Host header (none when null).
unsigned status_for(unsigned short port, const std::optional<std::string>& host,
                    const std::string& address = "127.0.0.1") {
    asio::io_context io;
    beast::tcp_stream stream(io);
    stream.connect(tcp::endpoint(asio::ip::make_address(address), port));
    http::request<http::string_body> request{http::verb::post, "/mcp", 11};
    if (host) {
        request.set(http::field::host, *host);
    }
    request.set(http::field::content_type, "application/json");
    request.set(http::field::accept, "application/json, text/event-stream");
    request.body() = make_initialize_request("1").dump();
    request.prepare_payload();
    http::write(stream, request);
    beast::flat_buffer buffer;
    http::response<http::string_body> response;
    http::read(stream, buffer, response);
    return response.result_int();
}

constexpr unsigned g_forbidden = 403;

/// Accepts one WebSocket connection on @p acceptor and returns its first message.
mcp::Task<std::string> read_one_message(tcp::acceptor& acceptor) {
    auto socket = co_await acceptor.async_accept(asio::use_awaitable);
    mcp::WebSocketServerTransport transport(std::move(socket));
    co_return co_await transport.read_message();
}

/// As read_one_message(), with the server allowing the names @p hosts.
mcp::Task<std::string> read_one_message_allowing(tcp::acceptor& acceptor,
                                                 std::vector<std::string> hosts) {
    auto socket = co_await acceptor.async_accept(asio::use_awaitable);
    mcp::WebSocketServerTransport transport(std::move(socket));
    transport.set_allowed_hosts(hosts);
    co_return co_await transport.read_message();
}

}  // namespace

// ------------------------------------------------------------------------------------------------
// The policy

TEST(HostPolicyTest, ComparesHostsWithoutPortBracketsCaseOrTrailingDot) {
    EXPECT_EQ(normalized_host("Example.COM:8443"), "example.com");
    EXPECT_EQ(normalized_host("localhost."), "localhost");
    EXPECT_EQ(normalized_host("[::1]:8080"), "::1");
    EXPECT_EQ(normalized_host("[::FFFF:127.0.0.1]"), "::ffff:127.0.0.1");
    EXPECT_EQ(normalized_host("127.0.0.1"), "127.0.0.1");
    EXPECT_EQ(normalized_host("[::1"), "");
    EXPECT_EQ(normalized_host("[::1]x"), "");
    EXPECT_EQ(normalized_host(""), "");
}

TEST(HostPolicyTest, ALoopbackConnectionMustNameALoopbackHost) {
    const HostPolicy policy;
    for (const char* host : {"localhost", "LOCALHOST:3000", "localhost.", "127.0.0.1:8080", "127.9.9.9",
                             "[::1]:8080", "[::ffff:127.0.0.1]"}) {
        EXPECT_TRUE(policy.allows(std::string_view(host), true)) << host;
    }
    for (const char* host :
         {"example.com", "evil.localhost.example", "0.0.0.0", "[::]", "10.0.0.1", "", "[::1"}) {
        EXPECT_FALSE(policy.allows(std::string_view(host), true)) << host;
    }
    EXPECT_FALSE(policy.allows(std::nullopt, true));
    // Other connections are not checked without a list.
    EXPECT_TRUE(policy.allows(std::string_view("example.com"), false));
    EXPECT_TRUE(policy.allows(std::nullopt, false));
}

TEST(HostPolicyTest, AListIsEnforcedOnEveryConnection) {
    const HostPolicy policy({"Example.com", "[::1]:9000", "api.example.com."});
    EXPECT_TRUE(policy.allows(std::string_view("example.COM:443"), true));
    EXPECT_TRUE(policy.allows(std::string_view("example.com"), false));
    EXPECT_TRUE(policy.allows(std::string_view("[::1]"), true));
    EXPECT_TRUE(policy.allows(std::string_view("api.example.com"), false));
    EXPECT_FALSE(policy.allows(std::string_view("localhost"), true));
    EXPECT_FALSE(policy.allows(std::string_view("other.example.com"), false));
    EXPECT_FALSE(policy.allows(std::nullopt, false));
    EXPECT_THROW(HostPolicy({"example.com", ""}), std::invalid_argument);
    EXPECT_THROW(HostPolicy({"[::1"}), std::invalid_argument);
}

TEST(HostPolicyTest, LoopbackAddressesIncludeMappedIpv4) {
    EXPECT_TRUE(mcp::detail::is_loopback_endpoint(endpoint_of("127.0.0.1")));
    EXPECT_TRUE(mcp::detail::is_loopback_endpoint(endpoint_of("127.200.0.1")));
    EXPECT_TRUE(mcp::detail::is_loopback_endpoint(endpoint_of("::1")));
    EXPECT_TRUE(mcp::detail::is_loopback_endpoint(endpoint_of("::ffff:127.0.0.1")));
    EXPECT_FALSE(mcp::detail::is_loopback_endpoint(endpoint_of("10.1.2.3")));
    EXPECT_FALSE(mcp::detail::is_loopback_endpoint(endpoint_of("::ffff:10.1.2.3")));
    EXPECT_FALSE(mcp::detail::is_loopback_endpoint(endpoint_of("0.0.0.0")));
}

// ------------------------------------------------------------------------------------------------
// The servers

TEST(HostValidationTest, TheSessionManagerRefusesAForeignHostOnLoopback) {
    const ManagerHost host(nullptr);
    const auto port = std::to_string(host.port());
    EXPECT_EQ(status_for(host.port(), "evil.example:" + port), g_forbidden);
    EXPECT_EQ(status_for(host.port(), "0.0.0.0:" + port), g_forbidden);
    EXPECT_EQ(status_for(host.port(), std::nullopt), g_forbidden);
    EXPECT_NE(status_for(host.port(), "localhost:" + port), g_forbidden);
    EXPECT_NE(status_for(host.port(), "127.0.0.1:" + port), g_forbidden);
}

TEST(HostValidationTest, TheHttpServerTransportRefusesAForeignHostOnLoopback) {
    const TransportHost host(nullptr);
    const auto port = std::to_string(host.port());
    EXPECT_EQ(status_for(host.port(), "evil.example:" + port), g_forbidden);
    EXPECT_EQ(status_for(host.port(), std::nullopt), g_forbidden);
    EXPECT_NE(status_for(host.port(), "localhost:" + port), g_forbidden);
}

TEST(HostValidationTest, AListedNameIsServedAndOthersAreNot) {
    const ManagerHost manager([](mcp::StreamableHttpSessionManager& configured) {
        configured.set_allowed_hosts({"mcp.example.com"});
    });
    EXPECT_NE(status_for(manager.port(), "mcp.example.com"), g_forbidden);
    EXPECT_EQ(status_for(manager.port(), "localhost"), g_forbidden);

    const TransportHost transport([](mcp::HttpServerTransport& configured) {
        configured.set_allowed_hosts({"mcp.example.com", "localhost"});
    });
    EXPECT_NE(status_for(transport.port(), "MCP.example.com:443"), g_forbidden);
    EXPECT_NE(status_for(transport.port(), "localhost"), g_forbidden);
    EXPECT_EQ(status_for(transport.port(), "other.example.com"), g_forbidden);
}

TEST(HostValidationTest, AServerOnAllAddressesIsCheckedWhenReachedThroughLoopback) {
    const ManagerHost host(nullptr, "0.0.0.0");
    EXPECT_EQ(status_for(host.port(), "evil.example"), g_forbidden);
    EXPECT_NE(status_for(host.port(), "localhost"), g_forbidden);
}

TEST(HostValidationTest, TheWebSocketServerRefusesAForeignHostOnLoopback) {
    mcp_test::tls::ServerThread thread;
    tcp::acceptor acceptor(thread.io(), tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const auto port = acceptor.local_endpoint().port();
    auto served = mcp_test::spawn_future(thread.io(), read_one_message(acceptor));
    thread.start();

    asio::io_context io;
    beast::tcp_stream stream(io);
    stream.connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));
    http::request<http::empty_body> upgrade{http::verb::get, "/", 11};
    upgrade.set(http::field::host, "evil.example:" + std::to_string(port));
    upgrade.set(http::field::upgrade, "websocket");
    upgrade.set(http::field::connection, "upgrade");
    upgrade.set(http::field::sec_websocket_key, "dGhlIHNhbXBsZSBub25jZQ==");
    upgrade.set(http::field::sec_websocket_version, "13");
    http::write(stream, upgrade);
    beast::flat_buffer buffer;
    http::response<http::string_body> response;
    http::read(stream, buffer, response);
    EXPECT_EQ(response.result_int(), g_forbidden);
    EXPECT_EQ(response.body(), "Host not allowed");

    ASSERT_EQ(served.wait_for(g_hang_guard), std::future_status::ready);
    EXPECT_THROW(served.get(), std::runtime_error);
    thread.stop();
}

TEST(HostValidationTest, AWebSocketClientNamesItsServerAsTheServerExpects) {
    mcp_test::tls::ServerThread thread;
    tcp::acceptor acceptor(thread.io(), tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const auto port = acceptor.local_endpoint().port();
    auto served = mcp_test::spawn_future(thread.io(), read_one_message(acceptor));
    thread.start();

    asio::io_context io;
    mcp::WebSocketClientTransport client(io.get_executor(), "localhost", std::to_string(port));
    auto written = mcp_test::spawn_future(io, client.write_message("hello"));
    io.run_for(g_hang_guard);
    EXPECT_NO_THROW(written.get());
    ASSERT_EQ(served.wait_for(g_hang_guard), std::future_status::ready);
    EXPECT_EQ(served.get(), "hello");
    client.close();
    thread.stop();
}

TEST(HostValidationTest, AWebSocketServerServesTheNamesItIsGiven) {
    mcp_test::tls::ServerThread thread;
    tcp::acceptor acceptor(thread.io(), tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const auto port = acceptor.local_endpoint().port();
    const std::vector<std::string> hosts{"mcp.example.com"};
    auto served = mcp_test::spawn_future(thread.io(), read_one_message_allowing(acceptor, hosts));
    thread.start();

    asio::io_context io;
    beast::websocket::stream<beast::tcp_stream> ws(io);
    beast::get_lowest_layer(ws).connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));
    ws.handshake("mcp.example.com:" + std::to_string(port), "/");
    ws.write(asio::buffer(std::string("hello")));
    ASSERT_EQ(served.wait_for(g_hang_guard), std::future_status::ready);
    EXPECT_EQ(served.get(), "hello");
    beast::error_code ignored;
    beast::get_lowest_layer(ws).socket().close(ignored);
    thread.stop();
}

}  // namespace host_validation_test

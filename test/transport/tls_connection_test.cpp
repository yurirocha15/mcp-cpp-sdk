// The idle check of a kept connection, against a TLS server that sends a session ticket after its
// response and then a close_notify. The check is internal, so the test includes its header.
#include "../../src/detail/connection.hpp"

#include <gtest/gtest.h>

#include "../support/tls_test_support.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <chrono>
#include <future>
#include <string>
#include <thread>

namespace tls_connection_test {

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using mcp_test::tls::CertSpec;
using mcp_test::tls::issue;
using tcp = asio::ip::tcp;

/// Waits until @p socket has bytes to read; false after two seconds.
bool wait_readable(tcp::socket& socket) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        boost::system::error_code error;
        if (socket.available(error) > 0 || error) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

}  // namespace

TEST(TlsConnectionTest, TheIdleCheckDropsLateTicketsAndSeesCloseNotify) {
    const auto ca = issue(CertSpec{.common_name = "Idle CA", .authority = true});
    const auto leaf = issue(CertSpec{.common_name = "idle", .alt_names = {"DNS:localhost"}}, &ca);

    asio::io_context server_io;
    tcp::acceptor acceptor(server_io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    std::promise<void> send_close_notify;
    auto close_notify_requested = send_close_notify.get_future();
    std::thread server([&] {
        asio::ssl::context context(asio::ssl::context::tls_server);
        context.use_certificate_chain(asio::buffer(leaf.cert_pem));
        context.use_private_key(asio::buffer(leaf.key_pem), asio::ssl::context::pem);
        asio::ssl::stream<tcp::socket> stream(acceptor.accept(), context);
        stream.handshake(asio::ssl::stream_base::server);
        beast::flat_buffer buffer;
        http::request<http::string_body> request;
        http::read(stream, buffer, request);
        http::response<http::string_body> response{http::status::ok, 11};
        response.body() = "ok";
        response.prepare_payload();
        http::write(stream, response);
        // A ticket after the response, which reaches the client while it is idle.
        if (SSL_new_session_ticket(stream.native_handle()) == 1) {
            stream.handshake(asio::ssl::stream_base::server);
        }
        close_notify_requested.wait();
        boost::system::error_code ignored;
        stream.shutdown(ignored);
    });

    asio::io_context io;
    asio::ssl::context context(asio::ssl::context::tls_client);
    // As the SDK's client contexts are set up.
    SSL_CTX_set_options(context.native_handle(), SSL_OP_NO_TICKET);
    SSL_CTX_set_session_cache_mode(context.native_handle(), SSL_SESS_CACHE_OFF);
    context.add_certificate_authority(asio::buffer(ca.cert_pem));
    context.set_verify_mode(asio::ssl::verify_peer);
    mcp::detail::Connection connection(io.get_executor(), context);
    connection.socket().connect(acceptor.local_endpoint());
    connection.tls()->handshake(asio::ssl::stream_base::client);
    connection.mark_established();

    http::request<http::string_body> request{http::verb::get, "/", 11};
    request.set(http::field::host, "localhost");
    http::write(*connection.tls(), request);
    beast::flat_buffer buffer;
    http::response<http::string_body> response;
    http::read(*connection.tls(), buffer, response);
    EXPECT_EQ(response.body(), "ok");

    // The ticket is waiting on the socket, and the connection is still good for a request.
    ASSERT_TRUE(wait_readable(connection.socket()));
    EXPECT_FALSE(mcp::detail::closed_by_peer(connection));
    EXPECT_FALSE(mcp::detail::closed_by_peer(connection));

    send_close_notify.set_value();
    ASSERT_TRUE(wait_readable(connection.socket()));
    EXPECT_TRUE(mcp::detail::closed_by_peer(connection));

    mcp::detail::close_connection(connection);
    EXPECT_TRUE(mcp::detail::closed_by_peer(connection));
    server.join();
}

}  // namespace tls_connection_test

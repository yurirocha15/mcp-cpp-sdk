#include "mcp/core/async.hpp"
#include "mcp/transport/tls.hpp"
#include "mcp/transport/websocket.hpp"

#include <gtest/gtest.h>

#include "support/hang_guard.hpp"
#include "support/network_error.hpp"
#include "support/spawn.hpp"
#include "support/tls_test_support.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace transport_ws_tls_test {

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
using mcp::TlsError;
using mcp_test::tls::CertSpec;
using mcp_test::tls::issue;
using mcp_test::tls::Issued;
using tcp = asio::ip::tcp;

struct Certs {
    Issued ca = issue(CertSpec{.common_name = "WebSocket Test CA", .authority = true});
    Issued server = issue(
        CertSpec{.common_name = "ws server", .alt_names = {"DNS:localhost", "IP:127.0.0.1"}}, &ca);
    Issued client = issue(CertSpec{.common_name = "ws client", .client = true}, &ca);
};

const Certs& certs() {
    static const Certs value;
    return value;
}

std::shared_ptr<const mcp::TlsServerContext> server_context(bool require_client = false) {
    mcp::TlsServerOptions options;
    options.certificate_chain_pem = certs().server.cert_pem;
    options.private_key_pem = certs().server.key_pem;
    if (require_client) {
        options.client_auth = mcp::TlsServerOptions::ClientAuth::eRequired;
        options.client_ca_pem = {certs().ca.cert_pem};
    }
    return mcp::TlsServerContext::create(options);
}

mcp::TlsClientOptions trusting_ca() {
    mcp::TlsClientOptions options;
    options.use_system_roots = false;
    options.ca_pem = {certs().ca.cert_pem};
    return options;
}

/// A wss server for one connection on a thread of its own: it answers each message with "echo:"
/// and the message, and records how the connection ended.
class EchoServer {
   public:
    explicit EchoServer(std::shared_ptr<const mcp::TlsServerContext> context)
        : acceptor_(thread_.io(), tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0)) {
        ended_ = mcp_test::spawn_future(
            thread_.io(), [this, context = std::move(context)]() mutable -> mcp::Task<void> {
                auto socket = co_await acceptor_.async_accept(asio::use_awaitable);
                transport_ = std::make_shared<mcp::WebSocketServerTransport>(std::move(socket),
                                                                             std::move(context));
                for (;;) {
                    auto message = co_await transport_->read_message();
                    co_await transport_->write_message("echo:" + message);
                }
            });
        thread_.on_stop([this] {
            boost::system::error_code ignored;
            acceptor_.close(ignored);
            if (transport_) {
                transport_->close();
            }
        });
        thread_.start();
    }

    EchoServer(const EchoServer&) = delete;
    EchoServer& operator=(const EchoServer&) = delete;
    ~EchoServer() { thread_.stop(); }

    [[nodiscard]] unsigned short port() const { return acceptor_.local_endpoint().port(); }

    /// The exception that ended the connection, once it has ended.
    std::exception_ptr ending() {
        if (ended_.wait_for(g_hang_guard) != std::future_status::ready) {
            ADD_FAILURE() << "the server connection did not end";
            return nullptr;
        }
        try {
            ended_.get();
        } catch (...) {
            return std::current_exception();
        }
        return nullptr;
    }

   private:
    mcp_test::tls::ServerThread thread_;
    tcp::acceptor acceptor_;
    std::shared_ptr<mcp::WebSocketServerTransport> transport_;
    std::future<void> ended_;
};

mcp::Task<std::string> write_then_read(mcp::WebSocketClientTransport& client, std::string message) {
    co_await client.write_message(message);
    co_return co_await client.read_message();
}

/// Sends @p message through @p client and returns the answer.
std::string exchange(asio::io_context& io, mcp::WebSocketClientTransport& client,
                     const std::string& message) {
    auto answer = mcp_test::spawn_future(io, write_then_read(client, message));
    io.restart();
    io.run_for(g_hang_guard);
    return answer.get();
}

/// The reason of the TlsError that @p action throws, if it throws one.
template <typename Action>
std::optional<TlsError::Reason> tls_failure(Action action) {
    try {
        action();
    } catch (const TlsError& error) {
        return error.reason();
    } catch (...) {
    }
    return std::nullopt;
}

}  // namespace

TEST(WebSocketTlsTest, ExchangesMessagesOverWss) {
    EchoServer server(server_context());
    asio::io_context io;
    mcp::WebSocketClientTransport client(io.get_executor(), "localhost", std::to_string(server.port()));
    client.set_tls(trusting_ca());
    EXPECT_EQ(exchange(io, client, "hello"), "echo:hello");
    EXPECT_EQ(exchange(io, client, "again"), "echo:again");
    client.close();
}

TEST(WebSocketTlsTest, TheClientChecksTheServerCertificate) {
    EchoServer untrusted(server_context());
    asio::io_context io;
    mcp::WebSocketClientTransport by_system(io.get_executor(), "localhost",
                                            std::to_string(untrusted.port()));
    by_system.set_tls(nullptr);
    EXPECT_EQ(tls_failure([&] { exchange(io, by_system, "hello"); }),
              TlsError::Reason::eUntrustedCertificate);

    EchoServer other(server_context());
    mcp::WebSocketClientTransport wrong_name(io.get_executor(), "127.0.0.1",
                                             std::to_string(other.port()));
    wrong_name.set_tls(trusting_ca());
    wrong_name.set_server_name("other.example.test");
    EXPECT_EQ(tls_failure([&] { exchange(io, wrong_name, "hello"); }),
              TlsError::Reason::eHostnameMismatch);
    EXPECT_THROW(wrong_name.set_server_name("late.example.test"), std::logic_error);
}

TEST(WebSocketTlsTest, AServerOverTlsRefusesAPlainClient) {
    EchoServer server(server_context());
    asio::io_context io;
    mcp::WebSocketClientTransport plain(io.get_executor(), "127.0.0.1", std::to_string(server.port()));
    EXPECT_ANY_THROW(exchange(io, plain, "hello"));
    plain.close();

    const auto ending = server.ending();
    ASSERT_TRUE(ending);
    EXPECT_EQ(tls_failure([&] { std::rethrow_exception(ending); }), TlsError::Reason::eNotTls);
}

TEST(WebSocketTlsTest, ARequiredClientCertificateIsChecked) {
    EchoServer server(server_context(true));
    asio::io_context io;
    mcp::WebSocketClientTransport client(io.get_executor(), "localhost", std::to_string(server.port()));
    auto options = trusting_ca();
    const auto directory = std::filesystem::temp_directory_path() / "mcp-ws-tls-test";
    std::filesystem::create_directories(directory);
    options.client_certificate_file = directory / "client.pem";
    options.client_key_file = directory / "client-key.pem";
    std::ofstream(options.client_certificate_file, std::ios::binary) << certs().client.cert_pem;
    std::ofstream(options.client_key_file, std::ios::binary) << certs().client.key_pem;
    client.set_tls(options);
    EXPECT_EQ(exchange(io, client, "hello"), "echo:hello");
    client.close();

    EchoServer strict(server_context(true));
    mcp::WebSocketClientTransport anonymous(io.get_executor(), "localhost",
                                            std::to_string(strict.port()));
    anonymous.set_tls(trusting_ca());
    EXPECT_ANY_THROW(exchange(io, anonymous, "hello"));
    anonymous.close();
}

// A close frame from the peer makes the server answer it and end the TLS layer with its
// close_notify, through the teardown hooks of the connection.
TEST(WebSocketTlsTest, APeerThatClosesGetsTheClosingHandshakeAndCloseNotify) {
    EchoServer server(server_context());
    auto closing = std::async(std::launch::async, [port = server.port()] {
        asio::io_context io;
        asio::ssl::context context(asio::ssl::context::tls_client);
        context.add_certificate_authority(asio::buffer(certs().ca.cert_pem));
        context.set_verify_mode(asio::ssl::verify_peer);
        beast::websocket::stream<asio::ssl::stream<tcp::socket>> ws(io, context);
        beast::get_lowest_layer(ws).connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));
        ws.next_layer().handshake(asio::ssl::stream_base::client);
        ws.handshake("localhost:" + std::to_string(port), "/");
        ws.text(true);
        ws.write(asio::buffer(std::string("hello")));
        beast::flat_buffer buffer;
        ws.read(buffer);
        const auto answer = beast::buffers_to_string(buffer.data());
        boost::system::error_code error;
        ws.close(beast::websocket::close_code::normal, error);
        return std::make_pair(answer, error);
    });
    ASSERT_EQ(closing.wait_for(g_hang_guard), std::future_status::ready);
    const auto [answer, error] = closing.get();
    EXPECT_EQ(answer, "echo:hello");
    EXPECT_FALSE(error) << error.message();

    const auto ending = server.ending();
    ASSERT_TRUE(ending);
    try {
        std::rethrow_exception(ending);
    } catch (const mcp::TransportError& closed) {
        EXPECT_TRUE(mcp_test::is_network_error(closed, mcp::TransportError::Kind::eClosed,
                                               beast::websocket::error::closed))
            << closed.what();
    } catch (const std::exception& other) {
        ADD_FAILURE() << other.what();
    }
}

TEST(WebSocketTlsTest, RefusesTlsSettingsThatCannotBeUsed) {
    asio::io_context io;
    tcp::socket socket(io);
    EXPECT_THROW(mcp::WebSocketServerTransport(std::move(socket),
                                               std::shared_ptr<const mcp::TlsServerContext>{}),
                 std::invalid_argument);
    mcp::WebSocketClientTransport client(io.get_executor(), "example.test", "443");
    EXPECT_THROW(client.set_server_name(""), std::invalid_argument);
    EXPECT_THROW(client.set_server_name("."), std::invalid_argument);
    EXPECT_NO_THROW(client.set_server_name("mcp.example.test."));
}

}  // namespace transport_ws_tls_test

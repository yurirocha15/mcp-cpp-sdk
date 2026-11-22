#include "mcp/client/client.hpp"
#include "mcp/client/endpoint.hpp"
#include "mcp/transport/http_client.hpp"
#include "mcp/transport/http_server.hpp"
#include "mcp/transport/http_session_manager.hpp"
#include "mcp/transport/tls.hpp"

#include <gtest/gtest.h>

#include "support/hang_guard.hpp"
#include "support/mcp_server_hosts.hpp"
#include "support/tls_test_support.hpp"

#include <openssl/rsa.h>
#include "support/test_utils.hpp"

#include <array>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace transport_tls_server_test {

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using mcp_test::ManagerHost;
using mcp_test::TransportHost;
using mcp_test::tls::CertSpec;
using mcp_test::tls::issue;
using mcp_test::tls::Issued;
using tcp = asio::ip::tcp;

struct Certs {
    Issued ca = issue(CertSpec{.common_name = "Server Test CA", .authority = true});
    Issued other_ca = issue(CertSpec{.common_name = "Other CA", .authority = true});
    Issued server = issue(
        CertSpec{.common_name = "test server", .alt_names = {"DNS:localhost", "IP:127.0.0.1"}}, &ca);
    Issued client = issue(CertSpec{.common_name = "client", .client = true}, &ca);
    Issued foreign_client = issue(CertSpec{.common_name = "foreign client", .client = true}, &other_ca);
};

const Certs& certs() {
    static const Certs value;
    return value;
}

mcp::TlsServerOptions server_options() {
    mcp::TlsServerOptions options;
    options.certificate_chain_pem = certs().server.cert_pem;
    options.private_key_pem = certs().server.key_pem;
    return options;
}

mcp::TlsClientOptions trusting_ca() {
    mcp::TlsClientOptions options;
    options.use_system_roots = false;
    options.ca_pem = {certs().ca.cert_pem};
    return options;
}

std::filesystem::path write_temp(const std::string& name, const std::string& text) {
    const auto directory = std::filesystem::temp_directory_path() / "mcp-tls-server-test";
    std::filesystem::create_directories(directory);
    const auto path = directory / name;
    std::ofstream(path, std::ios::binary) << text;
    return path;
}

std::string https_url(unsigned short port) {
    return "https://localhost:" + std::to_string(port) + "/mcp";
}

/// Connects to @p url with @p tls, calls greet and closes.
void greet(const std::string& url, const mcp::TlsClientOptions& tls,
           mcp::ClientProtocol protocol = mcp::ClientProtocol::automatic()) {
    mcp::ClientOptions options;
    options.protocol = std::move(protocol);
    mcp::HttpEndpointOptions endpoint_options;
    endpoint_options.configure = [tls](mcp::HttpClientTransport& transport) { transport.set_tls(tls); };
    mcp::Client client(mcp::Endpoint::http(url, std::move(endpoint_options)), options);
    client.sync().connect("tls-server-test", "1.0");
    const auto result = client.sync().call_tool("greet", nlohmann::json::object());
    if (result.isError.value_or(false)) {
        throw std::runtime_error("greet failed");
    }
}

/// How a RawTlsClient connects.
struct RawOptions {
    std::optional<int> max_version;
};

/// A blocking TLS client of the server on @p port, trusting the test authority.
class RawTlsClient {
   public:
    explicit RawTlsClient(unsigned short port, const RawOptions& options = RawOptions{})
        : context_(make_context(options)), stream_(io_, context_) {
        stream_.next_layer().connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));
    }

    boost::system::error_code handshake() {
        boost::system::error_code error;
        stream_.handshake(asio::ssl::stream_base::client, error);
        return error;
    }

    asio::ssl::stream<tcp::socket>& stream() { return stream_; }

   private:
    /// The context, set up before the stream copies its settings.
    static asio::ssl::context make_context(const RawOptions& options) {
        asio::ssl::context context(asio::ssl::context::tls_client);
        context.add_certificate_authority(asio::buffer(certs().ca.cert_pem));
        context.set_verify_mode(asio::ssl::verify_peer);
        if (options.max_version) {
            SSL_CTX_set_max_proto_version(context.native_handle(), *options.max_version);
        }
        return context;
    }

    asio::io_context io_;
    asio::ssl::context context_;
    asio::ssl::stream<tcp::socket> stream_;
};

/// Reads from @p socket until the peer ends the connection, and returns what arrived. Fails the
/// test when the connection is still open after the hang guard.
std::string read_until_closed(asio::io_context& io, tcp::socket& socket) {
    std::string received;
    bool closed = false;
    std::array<char, 512> chunk{};
    std::function<void()> read_more = [&] {
        socket.async_read_some(asio::buffer(chunk),
                               [&](const boost::system::error_code& error, std::size_t bytes) {
                                   received.append(chunk.data(), bytes);
                                   if (error) {
                                       closed = true;
                                       return;
                                   }
                                   read_more();
                               });
    };
    read_more();
    io.restart();
    io.run_for(g_hang_guard);
    EXPECT_TRUE(closed) << "the server kept the connection open";
    return received;
}

/// Repeats a TLS handshake with the server on @p port until one succeeds or the hang guard passes.
bool handshake_eventually(unsigned short port) {
    return eventually(
        [port] {
            RawTlsClient client(port);
            return !client.handshake();
        },
        g_hang_guard, std::chrono::milliseconds(10));
}

/// The bytes of a request with a Host header the server accepts.
std::string request_bytes(http::request<http::string_body> request) {
    request.set(http::field::host, "localhost");
    request.prepare_payload();
    std::ostringstream bytes;
    bytes << request;
    return bytes.str();
}

}  // namespace

// ------------------------------------------------------------------------------------------------
// Serving

TEST(TlsServerTest, TheSessionManagerServesBothProtocolsOverTls) {
    const ManagerHost host(
        [](mcp::StreamableHttpSessionManager& manager) { manager.set_tls(server_options()); });
    greet(https_url(host.port()), trusting_ca(), mcp::ClientProtocol::legacy());
    greet(https_url(host.port()), trusting_ca());
}

TEST(TlsServerTest, TheHttpServerTransportServesOverTls) {
    const TransportHost host([](mcp::HttpServerTransport& transport) {
        transport.set_tls(mcp::TlsServerContext::create(server_options()));
    });
    greet(https_url(host.port()), trusting_ca(), mcp::ClientProtocol::legacy());
    greet(https_url(host.port()), trusting_ca());
}

TEST(TlsServerTest, LoadsItsCertificateFromFilesWithAnEncryptedKey) {
    mcp::TlsServerOptions options;
    options.certificate_chain_file = write_temp("server.pem", certs().server.cert_pem);
    options.private_key_file =
        write_temp("server-key.pem", mcp_test::tls::key_to_pem(certs().server.key.get(), "secret"));
    options.key_password = [] { return std::string("secret"); };
    const ManagerHost host(
        [&options](mcp::StreamableHttpSessionManager& manager) { manager.set_tls(options); });
    greet(https_url(host.port()), trusting_ca());

    options.key_password = [] { return std::string("wrong"); };
    EXPECT_THROW((void)mcp::TlsServerContext::create(options), mcp::TlsConfigError);
}

TEST(TlsServerTest, RefusesOptionsThatCannotBeUsed) {
    const auto refused = [](const std::function<void(mcp::TlsServerOptions&)>& change) {
        auto options = server_options();
        change(options);
        try {
            (void)mcp::TlsServerContext::create(options);
        } catch (const mcp::TlsConfigError&) {
            return true;
        }
        return false;
    };
    EXPECT_TRUE(refused([](auto& options) { options.certificate_chain_pem.clear(); }));
    EXPECT_TRUE(refused([](auto& options) { options.private_key_file = "key.pem"; }));
    EXPECT_TRUE(refused([](auto& options) { options.private_key_pem = certs().client.key_pem; }));
    EXPECT_TRUE(refused([](auto& options) { options.client_ca_pem = {certs().ca.cert_pem}; }));
    EXPECT_TRUE(refused(
        [](auto& options) { options.client_auth = mcp::TlsServerOptions::ClientAuth::eRequired; }));
    EXPECT_TRUE(refused([](auto& options) {
        options.client_auth = mcp::TlsServerOptions::ClientAuth::eRequired;
        options.client_ca_pem = {"not a certificate"};
    }));
    EXPECT_TRUE(refused([](auto& options) {
        options.client_auth = mcp::TlsServerOptions::ClientAuth::eRequired;
        options.client_ca_files = {std::filesystem::temp_directory_path() / "mcp-no-such-ca.pem"};
    }));
    EXPECT_TRUE(refused([](auto& options) { options.certificate_chain_pem = "not a certificate"; }));
    EXPECT_TRUE(refused([](auto& options) {
        options.certificate_chain_pem.clear();
        options.certificate_chain_file = std::filesystem::temp_directory_path() / "mcp-no-such.pem";
    }));
    // A key of another type loads beside the EC certificate, and the check of the pair refuses it.
    EXPECT_TRUE(refused([](auto& options) {
        const mcp_test::tls::KeyPtr key(EVP_RSA_gen(2048), &EVP_PKEY_free);
        options.private_key_pem = mcp_test::tls::key_to_pem(key.get());
    }));
    EXPECT_FALSE(refused([](auto&) {}));
}

TEST(TlsServerTest, TheSettingsAreFixedOnceTheServerListens) {
    const ManagerHost host(nullptr);
    EXPECT_THROW(host.manager().set_tls(server_options()), std::logic_error);
    EXPECT_THROW(host.manager().set_allowed_hosts({"example.com"}), std::logic_error);
}

// ------------------------------------------------------------------------------------------------
// Clients that are refused

TEST(TlsServerTest, RequiresAClientCertificateFromTheNamedAuthorities) {
    auto options = server_options();
    options.client_auth = mcp::TlsServerOptions::ClientAuth::eRequired;
    options.client_ca_pem = {certs().ca.cert_pem};
    const ManagerHost host(
        [&options](mcp::StreamableHttpSessionManager& manager) { manager.set_tls(options); });

    auto with_certificate = trusting_ca();
    with_certificate.client_certificate_file = write_temp("client.pem", certs().client.cert_pem);
    with_certificate.client_key_file = write_temp("client-key.pem", certs().client.key_pem);
    greet(https_url(host.port()), with_certificate);

    EXPECT_ANY_THROW(greet(https_url(host.port()), trusting_ca()));

    auto foreign = trusting_ca();
    foreign.client_certificate_file = write_temp("foreign.pem", certs().foreign_client.cert_pem);
    foreign.client_key_file = write_temp("foreign-key.pem", certs().foreign_client.key_pem);
    EXPECT_ANY_THROW(greet(https_url(host.port()), foreign));
}

TEST(TlsServerTest, ReadsTheClientCertificateAuthoritiesFromFiles) {
    auto options = server_options();
    options.client_auth = mcp::TlsServerOptions::ClientAuth::eRequired;
    options.client_ca_files = {write_temp("client-ca.pem", certs().ca.cert_pem)};
    const ManagerHost host(
        [&options](mcp::StreamableHttpSessionManager& manager) { manager.set_tls(options); });

    auto with_certificate = trusting_ca();
    with_certificate.client_certificate_file = write_temp("client.pem", certs().client.cert_pem);
    with_certificate.client_key_file = write_temp("client-key.pem", certs().client.key_pem);
    greet(https_url(host.port()), with_certificate);
    EXPECT_ANY_THROW(greet(https_url(host.port()), trusting_ca()));
}

TEST(TlsServerTest, ClosesAConnectionThatDoesNotSpeakTls) {
    const ManagerHost host(
        [](mcp::StreamableHttpSessionManager& manager) { manager.set_tls(server_options()); });
    asio::io_context io;
    tcp::socket socket(io);
    socket.connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), host.port()));
    http::request<http::string_body> request{http::verb::post, "/mcp", 11};
    request.set(http::field::content_type, "application/json");
    request.body() = R"({"jsonrpc":"2.0","id":1,"method":"ping"})";
    asio::write(socket, asio::buffer(request_bytes(std::move(request))));

    const auto received = read_until_closed(io, socket);
    EXPECT_NE(received.rfind("HTTP/", 0), 0U) << received;

    greet(https_url(host.port()), trusting_ca());
}

TEST(TlsServerTest, RefusesAClientBelowItsMinimumVersion) {
    auto options = server_options();
    options.min_version = mcp::TlsVersion::eTls13;
    const ManagerHost host(
        [&options](mcp::StreamableHttpSessionManager& manager) { manager.set_tls(options); });
    RawTlsClient old_client(host.port(), {.max_version = TLS1_2_VERSION});
    EXPECT_TRUE(old_client.handshake());
    RawTlsClient client(host.port());
    EXPECT_FALSE(client.handshake());
}

TEST(TlsServerTest, ClosesAClientThatDoesNotHandshakeInTime) {
    auto options = server_options();
    options.handshake_timeout = std::chrono::milliseconds(200);
    const TransportHost host(
        [&options](mcp::HttpServerTransport& transport) { transport.set_tls(options); });
    asio::io_context io;
    tcp::socket silent(io);
    silent.connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), host.port()));
    EXPECT_EQ(read_until_closed(io, silent), "");
}

TEST(TlsServerTest, LimitsTheHandshakesInProgress) {
    auto options = server_options();
    options.handshake_timeout = std::chrono::milliseconds::zero();
    options.max_pending_handshakes = 2;
    const TransportHost host(
        [&options](mcp::HttpServerTransport& transport) { transport.set_tls(options); });
    const tcp::endpoint endpoint(asio::ip::make_address("127.0.0.1"), host.port());
    asio::io_context io;
    tcp::socket first(io);
    tcp::socket second(io);
    tcp::socket third(io);
    first.connect(endpoint);
    second.connect(endpoint);
    // Accepted after the first two, which hold both handshake slots without a time limit.
    third.connect(endpoint);
    EXPECT_EQ(read_until_closed(io, third), "");

    first.close();
    second.close();
    EXPECT_TRUE(handshake_eventually(host.port()));
}

// ------------------------------------------------------------------------------------------------
// Ending connections

TEST(TlsServerTest, SendsCloseNotifyWhenItEndsAConnection) {
    const TransportHost host(
        [](mcp::HttpServerTransport& transport) { transport.set_tls(server_options()); });
    RawTlsClient client(host.port());
    ASSERT_FALSE(client.handshake());
    http::request<http::string_body> request{http::verb::get, "/mcp", 11};
    request.set(http::field::accept, "text/event-stream");
    request.keep_alive(false);
    asio::write(client.stream(), asio::buffer(request_bytes(std::move(request))));
    beast::flat_buffer buffer;
    http::response<http::string_body> response;
    http::read(client.stream(), buffer, response);
    EXPECT_FALSE(response.keep_alive());

    std::array<char, 16> rest{};
    boost::system::error_code error;
    (void)client.stream().read_some(asio::buffer(rest), error);
    EXPECT_EQ(error, asio::error::eof) << error.message();
}

TEST(TlsServerTest, AStreamedRequestEndsWhenItsClientResets) {
    const ManagerHost host(
        [](mcp::StreamableHttpSessionManager& manager) { manager.set_tls(server_options()); });
    RawTlsClient client(host.port());
    ASSERT_FALSE(client.handshake());
    http::request<http::string_body> request{http::verb::post, "/mcp", 11};
    request.set(http::field::content_type, "application/json");
    request.set(http::field::accept, "application/json, text/event-stream");
    request.set("MCP-Protocol-Version", "2026-07-28");
    request.set("Mcp-Method", "subscriptions/listen");
    request.body() = make_modern_request(1, "subscriptions/listen",
                                         {{"notifications", {{"toolsListChanged", true}}}})
                         .dump();
    asio::write(client.stream(), asio::buffer(request_bytes(std::move(request))));
    // The stream opens with the acknowledgement of the subscription.
    std::string received;
    while (received.find("data:") == std::string::npos) {
        std::array<char, 512> chunk{};
        const auto bytes = client.stream().read_some(asio::buffer(chunk));
        received.append(chunk.data(), bytes);
    }

    auto& socket = client.stream().next_layer();
    socket.set_option(asio::socket_base::linger(true, 0));
    socket.close();
    eventually([&] { return host.manager().stream_stats().disconnect_cancels != 0; });
    EXPECT_EQ(host.manager().stream_stats().disconnect_cancels, 1U);
}

}  // namespace transport_tls_server_test

#pragma once

// Test helpers for TLS and proxies: certificates made in memory, a TLS front for a plain HTTP
// server, and an HTTP proxy. Each server runs on an io_context of its own, on a thread of its
// own, and records what it saw for the test to check.

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace mcp_test::tls {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;

// ----------------------------------------------------------------------------------------------
// Certificates

using KeyPtr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using CertPtr = std::unique_ptr<X509, decltype(&X509_free)>;

/// What a certificate says about itself.
struct CertSpec {
    std::string common_name;
    /// Subject alternative names as OpenSSL writes them: "DNS:localhost", "IP:127.0.0.1".
    std::vector<std::string> alt_names;
    bool authority{false};
    /// Issued for a client (clientAuth) rather than a server (serverAuth).
    bool client{false};
    /// Validity, in seconds from now.
    long valid_from{-3600};
    long valid_until{30L * 24 * 3600};
};

/// A certificate and its key, as objects and as PEM.
struct Issued {
    CertPtr cert{nullptr, &X509_free};
    KeyPtr key{nullptr, &EVP_PKEY_free};
    std::string cert_pem;
    std::string key_pem;
};

inline std::string read_bio(BIO* bio) {
    char* data = nullptr;
    const long size = BIO_get_mem_data(bio, &data);
    return {data, static_cast<std::size_t>(size)};
}

inline std::string cert_to_pem(X509* cert) {
    std::unique_ptr<BIO, decltype(&BIO_free)> bio(BIO_new(BIO_s_mem()), &BIO_free);
    PEM_write_bio_X509(bio.get(), cert);
    return read_bio(bio.get());
}

inline std::string key_to_pem(EVP_PKEY* key, const std::string& password = {}) {
    std::unique_ptr<BIO, decltype(&BIO_free)> bio(BIO_new(BIO_s_mem()), &BIO_free);
    if (password.empty()) {
        PEM_write_bio_PrivateKey(bio.get(), key, nullptr, nullptr, 0, nullptr, nullptr);
    } else {
        PEM_write_bio_PKCS8PrivateKey(bio.get(), key, EVP_aes_256_cbc(),
                                      const_cast<char*>(password.data()),
                                      static_cast<int>(password.size()), nullptr, nullptr);
    }
    return read_bio(bio.get());
}

inline void add_extension(X509* cert, X509V3_CTX& context, int nid, const std::string& value) {
    X509_EXTENSION* extension = X509V3_EXT_conf_nid(nullptr, &context, nid, value.c_str());
    if (extension == nullptr) {
        throw std::runtime_error("bad test certificate extension " + value);
    }
    X509_add_ext(cert, extension, -1);
    X509_EXTENSION_free(extension);
}

/// Issues a certificate signed by @p issuer, or self-signed when it is null.
inline Issued issue(const CertSpec& spec, const Issued* issuer = nullptr) {
    static std::atomic<long> serial{1};
    Issued issued;
    issued.key.reset(EVP_EC_gen("P-256"));
    issued.cert.reset(X509_new());
    X509* cert = issued.cert.get();
    X509_set_version(cert, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(cert), serial++);
    X509_gmtime_adj(X509_getm_notBefore(cert), spec.valid_from);
    X509_gmtime_adj(X509_getm_notAfter(cert), spec.valid_until);
    X509_set_pubkey(cert, issued.key.get());
    X509_NAME* name = X509_get_subject_name(cert);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                               reinterpret_cast<const unsigned char*>(spec.common_name.c_str()), -1, -1,
                               0);
    X509_set_issuer_name(cert, issuer != nullptr ? X509_get_subject_name(issuer->cert.get()) : name);

    X509V3_CTX context;
    X509V3_set_ctx_nodb(&context);
    X509V3_set_ctx(&context, issuer != nullptr ? issuer->cert.get() : cert, cert, nullptr, nullptr, 0);
    add_extension(cert, context, NID_basic_constraints,
                  spec.authority ? "critical,CA:TRUE" : "critical,CA:FALSE");
    add_extension(cert, context, NID_key_usage,
                  spec.authority ? "critical,keyCertSign,cRLSign" : "critical,digitalSignature");
    if (!spec.authority) {
        add_extension(cert, context, NID_ext_key_usage, spec.client ? "clientAuth" : "serverAuth");
    }
    if (!spec.alt_names.empty()) {
        std::string names;
        for (const auto& alt_name : spec.alt_names) {
            names += (names.empty() ? "" : ",") + alt_name;
        }
        add_extension(cert, context, NID_subject_alt_name, names);
    }
    add_extension(cert, context, NID_subject_key_identifier, "hash");
    if (issuer != nullptr) {
        add_extension(cert, context, NID_authority_key_identifier, "keyid:always");
    }
    X509_sign(cert, issuer != nullptr ? issuer->key.get() : issued.key.get(), EVP_sha256());
    issued.cert_pem = cert_to_pem(cert);
    issued.key_pem = key_to_pem(issued.key.get());
    return issued;
}

// ----------------------------------------------------------------------------------------------
// Servers

/// Runs an io_context on a thread of its own until the object is destroyed, and closes what the
/// server registered on the way out.
class ServerThread {
   public:
    ServerThread() : work_(asio::make_work_guard(io_)) {}
    ServerThread(const ServerThread&) = delete;
    ServerThread& operator=(const ServerThread&) = delete;

    ~ServerThread() { stop(); }

    void start() {
        thread_ = std::thread([this] { io_.run(); });
    }

    void stop() {
        if (!thread_.joinable()) {
            return;
        }
        asio::post(io_, [this] {
            for (auto& closer : closers_) {
                closer();
            }
            closers_.clear();
            io_.stop();
        });
        thread_.join();
    }

    asio::io_context& io() { return io_; }

    /// Runs @p closer when the server stops. Called on the server's thread.
    void on_stop(std::function<void()> closer) { closers_.push_back(std::move(closer)); }

   private:
    asio::io_context io_;
    asio::executor_work_guard<asio::io_context::executor_type> work_;
    std::thread thread_;
    std::vector<std::function<void()>> closers_;
};

/// Copies the response @p in reads to @p out as it arrives, an event stream piece by piece.
/// Returns the body bytes copied; stops early once @p cut_after body bytes went out (0: never).
template <typename In, typename Out>
asio::awaitable<std::size_t> relay_response(In& in, beast::flat_buffer& buffer, Out& out,
                                            std::size_t cut_after, bool& keep_alive,
                                            boost::system::error_code& error) {
    http::response_parser<http::buffer_body> parser;
    parser.body_limit(std::numeric_limits<std::uint64_t>::max());
    co_await http::async_read_header(in, buffer, parser,
                                     asio::redirect_error(asio::use_awaitable, error));
    if (error) {
        co_return 0;
    }
    keep_alive = parser.get().keep_alive();
    http::response_serializer<http::buffer_body> serializer{parser.get()};
    parser.get().body().data = nullptr;
    parser.get().body().more = !parser.is_done();
    co_await http::async_write_header(out, serializer,
                                      asio::redirect_error(asio::use_awaitable, error));
    if (error) {
        co_return 0;
    }
    std::array<char, 4096> chunk{};
    std::size_t copied = 0;
    while (!parser.is_done()) {
        std::size_t room = chunk.size();
        if (cut_after != 0) {
            room = std::min(room, cut_after - copied);
        }
        parser.get().body().data = chunk.data();
        parser.get().body().size = room;
        co_await http::async_read(in, buffer, parser, asio::redirect_error(asio::use_awaitable, error));
        if (error == http::error::need_buffer) {
            error = {};
        }
        if (error) {
            co_return copied;
        }
        const std::size_t got = room - parser.get().body().size;
        parser.get().body().data = chunk.data();
        parser.get().body().size = got;
        parser.get().body().more = !parser.is_done();
        co_await http::async_write(out, serializer, asio::redirect_error(asio::use_awaitable, error));
        if (error == http::error::need_buffer) {
            error = {};
        }
        if (error) {
            co_return copied;
        }
        copied += got;
        if (cut_after != 0 && copied >= cut_after) {
            co_return copied;
        }
    }
    if (!serializer.is_done()) {
        parser.get().body().data = nullptr;
        parser.get().body().size = 0;
        parser.get().body().more = false;
        co_await http::async_write(out, serializer, asio::redirect_error(asio::use_awaitable, error));
        if (error == http::error::need_buffer) {
            error = {};
        }
    }
    co_return copied;
}

/// How a TlsFront behaves.
struct FrontOptions {
    std::string cert_pem;  ///< The server certificate, followed by its chain.
    std::string key_pem;
    /// When set, a client certificate issued by this authority is required.
    std::string client_ca_pem;
    /// The highest TLS version offered, such as TLS1_2_VERSION; 0 for the library's.
    int max_version{0};
    /// Send a session ticket after each response, once the response has gone out.
    bool ticket_after_response{false};
    /// Cut the connection, without close_notify, after this many bytes of a response body.
    std::size_t cut_after_body_bytes{0};
    /// Close each connection, without close_notify, once a response has gone out, although the
    /// response keeps the connection alive.
    bool close_after_response{false};
};

/// A TLS server in front of a plain HTTP server on the loopback interface: it ends TLS and passes
/// each request to the backend and each response back, on a backend connection of its own per
/// client connection.
class TlsFront {
   public:
    TlsFront(FrontOptions options, unsigned short backend_port)
        : options_(std::move(options)),
          backend_port_(backend_port),
          context_(asio::ssl::context::tls_server),
          acceptor_(server_.io(), tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0)) {
        context_.use_certificate_chain(asio::buffer(options_.cert_pem));
        context_.use_private_key(asio::buffer(options_.key_pem), asio::ssl::context::pem);
        if (options_.max_version != 0) {
            SSL_CTX_set_max_proto_version(context_.native_handle(), options_.max_version);
        }
        if (!options_.client_ca_pem.empty()) {
            context_.add_certificate_authority(asio::buffer(options_.client_ca_pem));
            context_.set_verify_mode(asio::ssl::verify_peer | asio::ssl::verify_fail_if_no_peer_cert);
        }
        asio::co_spawn(server_.io(), accept_loop(), asio::detached);
        server_.start();
    }

    TlsFront(const TlsFront&) = delete;
    TlsFront& operator=(const TlsFront&) = delete;
    ~TlsFront() { server_.stop(); }

    [[nodiscard]] unsigned short port() const { return acceptor_.local_endpoint().port(); }

    [[nodiscard]] int handshakes() const {
        std::lock_guard lock(mutex_);
        return handshakes_;
    }
    /// The server name each connection's client sent, empty when it sent none.
    [[nodiscard]] std::vector<std::string> server_names() const {
        std::lock_guard lock(mutex_);
        return server_names_;
    }
    /// The Host header of each request.
    [[nodiscard]] std::vector<std::string> hosts() const {
        std::lock_guard lock(mutex_);
        return hosts_;
    }
    /// The requests each finished connection carried.
    [[nodiscard]] std::vector<int> requests_per_connection() const {
        std::lock_guard lock(mutex_);
        return requests_per_connection_;
    }
    /// How many connections the client ended with its close_notify.
    [[nodiscard]] int close_notifies() const {
        std::lock_guard lock(mutex_);
        return close_notifies_;
    }
    /// How many connections close_after_response closed.
    [[nodiscard]] int server_closes() const {
        std::lock_guard lock(mutex_);
        return server_closes_;
    }

   private:
    using TlsStream = asio::ssl::stream<tcp::socket>;

    asio::awaitable<void> accept_loop() {
        server_.on_stop([this] {
            boost::system::error_code ignored;
            acceptor_.close(ignored);
        });
        for (;;) {
            boost::system::error_code error;
            auto socket =
                co_await acceptor_.async_accept(asio::redirect_error(asio::use_awaitable, error));
            if (error) {
                co_return;
            }
            asio::co_spawn(server_.io(),
                           serve(std::make_shared<TlsStream>(std::move(socket), context_)),
                           asio::detached);
        }
    }

    asio::awaitable<void> serve(std::shared_ptr<TlsStream> client) {
        auto backend = std::make_shared<tcp::socket>(server_.io());
        server_.on_stop([client, backend] {
            boost::system::error_code ignored;
            client->next_layer().close(ignored);
            backend->close(ignored);
        });
        boost::system::error_code error;
        co_await client->async_handshake(asio::ssl::stream_base::server,
                                         asio::redirect_error(asio::use_awaitable, error));
        if (error) {
            co_return;
        }
        {
            const char* name = SSL_get_servername(client->native_handle(), TLSEXT_NAMETYPE_host_name);
            std::lock_guard lock(mutex_);
            ++handshakes_;
            server_names_.emplace_back(name != nullptr ? name : "");
        }
        co_await backend->async_connect(
            tcp::endpoint(asio::ip::make_address("127.0.0.1"), backend_port_),
            asio::redirect_error(asio::use_awaitable, error));
        if (error) {
            co_return;
        }
        beast::flat_buffer client_buffer;
        beast::flat_buffer backend_buffer;
        int requests = 0;
        for (;;) {
            http::request_parser<http::string_body> request_parser;
            request_parser.body_limit(std::numeric_limits<std::uint64_t>::max());
            co_await http::async_read(*client, client_buffer, request_parser,
                                      asio::redirect_error(asio::use_awaitable, error));
            if (error) {
                // Beast reports a clean end of stream between messages; over TLS that takes the
                // client's close_notify.
                std::lock_guard lock(mutex_);
                if (error == http::error::end_of_stream) {
                    ++close_notifies_;
                }
                requests_per_connection_.push_back(requests);
                break;
            }
            ++requests;
            auto request = request_parser.release();
            {
                std::lock_guard lock(mutex_);
                hosts_.emplace_back(request[http::field::host]);
            }
            co_await http::async_write(*backend, request,
                                       asio::redirect_error(asio::use_awaitable, error));
            if (error) {
                break;
            }
            bool keep_alive = true;
            const auto copied = co_await relay_response(
                *backend, backend_buffer, *client, options_.cut_after_body_bytes, keep_alive, error);
            if (error) {
                break;
            }
            if (options_.cut_after_body_bytes != 0 && copied >= options_.cut_after_body_bytes) {
                boost::system::error_code ignored;
                client->next_layer().close(ignored);
                break;
            }
            if (options_.close_after_response) {
                boost::system::error_code ignored;
                client->next_layer().close(ignored);
                std::lock_guard lock(mutex_);
                ++server_closes_;
                break;
            }
            if (options_.ticket_after_response) {
                // A TLS 1.3 server may send a ticket at any time; the next handshake call sends it.
                if (SSL_new_session_ticket(client->native_handle()) == 1) {
                    co_await client->async_handshake(asio::ssl::stream_base::server,
                                                     asio::redirect_error(asio::use_awaitable, error));
                    if (error) {
                        break;
                    }
                }
            }
            if (!keep_alive) {
                co_await client->async_shutdown(asio::redirect_error(asio::use_awaitable, error));
                break;
            }
        }
        boost::system::error_code ignored;
        backend->close(ignored);
        client->next_layer().close(ignored);
    }

    FrontOptions options_;
    unsigned short backend_port_;
    asio::ssl::context context_;
    ServerThread server_;
    tcp::acceptor acceptor_;
    mutable std::mutex mutex_;
    int handshakes_{0};
    int server_closes_{0};
    int close_notifies_{0};
    std::vector<std::string> server_names_;
    std::vector<std::string> hosts_;
    std::vector<int> requests_per_connection_;
};

/// An HTTP proxy on the loopback interface. Whatever server a client names, the proxy reaches
/// the one on @p backend_port: through a tunnel for CONNECT, else by passing the request on with
/// an origin-form target, one request per connection.
class TestProxy {
   public:
    explicit TestProxy(unsigned short backend_port)
        : backend_port_(backend_port),
          acceptor_(server_.io(), tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0)) {
        asio::co_spawn(server_.io(), accept_loop(), asio::detached);
        server_.start();
    }

    TestProxy(const TestProxy&) = delete;
    TestProxy& operator=(const TestProxy&) = delete;
    ~TestProxy() { server_.stop(); }

    [[nodiscard]] unsigned short port() const { return acceptor_.local_endpoint().port(); }

    /// The authority of each CONNECT.
    [[nodiscard]] std::vector<std::string> tunnels() const {
        std::lock_guard lock(mutex_);
        return tunnels_;
    }
    /// The target of each request passed on.
    [[nodiscard]] std::vector<std::string> forwarded() const {
        std::lock_guard lock(mutex_);
        return forwarded_;
    }
    /// The Proxy-Authorization of each CONNECT or request.
    [[nodiscard]] std::vector<std::string> authorizations() const {
        std::lock_guard lock(mutex_);
        return authorizations_;
    }
    [[nodiscard]] int connections() const {
        std::lock_guard lock(mutex_);
        return connections_;
    }

   private:
    asio::awaitable<void> accept_loop() {
        server_.on_stop([this] {
            boost::system::error_code ignored;
            acceptor_.close(ignored);
        });
        for (;;) {
            boost::system::error_code error;
            auto socket =
                co_await acceptor_.async_accept(asio::redirect_error(asio::use_awaitable, error));
            if (error) {
                co_return;
            }
            asio::co_spawn(server_.io(), serve(std::make_shared<tcp::socket>(std::move(socket))),
                           asio::detached);
        }
    }

    static asio::awaitable<void> pump(std::shared_ptr<tcp::socket> from,
                                      std::shared_ptr<tcp::socket> to) {
        std::array<char, 4096> chunk{};
        for (;;) {
            boost::system::error_code error;
            const auto got = co_await from->async_read_some(
                asio::buffer(chunk), asio::redirect_error(asio::use_awaitable, error));
            if (error) {
                break;
            }
            co_await asio::async_write(*to, asio::buffer(chunk.data(), got),
                                       asio::redirect_error(asio::use_awaitable, error));
            if (error) {
                break;
            }
        }
        boost::system::error_code ignored;
        to->shutdown(tcp::socket::shutdown_send, ignored);
    }

    asio::awaitable<void> serve(std::shared_ptr<tcp::socket> client) {
        auto backend = std::make_shared<tcp::socket>(server_.io());
        server_.on_stop([client, backend] {
            boost::system::error_code ignored;
            client->close(ignored);
            backend->close(ignored);
        });
        {
            std::lock_guard lock(mutex_);
            ++connections_;
        }
        boost::system::error_code error;
        beast::flat_buffer buffer;
        http::request_parser<http::string_body> parser;
        co_await http::async_read(*client, buffer, parser,
                                  asio::redirect_error(asio::use_awaitable, error));
        if (error) {
            co_return;
        }
        auto request = parser.release();
        {
            std::lock_guard lock(mutex_);
            authorizations_.emplace_back(request[http::field::proxy_authorization]);
            if (request.method() == http::verb::connect) {
                tunnels_.emplace_back(request.target());
            } else {
                forwarded_.emplace_back(request.target());
            }
        }
        co_await backend->async_connect(
            tcp::endpoint(asio::ip::make_address("127.0.0.1"), backend_port_),
            asio::redirect_error(asio::use_awaitable, error));
        if (error) {
            co_return;
        }
        if (request.method() == http::verb::connect) {
            static constexpr std::string_view g_established =
                "HTTP/1.1 200 Connection Established\r\n\r\n";
            co_await asio::async_write(*client, asio::buffer(g_established),
                                       asio::redirect_error(asio::use_awaitable, error));
            if (error) {
                co_return;
            }
            asio::co_spawn(server_.io(), pump(client, backend), asio::detached);
            co_await pump(backend, client);
            co_return;
        }
        // Requests are passed on one after another while the connection is kept alive.
        beast::flat_buffer backend_buffer;
        for (;;) {
            std::string target(request.target());
            if (const auto scheme = target.find("://"); scheme != std::string::npos) {
                const auto path = target.find('/', scheme + 3);
                target = path == std::string::npos ? "/" : target.substr(path);
            }
            request.target(target);
            request.erase(http::field::proxy_authorization);
            co_await http::async_write(*backend, request,
                                       asio::redirect_error(asio::use_awaitable, error));
            if (error) {
                break;
            }
            bool keep_alive = true;
            (void)co_await relay_response(*backend, backend_buffer, *client, 0, keep_alive, error);
            if (error || !keep_alive) {
                break;
            }
            http::request_parser<http::string_body> next;
            co_await http::async_read(*client, buffer, next,
                                      asio::redirect_error(asio::use_awaitable, error));
            if (error) {
                break;
            }
            request = next.release();
            {
                std::lock_guard lock(mutex_);
                authorizations_.emplace_back(request[http::field::proxy_authorization]);
                forwarded_.emplace_back(request.target());
            }
        }
        boost::system::error_code ignored;
        client->shutdown(tcp::socket::shutdown_send, ignored);
    }

    unsigned short backend_port_;
    ServerThread server_;
    tcp::acceptor acceptor_;
    mutable std::mutex mutex_;
    int connections_{0};
    std::vector<std::string> tunnels_;
    std::vector<std::string> forwarded_;
    std::vector<std::string> authorizations_;
};

}  // namespace mcp_test::tls

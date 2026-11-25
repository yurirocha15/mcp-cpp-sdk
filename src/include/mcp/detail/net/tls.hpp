#pragma once

#include <boost/asio/ssl/context.hpp>
#include <boost/system/error_code.hpp>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <mcp/transport/tls.hpp>
#include <memory>
#include <string>
#include <vector>

using SSL = struct ssl_st;
using X509 = struct x509_st;

namespace mcp::detail {

/// The OpenSSL context inside the public TLS context types.
struct TlsContextAccess {
    /// The context a stream is built on. Asio's stream takes it by non-const reference; OpenSSL
    /// takes a reference on it for every stream, and nothing changes it after create().
    static boost::asio::ssl::context& native(const TlsClientContext& context);
    static boost::asio::ssl::context& native(const TlsServerContext& context);

    /// What a listener needs of a server context besides the context itself.
    struct ServerLimits {
        std::chrono::milliseconds handshake_timeout;
        std::size_t max_pending_handshakes;
    };
    static ServerLimits limits(const TlsServerContext& context);
};

/// The context of a client given no TLS options: the system's authorities, TLS 1.2 or later.
/// Created once per process.
std::shared_ptr<const TlsClientContext> default_client_context();

/// The certificate authorities the operating system trusts.
struct SystemRoots {
    SystemRoots() = default;
    SystemRoots(const SystemRoots&) = delete;
    SystemRoots& operator=(const SystemRoots&) = delete;
    SystemRoots(SystemRoots&&) = delete;
    SystemRoots& operator=(SystemRoots&&) = delete;
    ~SystemRoots();

    /// Owned here; X509_STORE_add_cert() takes a reference of its own.
    std::vector<X509*> certificates;
    /// Hashed certificate directories, which each store reads on demand.
    std::vector<std::string> directories;
    /// Whether SSL_CERT_FILE or SSL_CERT_DIR named them, in place of the platform's stores.
    bool from_environment{false};
};

/// The system's certificate authorities, read on first use and kept for the life of the process.
/// A read that finds none is not kept, so the next call reads again.
std::shared_ptr<const SystemRoots> system_roots();

/// The name a client checks the server's certificate against, as the URL carries it: brackets
/// around an IPv6 literal and a trailing dot are removed.
std::string tls_server_name(std::string host);

/// Prepares a client stream's SSL object for @p server_name (from tls_server_name()): SNI for a host
/// name, never for an IP address, and the check that the certificate names the host or address.
void prepare_client_ssl(SSL* ssl, const std::string& server_name);

/// What a failed client handshake with @p host means, as the TlsError the caller throws.
TlsError handshake_failure(const boost::system::error_code& error, SSL* ssl, const std::string& host,
                           bool timed_out);

/// What a failed server handshake with the client at @p client means, as a TlsError.
TlsError server_handshake_failure(const boost::system::error_code& error, SSL* ssl,
                                  const std::string& client, bool timed_out);

/// Whether @p error says the peer ended a TLS connection without its close_notify.
bool is_tls_truncation(const boost::system::error_code& error);

/// What a failed read or write on an established TLS connection with @p host means. Null when
/// @p error is no TLS error, such as a closed socket.
std::unique_ptr<TlsError> established_failure(const boost::system::error_code& error,
                                              const std::string& host);

/// A path as OpenSSL's file functions take it: UTF-8 on Windows, the native bytes elsewhere.
std::string openssl_path(const std::filesystem::path& path);

}  // namespace mcp::detail

#include "../detail/tls.hpp"

#include "../detail/constants.hpp"

#include <boost/asio/error.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ssl/error.hpp>

#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509_vfy.h>
#include <openssl/x509v3.h>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace mcp {

namespace net = boost::asio;

struct TlsClientContext::Impl {
    Impl() : context(net::ssl::context::tls_client) {}

    // Mutable: an Asio stream takes the context by non-const reference. Nothing changes it once
    // create() returns.
    mutable net::ssl::context context;
};

namespace {

/// The reason OpenSSL gives for the last error it queued, and the queue cleared.
std::string openssl_reason() {
    const unsigned long code = ERR_peek_last_error();
    std::string reason;
    if (code != 0) {
        const char* text = ERR_reason_error_string(code);
        reason = text != nullptr ? text : "unknown error";
    } else {
        reason = "unknown error";
    }
    ERR_clear_error();
    return reason;
}

/// Hands OpenSSL the passphrase of an encrypted key.
int password_callback(char* buffer, int size, int /*rwflag*/, void* user_data) {
    const auto& provider = *static_cast<const std::function<std::string()>*>(user_data);
    std::string password;
    try {
        password = provider();
    } catch (...) {
        return -1;
    }
    if (password.size() > static_cast<std::size_t>(size)) {
        return -1;
    }
    password.copy(buffer, password.size());
    const auto length = static_cast<int>(password.size());
    OPENSSL_cleanse(password.data(), password.size());
    return length;
}

/// Adds every certificate of the PEM text @p pem to @p store. Returns how many there were.
std::size_t add_pem_certificates(X509_STORE* store, const std::string& pem) {
    std::unique_ptr<BIO, decltype(&BIO_free)> bio(
        BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())), &BIO_free);
    if (!bio) {
        throw TlsConfigError("Cannot read a ca_pem entry: " + openssl_reason());
    }
    std::size_t count = 0;
    for (;;) {
        X509* certificate = PEM_read_bio_X509_AUX(bio.get(), nullptr, nullptr, nullptr);
        if (certificate == nullptr) {
            break;
        }
        X509_STORE_add_cert(store, certificate);
        X509_free(certificate);
        ++count;
    }
    // The read that ends the loop queues a "no start line" error.
    ERR_clear_error();
    return count;
}

/// Loads the client certificate chain and its key into @p context.
void load_client_certificate(SSL_CTX* context, const TlsClientOptions& options) {
    if (options.client_certificate_file.empty() != options.client_key_file.empty()) {
        throw TlsConfigError(
            "client_certificate_file and client_key_file must be set together for a client "
            "certificate");
    }
    if (options.client_certificate_file.empty()) {
        return;
    }
    if (SSL_CTX_use_certificate_chain_file(
            context, detail::openssl_path(options.client_certificate_file).c_str()) != 1) {
        throw TlsConfigError("Cannot load the client certificate " +
                             options.client_certificate_file.filename().string() + ": " +
                             openssl_reason());
    }
    if (options.key_password) {
        SSL_CTX_set_default_passwd_cb(context, &password_callback);
        SSL_CTX_set_default_passwd_cb_userdata(
            context, const_cast<std::function<std::string()>*>(&options.key_password));
    }
    const int loaded = SSL_CTX_use_PrivateKey_file(
        context, detail::openssl_path(options.client_key_file).c_str(), SSL_FILETYPE_PEM);
    SSL_CTX_set_default_passwd_cb(context, nullptr);
    SSL_CTX_set_default_passwd_cb_userdata(context, nullptr);
    if (loaded != 1) {
        throw TlsConfigError("Cannot load the client key: " + openssl_reason());
    }
    if (SSL_CTX_check_private_key(context) != 1) {
        throw TlsConfigError("The client key does not match the client certificate: " +
                             openssl_reason());
    }
}

}  // namespace

TlsClientContext::TlsClientContext(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

TlsClientContext::~TlsClientContext() = default;

std::shared_ptr<const TlsClientContext> TlsClientContext::create(const TlsClientOptions& options) {
    auto impl = std::make_unique<Impl>();
    SSL_CTX* context = impl->context.native_handle();

    SSL_CTX_set_min_proto_version(
        context, options.min_version == TlsVersion::eTls13 ? TLS1_3_VERSION : TLS1_2_VERSION);
    // The client keeps no sessions, so it asks for no tickets.
    SSL_CTX_set_options(context, SSL_OP_NO_COMPRESSION | SSL_OP_NO_RENEGOTIATION | SSL_OP_NO_TICKET);
    SSL_CTX_set_session_cache_mode(context, SSL_SESS_CACHE_OFF);
    // Asio's context verifies nothing until told to.
    SSL_CTX_set_verify(context, SSL_VERIFY_PEER, nullptr);

    X509_STORE* store = SSL_CTX_get_cert_store(context);
    // A named authority is a trust anchor whether or not it is a self-signed root, as an
    // intermediate CA given in ca_files is meant to be.
    X509_STORE_set_flags(store, X509_V_FLAG_PARTIAL_CHAIN);

    bool custom_roots = false;
    for (const auto& file : options.ca_files) {
        if (X509_STORE_load_file(store, detail::openssl_path(file).c_str()) != 1) {
            throw TlsConfigError("Cannot load the CA file " + file.filename().string() + ": " +
                                 openssl_reason());
        }
        custom_roots = true;
    }
    for (const auto& directory : options.ca_dirs) {
        if (X509_STORE_load_path(store, detail::openssl_path(directory).c_str()) != 1) {
            throw TlsConfigError("Cannot use the CA directory " + directory.string() + ": " +
                                 openssl_reason());
        }
        custom_roots = true;
    }
    for (std::size_t index = 0; index < options.ca_pem.size(); ++index) {
        if (add_pem_certificates(store, options.ca_pem[index]) == 0) {
            throw TlsConfigError("ca_pem entry " + std::to_string(index) + " holds no certificate");
        }
        custom_roots = true;
    }

    if (options.use_system_roots) {
        const auto roots = detail::system_roots();
        for (X509* certificate : roots->certificates) {
            X509_STORE_add_cert(store, certificate);
        }
        for (const auto& directory : roots->directories) {
            X509_STORE_load_path(store, directory.c_str());
        }
        if (roots->certificates.empty() && roots->directories.empty()) {
            // Whatever this OpenSSL was built to look at.
            SSL_CTX_set_default_verify_paths(context);
        }
        ERR_clear_error();
    } else if (!custom_roots) {
        throw TlsConfigError(
            "use_system_roots is false and no certificate authority is named in ca_files, "
            "ca_dirs or ca_pem");
    }

    load_client_certificate(context, options);
    return std::shared_ptr<const TlsClientContext>(new TlsClientContext(std::move(impl)));
}

namespace detail {

net::ssl::context& TlsContextAccess::native(const TlsClientContext& context) {
    return context.impl_->context;
}

std::shared_ptr<const TlsClientContext> default_client_context() {
    static const std::shared_ptr<const TlsClientContext> s_context =
        TlsClientContext::create(TlsClientOptions{});
    return s_context;
}

std::string openssl_path(const std::filesystem::path& path) {
#if defined(_WIN32)
    const auto utf8 = path.u8string();
    return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
#else
    return path.string();
#endif
}

std::string tls_server_name(std::string host) {
    if (host.size() >= 2 && host.front() == '[' && host.back() == ']') {
        host = host.substr(1, host.size() - 2);
    }
    if (!host.empty() && host.back() == '.') {
        host.pop_back();
    }
    return host;
}

void prepare_client_ssl(SSL* ssl, const std::string& server_name) {
    boost::system::error_code not_an_address;
    (void)net::ip::make_address(server_name, not_an_address);
    if (!not_an_address) {
        // An address is checked against the certificate's IP addresses, and RFC 6066 allows no
        // address in SNI.
        if (X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(ssl), server_name.c_str()) != 1) {
            throw TlsError(TlsError::Kind::eHandshakeFailed,
                           "TLS to " + server_name + " failed: the address cannot be checked");
        }
        return;
    }
    // A wildcard covers one whole label, and a certificate that names its host only in the
    // subject's common name is refused.
    SSL_set_hostflags(ssl, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS | X509_CHECK_FLAG_NEVER_CHECK_SUBJECT);
    if (SSL_set1_host(ssl, server_name.c_str()) != 1 ||
        SSL_set_tlsext_host_name(ssl, const_cast<char*>(server_name.c_str())) != 1) {
        ERR_clear_error();
        throw TlsError(TlsError::Kind::eHandshakeFailed,
                       "TLS to " + server_name + " failed: the host name cannot be checked");
    }
}

namespace {

bool is_ssl_error(const boost::system::error_code& error) {
    return error.category() == net::error::get_ssl_category();
}

int ssl_reason(const boost::system::error_code& error) {
    return ERR_GET_REASON(static_cast<unsigned long>(error.value()));
}

TlsError verification_failure(SSL* ssl, const std::string& host) {
    const long result = ssl != nullptr ? SSL_get_verify_result(ssl) : X509_V_OK;
    const std::string detail = X509_verify_cert_error_string(result);
    TlsError::Kind kind = TlsError::Kind::eUntrustedCertificate;
    std::string what = "the certificate is not trusted";
    switch (result) {
        case X509_V_ERR_CERT_HAS_EXPIRED:
        case X509_V_ERR_CERT_NOT_YET_VALID:
            kind = TlsError::Kind::eExpiredCertificate;
            what = result == X509_V_ERR_CERT_HAS_EXPIRED ? "the certificate has expired"
                                                         : "the certificate is not valid yet";
            break;
        case X509_V_ERR_HOSTNAME_MISMATCH:
        case X509_V_ERR_IP_ADDRESS_MISMATCH:
            kind = TlsError::Kind::eHostnameMismatch;
            what = "the certificate does not name " + host;
            break;
        default:
            break;
    }
    return {kind, "TLS to " + host + " failed: " + what + " (" + detail + ")"};
}

}  // namespace

TlsError handshake_failure(const boost::system::error_code& error, SSL* ssl, const std::string& host,
                           bool timed_out) {
    const std::string prefix = "TLS to " + host + " failed: ";
    if (timed_out) {
        return {TlsError::Kind::eHandshakeTimeout, prefix + "the handshake did not finish in time"};
    }
    if (is_ssl_error(error)) {
        switch (ssl_reason(error)) {
            case SSL_R_CERTIFICATE_VERIFY_FAILED:
                return verification_failure(ssl, host);
            case SSL_R_UNSUPPORTED_PROTOCOL:
            case SSL_R_NO_PROTOCOLS_AVAILABLE:
            case SSL_R_TLSV1_ALERT_PROTOCOL_VERSION:
            case SSL_R_VERSION_TOO_LOW:
                return {TlsError::Kind::eProtocolVersion,
                        prefix + "no TLS version in common (" + error.message() + ")"};
            case SSL_R_WRONG_VERSION_NUMBER:
            case SSL_R_PACKET_LENGTH_TOO_LONG:
            case SSL_R_HTTP_REQUEST:
            case SSL_R_HTTPS_PROXY_REQUEST:
                return {TlsError::Kind::eNotTls,
                        prefix + "the server does not speak TLS (" + error.message() + ")"};
            default:
                return {TlsError::Kind::eHandshakeFailed, prefix + error.message()};
        }
    }
    if (is_tls_truncation(error) || error == net::error::eof || error == net::error::connection_reset) {
        return {TlsError::Kind::eHandshakeFailed,
                prefix + "the server closed the connection during the handshake"};
    }
    return {TlsError::Kind::eHandshakeFailed, prefix + error.message()};
}

bool is_tls_truncation(const boost::system::error_code& error) {
    if (error == net::ssl::error::stream_truncated) {
        return true;
    }
#if defined(SSL_R_UNEXPECTED_EOF_WHILE_READING)
    return is_ssl_error(error) && ssl_reason(error) == SSL_R_UNEXPECTED_EOF_WHILE_READING;
#else
    return false;
#endif
}

std::unique_ptr<TlsError> established_failure(const boost::system::error_code& error,
                                              const std::string& host) {
    if (is_tls_truncation(error)) {
        return std::make_unique<TlsError>(
            TlsError::Kind::eTruncated,
            "TLS to " + host + " failed: the connection ended without close_notify");
    }
    if (is_ssl_error(error)) {
        // A server that refuses the client certificate says so after a TLS 1.3 handshake, in an
        // alert the next read meets.
        return std::make_unique<TlsError>(TlsError::Kind::eHandshakeFailed,
                                          "TLS to " + host + " failed: " + error.message());
    }
    return nullptr;
}

}  // namespace detail

}  // namespace mcp

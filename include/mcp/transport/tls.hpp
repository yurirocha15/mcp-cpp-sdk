#pragma once

#include <mcp/core/export.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace mcp {

namespace detail {
struct TlsContextAccess;
}  // namespace detail

/** @brief The lowest TLS version a connection accepts. */
enum class TlsVersion : std::uint8_t {
    eTls12,  ///< TLS 1.2 and TLS 1.3.
    eTls13,  ///< TLS 1.3 only.
};

/**
 * @brief How a client checks the servers it reaches over TLS, and the certificate it shows them.
 *
 * Certificates and host names are always verified; there is no option to skip either. A server
 * certificate must name the host in a subject alternative name: the host name of the URL, or its
 * IP address when the URL names one. A wildcard covers one whole label.
 */
struct TlsClientOptions {
    /// Trust the certificate authorities of the operating system: the system's CA bundle on Linux
    /// and the BSDs, the Keychain on macOS, the ROOT store on Windows. The authorities named below
    /// are trusted as well. When false, only those are, and at least one must be named.
    bool use_system_roots{true};
    /// PEM files of trusted certificate authorities.
    std::vector<std::filesystem::path> ca_files;
    /// Directories of trusted certificate authorities in OpenSSL's hashed layout (`c_rehash`).
    std::vector<std::filesystem::path> ca_dirs;
    /// Trusted certificate authorities as PEM text.
    std::vector<std::string> ca_pem;
    /// PEM file of the client certificate, followed by its intermediates, for servers that require
    /// one (mutual TLS). Needs client_key_file.
    std::filesystem::path client_certificate_file;
    /// PEM file of the client certificate's private key.
    std::filesystem::path client_key_file;
    /// Returns the passphrase of an encrypted client key. Called while the context is created.
    std::function<std::string()> key_password;
    /// The lowest TLS version accepted.
    TlsVersion min_version{TlsVersion::eTls12};
};

/**
 * @brief Client TLS settings ready for connections: trusted authorities loaded, client
 * certificate checked.
 *
 * Immutable once created and safe to share between transports and threads. Create one and hand it
 * to every transport and OAuth client that should trust the same servers, so the files are read
 * once. The system's certificate authorities are read once per process; a process started before
 * a change to them does not see it.
 */
class MCP_API TlsClientContext {
   public:
    /**
     * @brief Read the files the options name and build a context.
     * @throws TlsConfigError When a file cannot be read or parsed, the client key does not match
     *         the certificate, or no certificate authority is trusted.
     */
    [[nodiscard]] static std::shared_ptr<const TlsClientContext> create(
        const TlsClientOptions& options);

    ~TlsClientContext();
    TlsClientContext(const TlsClientContext&) = delete;
    TlsClientContext& operator=(const TlsClientContext&) = delete;
    TlsClientContext(TlsClientContext&&) = delete;
    TlsClientContext& operator=(TlsClientContext&&) = delete;

   private:
    struct Impl;
    explicit TlsClientContext(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;
    friend struct detail::TlsContextAccess;
};

/** @brief TLS settings that cannot be used: a file that cannot be read, a key that does not match. */
class MCP_API TlsConfigError : public std::runtime_error {
   public:
    using std::runtime_error::runtime_error;
};

/** @brief A TLS connection failed. */
class MCP_API TlsError : public std::runtime_error {
   public:
    /** @brief What went wrong. */
    enum class Kind : std::uint8_t {
        eUntrustedCertificate,  ///< The certificate does not chain to a trusted authority.
        eExpiredCertificate,    ///< The certificate has expired or is not valid yet.
        eHostnameMismatch,      ///< The certificate does not name the host.
        eProtocolVersion,       ///< The two sides have no TLS version in common.
        eNotTls,                ///< The peer does not speak TLS, such as a plain HTTP server.
        eHandshakeFailed,       ///< The handshake failed for another reason.
        eHandshakeTimeout,      ///< The handshake did not finish in time.
        eTruncated,             ///< The connection ended without the peer's close_notify, mid-message.
    };

    TlsError(Kind kind, const std::string& message) : std::runtime_error(message), kind_(kind) {}

    /** @return What went wrong. */
    [[nodiscard]] Kind kind() const noexcept { return kind_; }

   private:
    Kind kind_;
};

}  // namespace mcp

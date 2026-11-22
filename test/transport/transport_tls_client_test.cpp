#include "mcp/auth/oauth.hpp"
#include "mcp/client/client.hpp"
#include "mcp/client/endpoint.hpp"
#include "mcp/core/async.hpp"
#include "mcp/server/server.hpp"
#include "mcp/transport/http_client.hpp"
#include "mcp/transport/http_session_manager.hpp"
#include "mcp/transport/proxy.hpp"
#include "mcp/transport/tls.hpp"

#include <gtest/gtest.h>

#include "../support/hang_guard.hpp"
#include "../support/loopback_server.hpp"
#include "../support/spawn.hpp"
#include "../support/tls_test_support.hpp"

#include <openssl/rsa.h>
#include <openssl/x509.h>

#include <array>
#include <atomic>
#include <boost/asio/io_context.hpp>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace transport_tls_client_test {

namespace {

namespace asio = boost::asio;
namespace http = boost::beast::http;
using mcp::TlsError;
using mcp_test::tls::CertSpec;
using mcp_test::tls::FrontOptions;
using mcp_test::tls::issue;
using mcp_test::tls::Issued;
using mcp_test::tls::TestProxy;
using mcp_test::tls::TlsFront;

/// The certificates the tests use, made once.
struct Certs {
    Issued ca = issue(CertSpec{.common_name = "Test CA", .authority = true});
    Issued other_ca = issue(CertSpec{.common_name = "Other CA", .authority = true});
    Issued server = issue(CertSpec{.common_name = "test server",
                                   .alt_names = {"DNS:localhost", "IP:127.0.0.1", "IP:::1",
                                                 "DNS:mcp.example.test", "DNS:as.example.test"}},
                          &ca);
    Issued untrusted =
        issue(CertSpec{.common_name = "untrusted", .alt_names = {"DNS:localhost"}}, &other_ca);
    Issued expired = issue(CertSpec{.common_name = "expired",
                                    .alt_names = {"DNS:localhost"},
                                    .valid_from = -7200,
                                    .valid_until = -3600},
                           &ca);
    Issued other_name =
        issue(CertSpec{.common_name = "other", .alt_names = {"DNS:other.example.test"}}, &ca);
    Issued common_name_only = issue(CertSpec{.common_name = "localhost"}, &ca);
    Issued wildcard =
        issue(CertSpec{.common_name = "wildcard", .alt_names = {"DNS:*.example.test"}}, &ca);
    Issued dns_only = issue(CertSpec{.common_name = "dns only", .alt_names = {"DNS:localhost"}}, &ca);
    Issued client = issue(CertSpec{.common_name = "client", .client = true}, &ca);
    Issued foreign_client = issue(CertSpec{.common_name = "foreign client", .client = true}, &other_ca);
};

const Certs& certs() {
    static const Certs value;
    return value;
}

FrontOptions front_for(const Issued& server) {
    FrontOptions options;
    options.cert_pem = server.cert_pem;
    options.key_pem = server.key_pem;
    return options;
}

/// Client options that trust @p ca alone.
mcp::TlsClientOptions trusting(const Issued& ca) {
    mcp::TlsClientOptions options;
    options.use_system_roots = false;
    options.ca_pem = {ca.cert_pem};
    return options;
}

/// A key of another type than the test certificates' EC keys, as PEM.
std::string rsa_key_pem() {
    const mcp_test::tls::KeyPtr key(EVP_RSA_gen(2048), &EVP_PKEY_free);
    return mcp_test::tls::key_to_pem(key.get());
}

/// The kind of the TlsError @p error is or nests, if it holds one.
std::optional<TlsError::Kind> tls_kind(const std::exception& error) {
    if (const auto* tls = dynamic_cast<const TlsError*>(&error)) {
        return tls->kind();
    }
    try {
        std::rethrow_if_nested(error);
    } catch (const std::exception& inner) {
        return tls_kind(inner);
    } catch (...) {
    }
    return std::nullopt;
}

/// The text of the TlsError @p error is or nests, or empty.
std::string tls_text(const std::exception& error) {
    if (dynamic_cast<const TlsError*>(&error) != nullptr) {
        return error.what();
    }
    try {
        std::rethrow_if_nested(error);
    } catch (const std::exception& inner) {
        return tls_text(inner);
    } catch (...) {
    }
    return {};
}

/// An MCP server over plain HTTP on a loopback port, with a greet tool. A proxy in front of it passes
/// on the name the client used in Host, which the server accepts once @p allowed_hosts lists it.
class PlainServer {
   public:
    explicit PlainServer(std::vector<std::string> allowed_hosts = {}) {
        mcp::Implementation info;
        info.name = "tls-test-server";
        info.version = "1.0";
        mcp::ServerCapabilities capabilities;
        capabilities.tools = mcp::ServerCapabilities::ToolsCapability{};
        server_ = std::make_unique<mcp::Server>(std::move(info), std::move(capabilities));
        server_->add_tool<nlohmann::json, nlohmann::json>(
            "greet", "Greets", nlohmann::json{{"type", "object"}},
            [](const nlohmann::json&) -> mcp::Task<nlohmann::json> {
                co_return nlohmann::json{{"greeting", "hello"}};
            });
        thread_ = std::thread([this, allowed_hosts = std::move(allowed_hosts)] {
            server_->run_http("127.0.0.1", 0,
                              [&allowed_hosts](mcp::StreamableHttpSessionManager& http) {
                                  if (!allowed_hosts.empty()) {
                                      http.set_allowed_hosts(allowed_hosts);
                                  }
                              });
            finished_ = true;
        });
        while (server_->bound_port() == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        port_ = server_->bound_port();
    }

    PlainServer(const PlainServer&) = delete;
    PlainServer& operator=(const PlainServer&) = delete;

    ~PlainServer() {
        while (!finished_.load()) {
            server_->stop();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        thread_.join();
    }

    [[nodiscard]] unsigned short port() const { return port_; }

   private:
    std::unique_ptr<mcp::Server> server_;
    std::thread thread_;
    std::atomic<bool> finished_{false};
    unsigned short port_{0};
};

using Configure = std::function<void(mcp::HttpClientTransport&)>;

/// Connects to @p url, calls greet and closes.
void greet(const std::string& url, Configure configure,
           mcp::ClientProtocol protocol = mcp::ClientProtocol::automatic()) {
    mcp::ClientOptions options;
    options.protocol = std::move(protocol);
    mcp::HttpEndpointOptions endpoint_options;
    endpoint_options.configure = std::move(configure);
    mcp::Client client(mcp::Endpoint::http(url, std::move(endpoint_options)), options);
    client.sync().connect("tls-test", "1.0");
    const auto result = client.sync().call_tool("greet", nlohmann::json::object());
    if (result.isError.value_or(false)) {
        throw std::runtime_error("greet failed");
    }
}

/// The TLS failure greet() meets, or nullopt when it succeeds or fails another way.
std::optional<TlsError::Kind> greet_failure(const std::string& url, Configure configure) {
    try {
        greet(url, std::move(configure));
    } catch (const std::exception& error) {
        return tls_kind(error);
    }
    return std::nullopt;
}

/// Whether greet() succeeds.
bool greets(const std::string& url, Configure configure) {
    try {
        greet(url, std::move(configure));
    } catch (const std::exception&) {
        return false;
    }
    return true;
}

std::string https_url(unsigned short port, const std::string& host = "localhost") {
    return "https://" + host + ":" + std::to_string(port) + "/mcp";
}

Configure with_tls(mcp::TlsClientOptions options) {
    return [options = std::move(options)](mcp::HttpClientTransport& transport) {
        transport.set_tls(options);
    };
}

/// A file under the test's temporary directory holding @p text.
std::filesystem::path write_temp(const std::string& name, const std::string& text) {
    const auto directory = std::filesystem::temp_directory_path() / "mcp-tls-client-test";
    std::filesystem::create_directories(directory);
    const auto path = directory / name;
    std::ofstream(path, std::ios::binary) << text;
    return path;
}

void set_variable(const char* name, const char* value) {
#if defined(_WIN32)
    _putenv_s(name, value);
#else
    if (*value == '\0') {
        ::unsetenv(name);
    } else {
        ::setenv(name, value, 1);
    }
#endif
}

void use_threadsafe_death_tests() {
#ifdef GTEST_FLAG_SET
    GTEST_FLAG_SET(death_test_style, "threadsafe");
#else  // GoogleTest before 1.12
    ::testing::GTEST_FLAG(death_test_style) = "threadsafe";
#endif
}

}  // namespace

// ------------------------------------------------------------------------------------------------
// Connections that succeed

TEST(TlsClientTest, CallsAToolOverTls) {
    const PlainServer server;
    const TlsFront front(front_for(certs().server), server.port());

    greet(https_url(front.port()), with_tls(trusting(certs().ca)));

    ASSERT_FALSE(front.server_names().empty());
    for (const auto& name : front.server_names()) {
        EXPECT_EQ(name, "localhost");
    }
    // A port other than 443 is part of the Host header.
    for (const auto& host : front.hosts()) {
        EXPECT_EQ(host, "localhost:" + std::to_string(front.port()));
    }
}

TEST(TlsClientTest, TrustsAuthoritiesFromFilesAndSharesAContext) {
    const PlainServer server;
    const TlsFront front(front_for(certs().server), server.port());
    mcp::TlsClientOptions options;
    options.use_system_roots = false;
    options.ca_files = {write_temp("ca.pem", certs().ca.cert_pem)};
    const auto context = mcp::TlsClientContext::create(options);

    for (int round = 0; round < 2; ++round) {
        greet(https_url(front.port()),
              [context](mcp::HttpClientTransport& transport) { transport.set_tls(context); });
    }
}

TEST(TlsClientTest, AnAddressUrlIsCheckedAgainstTheAddressAndSendsNoServerName) {
    const PlainServer server;
    const TlsFront front(front_for(certs().server), server.port());

    greet(https_url(front.port(), "127.0.0.1"), with_tls(trusting(certs().ca)));

    ASSERT_FALSE(front.server_names().empty());
    for (const auto& name : front.server_names()) {
        EXPECT_EQ(name, "");
    }
}

TEST(TlsClientTest, AWildcardCoversOneLabel) {
    const PlainServer server;
    const TlsFront front(front_for(certs().wildcard), server.port());
    const auto options = trusting(certs().ca);

    greet(https_url(front.port()), [&options](mcp::HttpClientTransport& transport) {
        transport.set_tls(options);
        transport.set_server_name("api.example.test");
    });
    EXPECT_EQ(greet_failure(https_url(front.port()),
                            [&options](mcp::HttpClientTransport& transport) {
                                transport.set_tls(options);
                                transport.set_server_name("deep.api.example.test");
                            }),
              TlsError::Kind::eHostnameMismatch);
    EXPECT_EQ(greet_failure(https_url(front.port()),
                            [&options](mcp::HttpClientTransport& transport) {
                                transport.set_tls(options);
                                transport.set_server_name("example.test");
                            }),
              TlsError::Kind::eHostnameMismatch);
}

TEST(TlsClientTest, PresentsAClientCertificateWhenTheServerRequiresOne) {
    const PlainServer server;
    auto options = front_for(certs().server);
    options.client_ca_pem = certs().ca.cert_pem;
    const TlsFront front(options, server.port());

    auto with_certificate = trusting(certs().ca);
    with_certificate.client_certificate_file = write_temp("client.pem", certs().client.cert_pem);
    with_certificate.client_key_file =
        write_temp("client-key.pem", mcp_test::tls::key_to_pem(certs().client.key.get(), "secret"));
    with_certificate.key_password = [] { return std::string("secret"); };
    greet(https_url(front.port()), with_tls(with_certificate));

    EXPECT_TRUE(greet_failure(https_url(front.port()), with_tls(trusting(certs().ca))).has_value());

    auto foreign = trusting(certs().ca);
    foreign.client_certificate_file = write_temp("foreign.pem", certs().foreign_client.cert_pem);
    foreign.client_key_file = write_temp("foreign-key.pem", certs().foreign_client.key_pem);
    EXPECT_TRUE(greet_failure(https_url(front.port()), with_tls(foreign)).has_value());
}

// ------------------------------------------------------------------------------------------------
// Certificates and servers that are refused

TEST(TlsClientTest, RefusesACertificateFromAnUntrustedAuthority) {
    const PlainServer server;
    const TlsFront front(front_for(certs().untrusted), server.port());
    try {
        greet(https_url(front.port()), with_tls(trusting(certs().ca)));
        FAIL() << "the connection succeeded";
    } catch (const std::exception& error) {
        EXPECT_EQ(tls_kind(error), TlsError::Kind::eUntrustedCertificate);
        const auto text = tls_text(error);
        EXPECT_NE(text.find("TLS to localhost failed: the server certificate is not trusted"),
                  std::string::npos)
            << text;
    }
}

TEST(TlsClientTest, TheSystemAuthoritiesDoNotTrustATestAuthority) {
    const PlainServer server;
    const TlsFront front(front_for(certs().server), server.port());
    EXPECT_EQ(greet_failure(https_url(front.port()), nullptr), TlsError::Kind::eUntrustedCertificate);
}

TEST(TlsClientTest, RefusesAnExpiredCertificate) {
    const PlainServer server;
    const TlsFront front(front_for(certs().expired), server.port());
    EXPECT_EQ(greet_failure(https_url(front.port()), with_tls(trusting(certs().ca))),
              TlsError::Kind::eExpiredCertificate);
}

TEST(TlsClientTest, RefusesACertificateForAnotherHost) {
    const PlainServer server;
    const TlsFront front(front_for(certs().other_name), server.port());
    EXPECT_EQ(greet_failure(https_url(front.port()), with_tls(trusting(certs().ca))),
              TlsError::Kind::eHostnameMismatch);
}

TEST(TlsClientTest, RefusesACertificateThatNamesItsHostOnlyInTheCommonName) {
    const PlainServer server;
    const TlsFront front(front_for(certs().common_name_only), server.port());
    EXPECT_EQ(greet_failure(https_url(front.port()), with_tls(trusting(certs().ca))),
              TlsError::Kind::eHostnameMismatch);
}

TEST(TlsClientTest, AnAddressUrlNeedsTheAddressInTheCertificate) {
    const PlainServer server;
    const TlsFront front(front_for(certs().dns_only), server.port());
    EXPECT_EQ(greet_failure(https_url(front.port(), "127.0.0.1"), with_tls(trusting(certs().ca))),
              TlsError::Kind::eHostnameMismatch);
}

TEST(TlsClientTest, APlainServerIsNotTls) {
    const PlainServer server;
    EXPECT_EQ(greet_failure(https_url(server.port()), with_tls(trusting(certs().ca))),
              TlsError::Kind::eNotTls);
}

TEST(TlsClientTest, NoVersionInCommon) {
    const PlainServer server;
    auto options = front_for(certs().server);
    options.max_version = TLS1_2_VERSION;
    const TlsFront front(options, server.port());
    auto client_options = trusting(certs().ca);
    client_options.min_version = mcp::TlsVersion::eTls13;
    EXPECT_EQ(greet_failure(https_url(front.port()), with_tls(client_options)),
              TlsError::Kind::eProtocolVersion);
}

// ------------------------------------------------------------------------------------------------
// Closing

TEST(TlsClientTest, AClientEndsItsConnectionsWithCloseNotify) {
    const PlainServer server;
    int close_notifies = 0;
    {
        const TlsFront front(front_for(certs().server), server.port());
        greet(https_url(front.port()), with_tls(trusting(certs().ca)), mcp::ClientProtocol::legacy());
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline && front.close_notifies() == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        close_notifies = front.close_notifies();
    }
    EXPECT_GE(close_notifies, 1);
}

// ------------------------------------------------------------------------------------------------
// Connection reuse

// A request goes on a connection an earlier answer came on, so further calls make no handshakes,
// and the client ends its kept connections with close_notify when it closes.
TEST(TlsClientTest, RequestsShareTheConnectionsOfEarlierOnes) {
    const PlainServer server;
    const TlsFront front(front_for(certs().server), server.port());
    for (const auto& protocol : {mcp::ClientProtocol::automatic(), mcp::ClientProtocol::legacy()}) {
        const bool automatic = protocol.kind() == mcp::ClientProtocol::automatic().kind();
        SCOPED_TRACE(automatic ? "automatic" : "legacy");
        const auto before = front.handshakes();
        const auto close_notifies = front.close_notifies();
        {
            mcp::ClientOptions options;
            options.protocol = protocol;
            mcp::HttpEndpointOptions endpoint_options;
            endpoint_options.configure = with_tls(trusting(certs().ca));
            mcp::Client client(
                mcp::Endpoint::http(https_url(front.port()), std::move(endpoint_options)), options);
            client.sync().connect("tls-test", "1.0");
            const auto greet_once = [&client] {
                EXPECT_FALSE(
                    client.sync().call_tool("greet", nlohmann::json::object()).isError.value_or(false));
            };
            greet_once();
            const auto after_first = front.handshakes();
            for (int call = 0; call < 4; ++call) {
                greet_once();
            }
            EXPECT_EQ(front.handshakes(), after_first);
            if (automatic) {
                // Discovery and every call went over one connection.
                EXPECT_EQ(after_first - before, 1);
            }
        }
        EXPECT_TRUE(eventually([&] { return front.close_notifies() > close_notifies; }));
    }
}

// The client checks a kept connection before it sends on it: one the server has closed is not used.
TEST(TlsClientTest, AConnectionTheServerClosedIsNotUsedAgain) {
    const PlainServer server;
    auto front_options = front_for(certs().server);
    front_options.close_after_response = true;
    const TlsFront front(front_options, server.port());
    mcp::HttpEndpointOptions endpoint_options;
    endpoint_options.configure = with_tls(trusting(certs().ca));
    mcp::Client client(mcp::Endpoint::http(https_url(front.port()), std::move(endpoint_options)));
    client.sync().connect("tls-test", "1.0");
    for (int call = 0; call < 3; ++call) {
        // The connection of the last answer is closed before the next request.
        ASSERT_TRUE(eventually([&] { return front.server_closes() == front.handshakes(); }));
        EXPECT_FALSE(
            client.sync().call_tool("greet", nlohmann::json::object()).isError.value_or(false));
    }
}

// ------------------------------------------------------------------------------------------------
// Settings

TEST(TlsClientTest, SettingsThatCannotBeUsed) {
    mcp::TlsClientOptions none;
    none.use_system_roots = false;
    EXPECT_THROW((void)mcp::TlsClientContext::create(none), mcp::TlsConfigError);

    mcp::TlsClientOptions not_pem;
    not_pem.ca_pem = {"not a certificate"};
    EXPECT_THROW((void)mcp::TlsClientContext::create(not_pem), mcp::TlsConfigError);

    mcp::TlsClientOptions missing;
    missing.ca_files = {std::filesystem::temp_directory_path() / "mcp-no-such-ca.pem"};
    EXPECT_THROW((void)mcp::TlsClientContext::create(missing), mcp::TlsConfigError);

    mcp::TlsClientOptions half;
    half.client_certificate_file = write_temp("half.pem", certs().client.cert_pem);
    EXPECT_THROW((void)mcp::TlsClientContext::create(half), mcp::TlsConfigError);

    mcp::TlsClientOptions mismatched;
    mismatched.client_certificate_file = write_temp("mismatch.pem", certs().client.cert_pem);
    mismatched.client_key_file = write_temp("mismatch-key.pem", certs().server.key_pem);
    try {
        (void)mcp::TlsClientContext::create(mismatched);
        FAIL() << "a key of another certificate was accepted";
    } catch (const mcp::TlsConfigError& error) {
        EXPECT_EQ(std::string(error.what()).find("mismatch-key"), std::string::npos) << error.what();
    }

    asio::io_context io;
    mcp::HttpClientTransport plain(io.get_executor(), "http://localhost:8080/mcp");
    EXPECT_THROW(plain.set_tls(mcp::TlsClientOptions{}), std::invalid_argument);
    EXPECT_THROW(plain.set_server_name("example.test"), std::invalid_argument);
    EXPECT_THROW(mcp::HttpClientTransport(io.get_executor(), "https://user:pw@example.test/mcp"),
                 std::invalid_argument);
    EXPECT_THROW(mcp::HttpClientTransport(io.get_executor(), "ftp://example.test/mcp"),
                 std::invalid_argument);
    mcp::ProxyOptions socks;
    socks.https_proxy = "socks5://127.0.0.1:1080";
    EXPECT_THROW(plain.set_proxy(socks), std::invalid_argument);

    for (const char* url : {"http://[::1/mcp", "http://[::1]x/mcp", "http://example.test:/mcp",
                            "http://[::1]:/mcp", "http:///mcp"}) {
        EXPECT_THROW(mcp::HttpClientTransport(io.get_executor(), url), std::invalid_argument) << url;
    }
    // A query or a fragment may follow the host without a path.
    EXPECT_NO_THROW(mcp::HttpClientTransport(io.get_executor(), "http://example.test?x=1"));
    EXPECT_NO_THROW(mcp::HttpClientTransport(io.get_executor(), "https://[::1]#top"));

    mcp::HttpClientTransport secure(io.get_executor(), "https://example.test/mcp");
    EXPECT_THROW(secure.set_server_name(""), std::invalid_argument);
    EXPECT_THROW(secure.set_server_name("."), std::invalid_argument);
    EXPECT_NO_THROW(secure.set_server_name("[::1]"));
}

TEST(TlsClientTest, RefusesClientCredentialsItCannotUse) {
    const auto with_credentials = [](const std::string& name, const std::string& certificate_pem,
                                     const std::string& key_pem) {
        mcp::TlsClientOptions options;
        options.client_certificate_file = write_temp(name + ".pem", certificate_pem);
        options.client_key_file = write_temp(name + "-key.pem", key_pem);
        return options;
    };
    EXPECT_THROW((void)mcp::TlsClientContext::create(
                     with_credentials("garbage", "not a certificate", certs().client.key_pem)),
                 mcp::TlsConfigError);
    // A key of another type loads beside the certificate, and the check of the pair refuses it.
    EXPECT_THROW((void)mcp::TlsClientContext::create(
                     with_credentials("other-type", certs().client.cert_pem, rsa_key_pem())),
                 mcp::TlsConfigError);

    const auto encrypted = mcp_test::tls::key_to_pem(certs().client.key.get(), "secret");
    auto right = with_credentials("encrypted", certs().client.cert_pem, encrypted);
    right.key_password = [] { return std::string("secret"); };
    EXPECT_NO_THROW((void)mcp::TlsClientContext::create(right));
    auto failing = right;
    failing.key_password = []() -> std::string { throw std::runtime_error("no passphrase"); };
    EXPECT_THROW((void)mcp::TlsClientContext::create(failing), mcp::TlsConfigError);
    auto oversized = right;
    oversized.key_password = [] { return std::string(64 * 1024, 'x'); };
    EXPECT_THROW((void)mcp::TlsClientContext::create(oversized), mcp::TlsConfigError);
}

// A ca_dirs entry is a directory of certificates named by their subject hash, as c_rehash makes.
TEST(TlsClientTest, TrustsAHashedCertificateDirectory) {
    const PlainServer server;
    const TlsFront front(front_for(certs().server), server.port());
    const auto directory = std::filesystem::temp_directory_path() / "mcp-tls-client-test" / "hashed";
    std::filesystem::create_directories(directory);
    std::array<char, 16> name{};
    std::snprintf(name.data(), name.size(), "%08lx.0", X509_subject_name_hash(certs().ca.cert.get()));
    std::ofstream(directory / name.data(), std::ios::binary) << certs().ca.cert_pem;

    mcp::TlsClientOptions options;
    options.use_system_roots = false;
    options.ca_dirs = {directory};
    greet(https_url(front.port()), with_tls(options));

    options.ca_dirs = {std::filesystem::temp_directory_path() / "mcp-tls-client-test" / "empty"};
    std::filesystem::create_directories(options.ca_dirs.front());
    EXPECT_EQ(greet_failure(https_url(front.port()), with_tls(options)),
              TlsError::Kind::eUntrustedCertificate);
}

// SSL_CERT_FILE and SSL_CERT_DIR name the authorities a client trusts by default, in place of the
// operating system's stores. Those are read once per process, so each case runs in a child process
// that re-executes the test binary.
TEST(TlsClientRootsDeathTest, TheEnvironmentNamesTheSystemAuthorities) {
    use_threadsafe_death_tests();
    const PlainServer server;
    const TlsFront front(front_for(certs().server), server.port());
    const auto url = https_url(front.port());
    const auto file = write_temp("environment-ca.pem", certs().ca.cert_pem);
    const auto hashed = file.parent_path() / "environment-hashed";
    std::filesystem::create_directories(hashed);
    std::array<char, 16> name{};
    std::snprintf(name.data(), name.size(), "%08lx.0", X509_subject_name_hash(certs().ca.cert.get()));
    std::ofstream(hashed / name.data(), std::ios::binary) << certs().ca.cert_pem;
    const auto unhashed = file.parent_path() / "environment-unhashed";
    std::filesystem::create_directories(unhashed);
    const auto missing = file.parent_path() / "environment-missing";
    const auto list = [](const std::vector<std::filesystem::path>& entries) {
#if defined(_WIN32)
        const char separator = ';';
#else
        const char separator = ':';
#endif
        std::string joined;
        for (const auto& entry : entries) {
            if (&entry != &entries.front()) {
                joined += separator;
            }
            joined += entry.string();
        }
        return joined;
    };
    const auto default_roots = [] { return with_tls(mcp::TlsClientOptions{}); };

    // The file's authorities are read; list entries that name no directory are skipped.
    EXPECT_EXIT(
        {
            set_variable(X509_get_default_cert_file_env(), file.string().c_str());
            set_variable(X509_get_default_cert_dir_env(), list({unhashed, "", missing, file}).c_str());
            std::exit(greets(url, default_roots()) ? 0 : 1);
        },
        ::testing::ExitedWithCode(0), "");

    // So are the hashed directories of the list.
    EXPECT_EXIT(
        {
            set_variable(X509_get_default_cert_file_env(), "");
            set_variable(X509_get_default_cert_dir_env(), list({missing, hashed}).c_str());
            std::exit(greets(url, default_roots()) ? 0 : 1);
        },
        ::testing::ExitedWithCode(0), "");

    // Variables that name no authority give none, so the server is not trusted. That read is not
    // kept: a later client reads them again.
    EXPECT_EXIT(
        {
            set_variable(X509_get_default_cert_file_env(), "");
            set_variable(X509_get_default_cert_dir_env(), missing.string().c_str());
            const bool refused =
                greet_failure(url, default_roots()) == TlsError::Kind::eUntrustedCertificate;
            set_variable(X509_get_default_cert_file_env(), file.string().c_str());
            std::exit(refused && greets(url, default_roots()) ? 0 : 1);
        },
        ::testing::ExitedWithCode(0), "");
}

// A host name written with a trailing dot is the same name; the certificate is checked, and the
// server name sent, without it.
TEST(TlsClientTest, AHostWithATrailingDotIsCheckedWithoutIt) {
    const PlainServer server;
    const TlsFront front(front_for(certs().server), server.port());
    greet(https_url(front.port(), "localhost."), with_tls(trusting(certs().ca)));
    ASSERT_FALSE(front.server_names().empty());
    for (const auto& name : front.server_names()) {
        EXPECT_EQ(name, "localhost");
    }
}

// ------------------------------------------------------------------------------------------------
// Proxies

TEST(TlsClientTest, ReachesAnHttpsServerThroughAProxyTunnel) {
    const PlainServer server({"mcp.example.test"});
    const TlsFront front(front_for(certs().server), server.port());
    const TestProxy proxy(front.port());
    mcp::ProxyOptions proxy_options;
    proxy_options.https_proxy = "http://user:p%40ss@127.0.0.1:" + std::to_string(proxy.port());
    const auto options = trusting(certs().ca);

    greet("https://mcp.example.test/mcp", [&](mcp::HttpClientTransport& transport) {
        transport.set_tls(options);
        transport.set_proxy(proxy_options);
    });

    // The requests of the client went through one tunnel.
    ASSERT_EQ(proxy.tunnels().size(), 1U);
    EXPECT_EQ(proxy.tunnels().front(), "mcp.example.test:443");
    for (const auto& authorization : proxy.authorizations()) {
        EXPECT_EQ(authorization, "Basic dXNlcjpwQHNz");  // user:p@ss
    }
    for (const auto& name : front.server_names()) {
        EXPECT_EQ(name, "mcp.example.test");
    }
    // The default port is left out of the Host header.
    for (const auto& host : front.hosts()) {
        EXPECT_EQ(host, "mcp.example.test");
    }
}

TEST(TlsClientTest, SendsPlainRequestsToTheProxyWithTheFullUrl) {
    const PlainServer server({"mcp.example.test"});
    const TestProxy proxy(server.port());
    mcp::ProxyOptions proxy_options;
    proxy_options.http_proxy = "127.0.0.1:" + std::to_string(proxy.port());

    greet("http://mcp.example.test:8080/mcp",
          [&](mcp::HttpClientTransport& transport) { transport.set_proxy(proxy_options); });

    ASSERT_GE(proxy.forwarded().size(), 2U);
    for (const auto& target : proxy.forwarded()) {
        EXPECT_EQ(target, "http://mcp.example.test:8080/mcp");
    }
    EXPECT_TRUE(proxy.tunnels().empty());
    // Over one connection to the proxy.
    EXPECT_EQ(proxy.connections(), 1);
}

// The proxy's credentials go with each request passed through it, and the URL's fragment, which
// names a part of a document, is never sent.
TEST(TlsClientTest, SendsProxyCredentialsAndNoFragmentToAPlainProxy) {
    const PlainServer server({"mcp.example.test"});
    const TestProxy proxy(server.port());
    mcp::ProxyOptions proxy_options;
    proxy_options.http_proxy = "http://alice:secret@127.0.0.1:" + std::to_string(proxy.port());

    greet("http://mcp.example.test:8080/mcp#part",
          [&](mcp::HttpClientTransport& transport) { transport.set_proxy(proxy_options); });

    ASSERT_FALSE(proxy.forwarded().empty());
    for (const auto& target : proxy.forwarded()) {
        EXPECT_EQ(target, "http://mcp.example.test:8080/mcp");
    }
    for (const auto& authorization : proxy.authorizations()) {
        EXPECT_EQ(authorization, "Basic YWxpY2U6c2VjcmV0");  // alice:secret
    }
}

TEST(TlsClientTest, LoopbackAndListedHostsBypassTheProxy) {
    const PlainServer server;
    const TlsFront front(front_for(certs().server), server.port());
    const TestProxy proxy(front.port());
    mcp::ProxyOptions proxy_options;
    proxy_options.https_proxy = "http://127.0.0.1:" + std::to_string(proxy.port());
    const auto options = trusting(certs().ca);

    greet(https_url(front.port()), [&](mcp::HttpClientTransport& transport) {
        transport.set_tls(options);
        transport.set_proxy(proxy_options);
    });
    EXPECT_EQ(proxy.connections(), 0);
}

// ------------------------------------------------------------------------------------------------
// OAuth

namespace {

/// Runs @p body, an OAuth request against a mock authorization server behind a TLS front.
struct OAuthFixture {
    explicit OAuthFixture(FrontOptions options, int requests = 1)
        : backend(io), front(std::move(options), backend_port()) {
        backend.set_handler([](const http::request<http::string_body>& request) {
            http::response<http::string_body> response{http::status::ok, request.version()};
            response.set(http::field::content_type, "application/json");
            response.body() = R"({"issuer":"https://as.example.test","path":")" +
                              std::string(request.target()) + R"("})";
            return response;
        });
        mcp_test::spawn_detached(
            io, [this, requests]() -> asio::awaitable<void> { co_await backend.serve(requests); });
    }

    [[nodiscard]] unsigned short backend_port() const {
        const auto url = backend.base_url();
        return static_cast<unsigned short>(std::stoi(url.substr(url.rfind(':') + 1)));
    }

    /// Fetches @p url with @p client configured by @p configure; returns the JSON or throws.
    nlohmann::json get(const std::string& url,
                       const std::function<void(mcp::auth::OAuthHttpClient&)>& configure) {
        mcp::auth::OAuthHttpClient client(io.get_executor());
        configure(client);
        nlohmann::json result;
        std::exception_ptr failure;
        mcp_test::spawn_detached(io, [&]() -> asio::awaitable<void> {
            try {
                result = co_await client.get_json(url);
            } catch (...) {
                failure = std::current_exception();
            }
            backend.close();
        });
        io.run();
        if (failure) {
            std::rethrow_exception(failure);
        }
        return result;
    }

    asio::io_context io;
    mcp_test::LoopbackServer backend;
    TlsFront front;
};

mcp::auth::MetadataFetchPolicy https_policy(const std::string& origin) {
    mcp::auth::MetadataFetchPolicy policy;
    policy.allowed_origins.push_back(origin);
    policy.allow_plain_http_loopback = true;
    return policy;
}

}  // namespace

TEST(TlsClientTest, OAuthFetchesFromAnHttpsAuthorizationServer) {
    OAuthFixture fixture(front_for(certs().server));
    const auto origin = "https://localhost:" + std::to_string(fixture.front.port());
    const auto context = mcp::TlsClientContext::create(trusting(certs().ca));

    const auto result = fixture.get(origin + "/.well-known/oauth-authorization-server",
                                    [&](mcp::auth::OAuthHttpClient& client) {
                                        client.configure(https_policy(origin), {}, context);
                                    });

    EXPECT_EQ(result.value("path", ""), "/.well-known/oauth-authorization-server");
    ASSERT_EQ(fixture.front.server_names().size(), 1U);
    EXPECT_EQ(fixture.front.server_names().front(), "localhost");
    EXPECT_EQ(fixture.front.hosts().front(), "localhost:" + std::to_string(fixture.front.port()));
}

TEST(TlsClientTest, OAuthChecksTheUrlHostNotThePinnedAddress) {
    OAuthFixture fixture(front_for(certs().other_name));
    const auto origin = "https://localhost:" + std::to_string(fixture.front.port());
    const auto context = mcp::TlsClientContext::create(trusting(certs().ca));
    try {
        (void)fixture.get(origin + "/metadata", [&](mcp::auth::OAuthHttpClient& client) {
            client.configure(https_policy(origin), {}, context);
        });
        FAIL() << "the fetch succeeded";
    } catch (const std::exception& error) {
        EXPECT_EQ(tls_kind(error), TlsError::Kind::eHostnameMismatch) << error.what();
    }
}

TEST(TlsClientTest, OAuthReportsABodyCutWithoutCloseNotify) {
    auto options = front_for(certs().server);
    options.cut_after_body_bytes = 10;
    OAuthFixture fixture(options);
    const auto origin = "https://localhost:" + std::to_string(fixture.front.port());
    const auto context = mcp::TlsClientContext::create(trusting(certs().ca));
    try {
        (void)fixture.get(origin + "/metadata", [&](mcp::auth::OAuthHttpClient& client) {
            client.configure(https_policy(origin), {}, context);
        });
        FAIL() << "the fetch succeeded";
    } catch (const std::exception& error) {
        EXPECT_EQ(tls_kind(error), TlsError::Kind::eTruncated) << error.what();
    }
}

TEST(TlsClientTest, OAuthTunnelsThroughAProxyByName) {
    OAuthFixture fixture(front_for(certs().server));
    const TestProxy proxy(fixture.front.port());
    const auto context = mcp::TlsClientContext::create(trusting(certs().ca));
    mcp::ProxyOptions proxy_options;
    proxy_options.https_proxy = "http://127.0.0.1:" + std::to_string(proxy.port());

    const auto result =
        fixture.get("https://as.example.test/metadata", [&](mcp::auth::OAuthHttpClient& client) {
            client.configure(https_policy("https://as.example.test"), {}, context, proxy_options);
        });

    EXPECT_EQ(result.value("path", ""), "/metadata");
    ASSERT_EQ(proxy.tunnels().size(), 1U);
    EXPECT_EQ(proxy.tunnels().front(), "as.example.test:443");
}

}  // namespace transport_tls_client_test

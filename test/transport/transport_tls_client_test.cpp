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

#include "../support/loopback_server.hpp"
#include "../support/spawn.hpp"
#include "../support/tls_test_support.hpp"

#include <atomic>
#include <boost/asio/io_context.hpp>
#include <chrono>
#include <cstdint>
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

    ASSERT_FALSE(proxy.tunnels().empty());
    for (const auto& tunnel : proxy.tunnels()) {
        EXPECT_EQ(tunnel, "mcp.example.test:443");
    }
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

    ASSERT_FALSE(proxy.forwarded().empty());
    for (const auto& target : proxy.forwarded()) {
        EXPECT_EQ(target, "http://mcp.example.test:8080/mcp");
    }
    EXPECT_TRUE(proxy.tunnels().empty());
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

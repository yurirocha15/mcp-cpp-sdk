// The proxy rules are internal, so the test program builds src/transport/proxy.cpp itself.
#include "../../src/detail/proxy.hpp"

#include "mcp/transport/proxy.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace proxy_rules_test {

namespace {

using mcp::detail::select_proxy;

mcp::ProxyOptions both(std::vector<std::string> no_proxy = {}) {
    mcp::ProxyOptions options;
    options.http_proxy = "http://proxy.example.test:3128";
    options.https_proxy = "http://proxy.example.test:3128";
    options.no_proxy = std::move(no_proxy);
    return options;
}

bool proxied(const mcp::ProxyOptions& options, const std::string& host,
             const std::string& port = "443") {
    return select_proxy(options, true, host, port).has_value();
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

/// Sets proxy variables for one test and clears them again.
class Environment {
   public:
    Environment() { clear(); }
    Environment(const Environment&) = delete;
    Environment& operator=(const Environment&) = delete;
    ~Environment() { clear(); }

    static void clear() {
        for (const char* name : {"http_proxy", "HTTP_PROXY", "https_proxy", "HTTPS_PROXY", "no_proxy",
                                 "NO_PROXY", "REQUEST_METHOD"}) {
            set_variable(name, "");
        }
    }
};

}  // namespace

TEST(ProxyRules, ParsesProxyUrls) {
    const auto route = mcp::detail::parse_proxy_url("http://alice:s%3Acret@proxy.test:8080", "test");
    EXPECT_EQ(route.host, "proxy.test");
    EXPECT_EQ(route.port, "8080");
    EXPECT_EQ(route.authorization, "Basic YWxpY2U6czpjcmV0");  // alice:s:cret

    const auto bare = mcp::detail::parse_proxy_url("proxy.test", "test");
    EXPECT_EQ(bare.port, "80");
    EXPECT_TRUE(bare.authorization.empty());

    const auto v6 = mcp::detail::parse_proxy_url("http://[fd00::1]:3128/", "test");
    EXPECT_EQ(v6.host, "fd00::1");
    EXPECT_EQ(v6.port, "3128");

    EXPECT_THROW((void)mcp::detail::parse_proxy_url("https://proxy.test", "test"),
                 std::invalid_argument);
    EXPECT_THROW((void)mcp::detail::parse_proxy_url("socks5://proxy.test", "test"),
                 std::invalid_argument);
    EXPECT_THROW((void)mcp::detail::parse_proxy_url("http://proxy.test:99999", "test"),
                 std::invalid_argument);
    EXPECT_THROW((void)mcp::detail::parse_proxy_url("http://:8080", "test"), std::invalid_argument);
    EXPECT_THROW((void)mcp::detail::parse_proxy_url("http://a:%zz@proxy.test", "test"),
                 std::invalid_argument);
}

TEST(ProxyRules, LoopbackHostsNeverUseTheProxy) {
    const auto options = both();
    for (const char* host : {"localhost", "LOCALHOST", "localhost.", "127.0.0.1", "127.9.9.9", "::1",
                             "::ffff:127.0.0.1"}) {
        EXPECT_FALSE(proxied(options, host)) << host;
    }
    EXPECT_TRUE(proxied(options, "example.test"));
    EXPECT_TRUE(proxied(options, "10.0.0.1"));
}

TEST(ProxyRules, TheBypassListMatchesHostsDomainsAddressesAndPorts) {
    const auto options = both({"internal.test", ".corp.test", "*.wild.test", "10.0.0.0/8",
                               "192.168.1.7", "fd00::/8", "pinned.test:8443", " spaced.test "});
    EXPECT_FALSE(proxied(options, "internal.test"));
    EXPECT_FALSE(proxied(options, "api.internal.test"));
    EXPECT_FALSE(proxied(options, "INTERNAL.TEST."));
    EXPECT_TRUE(proxied(options, "notinternal.test"));
    EXPECT_FALSE(proxied(options, "corp.test"));
    EXPECT_FALSE(proxied(options, "a.corp.test"));
    EXPECT_FALSE(proxied(options, "a.wild.test"));
    EXPECT_FALSE(proxied(options, "10.1.2.3"));
    EXPECT_TRUE(proxied(options, "11.1.2.3"));
    EXPECT_FALSE(proxied(options, "192.168.1.7"));
    EXPECT_TRUE(proxied(options, "192.168.1.8"));
    EXPECT_FALSE(proxied(options, "fd12::5"));
    EXPECT_FALSE(proxied(options, "pinned.test", "8443"));
    EXPECT_TRUE(proxied(options, "pinned.test", "443"));
    EXPECT_FALSE(proxied(options, "spaced.test"));
    EXPECT_FALSE(proxied(both({"*"}), "anything.test"));
}

TEST(ProxyRules, EachSchemeHasItsOwnProxy) {
    mcp::ProxyOptions options;
    options.https_proxy = "http://proxy.test:3128";
    EXPECT_TRUE(select_proxy(options, true, "example.test", "443").has_value());
    EXPECT_FALSE(select_proxy(options, false, "example.test", "80").has_value());
}

TEST(ProxyRules, AuthoritiesNameTheirPortUnlessItIsTheDefault) {
    EXPECT_EQ(mcp::detail::authority_of("example.test", "443", true), "example.test");
    EXPECT_EQ(mcp::detail::authority_of("example.test", "80", true), "example.test:80");
    EXPECT_EQ(mcp::detail::authority_of("example.test", "80", false), "example.test");
    EXPECT_EQ(mcp::detail::authority_of("::1", "8080", false), "[::1]:8080");
    EXPECT_EQ(mcp::detail::authority_of("::1", "443", true), "[::1]");
}

TEST(ProxyRules, ReadsTheEnvironment) {
    const Environment environment;
    set_variable("HTTPS_PROXY", "http://upper.test:1");
    set_variable("https_proxy", "http://lower.test:2");
    set_variable("HTTP_PROXY", "http://plain.test:3");
    set_variable("NO_PROXY", "a.test, b.test,,");
    auto options = mcp::ProxyOptions::from_environment();
    EXPECT_EQ(options.https_proxy, "http://lower.test:2");
    EXPECT_EQ(options.http_proxy, "http://plain.test:3");
    EXPECT_EQ(options.no_proxy, (std::vector<std::string>{"a.test", "b.test"}));

    // A CGI program's HTTP_PROXY may come from a request header.
    set_variable("REQUEST_METHOD", "GET");
    options = mcp::ProxyOptions::from_environment();
    EXPECT_TRUE(options.http_proxy.empty());
    EXPECT_EQ(options.https_proxy, "http://lower.test:2");
#if !defined(_WIN32)
    // The lower-case name stays, as no request header can set it; on Windows it is HTTP_PROXY.
    set_variable("http_proxy", "http://lower-plain.test:4");
    EXPECT_EQ(mcp::ProxyOptions::from_environment().http_proxy, "http://lower-plain.test:4");
#endif

    set_variable("https_proxy", "socks5://lower.test:2");
    EXPECT_THROW((void)mcp::ProxyOptions::from_environment(), std::invalid_argument);
}

}  // namespace proxy_rules_test

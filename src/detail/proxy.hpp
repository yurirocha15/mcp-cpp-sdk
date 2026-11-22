#pragma once

#include <mcp/transport/proxy.hpp>
#include <optional>
#include <string>

namespace mcp::detail {

/// Where a connection goes when it goes through a proxy.
struct ProxyRoute {
    std::string host;  ///< The proxy's host; an IPv6 literal without brackets.
    std::string port;
    /// The value of Proxy-Authorization, or empty to send none.
    std::string authorization;
};

/// Parses a proxy URL as ProxyOptions describes it. @p origin names where it came from in errors.
/// @throws std::invalid_argument For a URL that is malformed or names an unsupported proxy.
ProxyRoute parse_proxy_url(const std::string& url, const std::string& origin);

/// Checks every proxy URL of @p options. @throws std::invalid_argument As parse_proxy_url().
void validate_proxy_options(const ProxyOptions& options);

/// The proxy a connection to @p host (no brackets) on @p port goes through, or nullopt to connect
/// directly: no proxy for the scheme, a loopback host, or a host the bypass list names.
std::optional<ProxyRoute> select_proxy(const ProxyOptions& options, bool https, const std::string& host,
                                       const std::string& port);

/// The authority an HTTP request names for @p host on @p port: brackets around an IPv6 literal, and
/// the port unless it is the scheme's default.
std::string authority_of(const std::string& host, const std::string& port, bool https);

/// Whether @p host (no brackets) is `localhost` or a loopback address.
bool is_loopback_host(const std::string& host);

}  // namespace mcp::detail

#pragma once

#include <mcp/core/export.hpp>

#include <string>
#include <vector>

namespace mcp {

/**
 * @brief The HTTP proxy a client's connections go through.
 *
 * Clients connect directly unless they are given proxy options. A connection to an `https://`
 * server goes through the proxy as a CONNECT tunnel, so TLS runs between the client and the server
 * and the proxy sees neither the requests nor the certificate check. A request to an `http://`
 * server is sent to the proxy with the full URL as its target. Connections to `localhost` and to
 * loopback addresses never use a proxy.
 *
 * A proxy URL is `http://host:port`, or `http://user:password@host:port` for a proxy that asks for
 * Basic credentials (percent-encode reserved characters in them). Without a port it is 80, and
 * without a scheme `http://` is assumed. Proxies reached over TLS or SOCKS are not supported.
 */
struct ProxyOptions {
    /// The proxy for `http://` servers; empty connects directly.
    std::string http_proxy;
    /// The proxy for `https://` servers; empty connects directly.
    std::string https_proxy;
    /// Servers reached directly. An entry is a host name, which covers its subdomains as well (a
    /// leading dot is ignored), an IP address, a CIDR range such as `10.0.0.0/8`, any of these
    /// followed by `:port` to match that port only, or `*` for every server.
    std::vector<std::string> no_proxy;

    /**
     * @brief The proxies the environment names: `HTTP_PROXY`, `HTTPS_PROXY` and `NO_PROXY`, or
     * their lower-case forms, which win when both are set. `NO_PROXY` is a comma-separated list.
     *
     * `HTTP_PROXY` is ignored when `REQUEST_METHOD` is set, as it is for a CGI program, whose
     * environment a request header can fill.
     *
     * @throws std::invalid_argument When a variable names a proxy that is not supported.
     */
    [[nodiscard]] MCP_API static ProxyOptions from_environment();
};

}  // namespace mcp

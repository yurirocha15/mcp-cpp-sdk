#pragma once

#include "connection.hpp"
#include "network_failure.hpp"
#include "proxy.hpp"

#include <boost/asio/ip/tcp.hpp>

#include <mcp/core/async.hpp>
#include <mcp/transport/tls.hpp>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace mcp::detail {

/// How a client reaches one server: directly or through a proxy, in plain text or over TLS.
struct ConnectPlan {
    std::string host;  ///< The server's host; an IPv6 literal without brackets.
    std::string port;
    bool https{false};
    /// The name the server's certificate must carry (see tls_server_name()).
    std::string server_name;
    /// The TLS settings of an https connection.
    std::shared_ptr<const TlsClientContext> tls;
    std::optional<ProxyRoute> proxy;

    /// The host a connection resolves and connects to: the proxy's, or the server's.
    [[nodiscard]] const std::string& connect_host() const { return proxy ? proxy->host : host; }
    [[nodiscard]] const std::string& connect_port() const { return proxy ? proxy->port : port; }

    /// Whether requests go to the proxy itself, with the server's URL as their target, rather than
    /// through a tunnel: a plain-text server reached through a proxy.
    [[nodiscard]] bool forwards_requests() const { return proxy && !https; }

    /// The target of a request for @p path: the path, or the whole URL when the proxy forwards it.
    [[nodiscard]] std::string request_target(const std::string& path) const;
};

/// A connection object for @p plan, plain or TLS, not yet connected.
std::shared_ptr<Connection> make_connection(const boost::asio::any_io_executor& executor,
                                            const ConnectPlan& plan);

/// Connects @p connection to one of @p endpoints (the proxy's, or the server's), opens a CONNECT
/// tunnel through the proxy to an https server, and runs the TLS handshake. Bounded by the client
/// timeout as a whole.
/// @throws TlsError When the handshake fails.
/// @throws ConnectFailure With beast::error::timeout when the time runs out before the handshake
///         starts, or with the connect error.
/// @throws TransportError When the proxy refuses the tunnel.
Task<void> establish(std::shared_ptr<Connection> connection,
                     std::vector<boost::asio::ip::tcp::endpoint> endpoints,
                     std::shared_ptr<const ConnectPlan> plan);

}  // namespace mcp::detail

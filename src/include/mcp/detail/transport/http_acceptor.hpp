#pragma once

#include <mcp/transport/transport.hpp>

#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/socket_base.hpp>
#include <boost/system/error_code.hpp>

#include <string>

namespace mcp::detail {

/// Opens @p acceptor on @p host, an IP address, and @p port, with address reuse, and starts it
/// listening.
/// @throws TransportError When @p host is not an IP address or a step fails.
inline void open_http_acceptor(boost::asio::ip::tcp::acceptor& acceptor, const std::string& host,
                               unsigned short port) {
    boost::system::error_code ec;
    const auto bind_address = boost::asio::ip::make_address(host, ec);
    if (ec) {
        throw TransportError(TransportError::Kind::eConnect, "Invalid bind address: " + host);
    }

    const auto endpoint = boost::asio::ip::tcp::endpoint(bind_address, port);
    (void)acceptor.open(endpoint.protocol(), ec);
    if (ec) {
        throw TransportError(TransportError::Kind::eConnect,
                             "Failed to open HTTP acceptor: " + ec.message(), ec);
    }

    (void)acceptor.set_option(boost::asio::socket_base::reuse_address(true), ec);
    if (ec) {
        throw TransportError(TransportError::Kind::eConnect,
                             "Failed to set reuse_address: " + ec.message(), ec);
    }

    (void)acceptor.bind(endpoint, ec);
    if (ec) {
        throw TransportError(TransportError::Kind::eConnect,
                             "Failed to bind HTTP acceptor: " + ec.message(), ec);
    }

    (void)acceptor.listen(boost::asio::socket_base::max_listen_connections, ec);
    if (ec) {
        throw TransportError(TransportError::Kind::eConnect,
                             "Failed to listen on HTTP acceptor: " + ec.message(), ec);
    }
}

}  // namespace mcp::detail

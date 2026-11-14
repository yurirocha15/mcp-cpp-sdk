#pragma once

#include <boost/asio/detail/socket_option.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/socket_base.hpp>

#include <cstdint>

namespace mcp::detail {

/// What the socket says of the peer, without reading anything from it.
enum class PeerState : std::uint8_t {
    eConnected,  ///< Nothing has happened, or there is data still to read.
    eFinished,   ///< The peer closed its sending side, or all of the connection.
    eReset,      ///< The connection failed.
};

/// Asks the socket about its peer. A wait for a socket error ends on a reset under epoll, but kqueue
/// and the Windows reactor end it only for out-of-band data, so the socket is asked instead: for an
/// error it has recorded, and with a peek, which past the peer's end of stream reads end of file
/// until a reset. No other operation may be running on @p socket.
inline PeerState probe_peer(boost::asio::ip::tcp::socket& socket) {
    using PendingError = boost::asio::detail::socket_option::integer<SOL_SOCKET, SO_ERROR>;
    boost::system::error_code error;
    PendingError pending;
    socket.get_option(pending, error);
    if (!error && pending.value() != 0) {
        return PeerState::eReset;
    }
    const bool was_non_blocking = socket.non_blocking();
    socket.non_blocking(true, error);
    if (error) {
        return PeerState::eConnected;
    }
    char byte = 0;
    (void)socket.receive(boost::asio::buffer(&byte, 1), boost::asio::socket_base::message_peek, error);
    boost::system::error_code ignored;
    socket.non_blocking(was_non_blocking, ignored);
    if (error == boost::asio::error::eof) {
        return PeerState::eFinished;
    }
    return error && error != boost::asio::error::would_block ? PeerState::eReset
                                                             : PeerState::eConnected;
}

}  // namespace mcp::detail

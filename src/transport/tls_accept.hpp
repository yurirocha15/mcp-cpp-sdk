#pragma once

#include "../detail/net/connection.hpp"

#include <mcp/core/async.hpp>

#include <boost/asio/any_io_executor.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>

namespace mcp::detail {

/// The handshake limit of a listener: @p configured, or for zero a quarter of @p max_connections
/// (resolved), and at least one.
std::size_t resolved_max_pending_handshakes(std::size_t configured, std::size_t max_connections);

/// Takes one of @p limit handshake slots counted in @p taken, for a connection just accepted. False,
/// taking nothing, when all are taken.
bool take_handshake_slot(std::atomic<std::size_t>& taken, std::size_t limit) noexcept;

/// Runs the server side of the TLS handshake on @p connection, from @p executor, where its
/// operations run. The socket is closed when the handshake takes longer than @p timeout; zero, a
/// negative value or one too long for the clock means no limit.
/// @throws TlsError When the handshake fails or times out.
Task<void> accept_tls(Connection& connection, const boost::asio::any_io_executor& executor,
                      std::chrono::milliseconds timeout);

/// accept_tls() for a listener, which closes a connection whose handshake fails: false then. The
/// slot taken from @p taken is given back whichever way the handshake ends.
Task<bool> accept_tls_for_listener(Connection& connection, const boost::asio::any_io_executor& executor,
                                   std::chrono::milliseconds timeout, std::atomic<std::size_t>& taken);

}  // namespace mcp::detail

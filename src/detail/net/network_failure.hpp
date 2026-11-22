#pragma once

#include <mcp/core/export.hpp>
#include <mcp/transport/transport.hpp>

#include <boost/system/error_code.hpp>
#include <boost/system/system_error.hpp>

#include <exception>
#include <string>
#include <string_view>

namespace mcp::detail {

/// A connection that could not be set up: its name resolved, its socket connected or a proxy
/// tunnel opened. It stays a boost::system::system_error for the code between there and the
/// transport's boundary, which reports it as TransportError::Kind::eConnect.
class ConnectFailure : public boost::system::system_error {
   public:
    using boost::system::system_error::system_error;
};

/// The TransportError of @p transport_name for @p code, an error the network library reported.
/// @p kind is what was under way: eConnect while the connection was set up, eIo after. An
/// operation cancelled by close() is eClosed, and so, during eIo, is a connection the peer closed
/// or reset; a message larger than the transport's limit is eProtocol.
MCP_API TransportError network_failure(const boost::system::error_code& code, TransportError::Kind kind,
                                       std::string_view transport_name);

/// The TransportError of @p transport_name for @p failure when it is a
/// boost::system::system_error (eConnect for a ConnectFailure, else as network_failure() with
/// eIo); @p failure itself otherwise.
MCP_API std::exception_ptr as_transport_failure(const std::exception_ptr& failure,
                                                std::string_view transport_name);

}  // namespace mcp::detail

#pragma once

#include <mcp/transport/transport.hpp>

#include <boost/system/error_code.hpp>

#include <system_error>

namespace mcp_test {

/// Whether @p error is a TransportError of @p kind for @p code, an error the network library
/// reported. The codes are compared by value and message: the std::error_category a Boost error
/// converts to is not the same object in every Boost version.
inline bool is_network_error(const mcp::TransportError& error, mcp::TransportError::Kind kind,
                             const boost::system::error_code& code) {
    const std::error_code expected = code;
    return error.kind() == kind && error.code().value() == expected.value() &&
           error.code().message() == expected.message();
}

}  // namespace mcp_test

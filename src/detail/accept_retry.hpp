#pragma once

#include <boost/asio/error.hpp>
#include <boost/system/error_code.hpp>

#include <chrono>

namespace mcp::detail {

/// How long an accept loop waits after a failure that can pass.
inline constexpr auto g_accept_retry_delay = std::chrono::milliseconds(100);

/// Whether a failed accept says something about this moment rather than the listening socket: the
/// process or the system is out of descriptors or memory, or the connection died before it was
/// accepted. Ending the loop over one of these would leave a server that never accepts again.
inline bool accept_failure_can_pass(const boost::system::error_code& error) {
    namespace errors = boost::asio::error;
    return error == errors::no_descriptors || error == errors::no_buffer_space ||
           error == errors::no_memory || error == errors::connection_aborted ||
           error == errors::connection_reset ||
           error ==
               boost::system::errc::make_error_code(boost::system::errc::too_many_files_open_in_system);
}

}  // namespace mcp::detail

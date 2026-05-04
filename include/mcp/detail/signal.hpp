#pragma once

#include <mcp/constants.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/system/error_code.hpp>

#include <chrono>
#include <csignal>
#include <memory>

namespace mcp::detail {

/**
 * @brief Sends a termination signal to the current process in a cross-platform way.
 *
 * Uses std::raise(SIGINT) on all platforms. This is per-process — it does not
 * affect sibling processes.
 *
 * @important Call from the main thread for best cross-platform reliability.
 */
inline void trigger_shutdown_signal() { std::raise(SIGINT); }

}  // namespace mcp::detail

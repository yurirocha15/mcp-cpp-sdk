#pragma once

#include <mcp/constants.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/system/error_code.hpp>

#include <chrono>
#include <memory>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/types.h>
#include <unistd.h>
#include <csignal>
#endif

namespace mcp::detail {

/**
 * @brief Sends a termination signal to the current process in a cross-platform way.
 *
 * On Windows, it uses GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0) which is correctly
 * intercepted by boost::asio::signal_set.
 * On other platforms, it uses kill(getpid(), SIGTERM).
 */
inline void trigger_shutdown_signal() {
#ifdef _WIN32
    GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0);
#else
    ::kill(::getpid(), SIGTERM);
#endif
}

/**
 * @brief Performs a graceful shutdown of the IO context and transport.
 *
 * Closes the transport and starts a timer to force stop the io_context if it
 * doesn't terminate within the specified timeout.
 *
 * @return A shared pointer to the watchdog timer.
 */
template <typename TransportPtr>
auto graceful_shutdown(boost::asio::io_context& io_ctx, TransportPtr transport,
                       std::chrono::milliseconds timeout =
                           std::chrono::milliseconds(mcp::constants::g_shutdown_timeout_ms)) {
    transport->close();

    auto timer = std::make_shared<boost::asio::steady_timer>(io_ctx);
    timer->expires_after(timeout);
    timer->async_wait([&io_ctx, timer](const boost::system::error_code& ec) {
        if (!ec) {
            io_ctx.stop();
        }
    });
    return timer;
}

}  // namespace mcp::detail

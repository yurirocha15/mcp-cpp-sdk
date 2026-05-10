#pragma once

#include <mcp/detail/signal.hpp>

namespace mcp {

/**
 * @brief Performs a graceful shutdown of the IO context and transport.
 *
 * Closes the transport (preventing new connections) and starts a watchdog
 * timer. If the io_context doesn't terminate within the specified timeout,
 * it is forcefully stopped.
 *
 * @return A shared pointer to the watchdog timer. Hold or discard — the
 *         timer self-captures in its callback and auto-destructs on fire.
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

}  // namespace mcp

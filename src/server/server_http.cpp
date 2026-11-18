#include <mcp/core/async.hpp>
#include <mcp/core/signal.hpp>
#include <mcp/server/server.hpp>
#include <mcp/transport/http_session_manager.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/steady_timer.hpp>

#include <cstdint>
#include <exception>
#include <memory>
#include <string>
#include <utility>

namespace mcp {

void Server::run_http(const std::string& host, uint16_t port) {
    boost::asio::io_context io_ctx;
    const Executor executor(io_ctx.get_executor());

    // Each legacy session is served by a server of its own that shares this one's registrations.
    // A 2026-07-28 request belongs to no session, and this server answers it; the manager does not
    // own it, since it outlives the call.
    auto manager = std::make_shared<StreamableHttpSessionManager>(
        executor, host, port, [this](const Executor&) { return session_server(); });
    manager->set_modern_server(std::shared_ptr<Server>(std::shared_ptr<Server>(), this));

    std::shared_ptr<boost::asio::steady_timer> watchdog;
    boost::asio::signal_set signals(io_ctx, SIGINT, SIGTERM);
    signals.async_wait([manager, &io_ctx, &watchdog](const boost::system::error_code& ec, int) {
        if (ec == boost::asio::error::operation_aborted) {
            return;
        }
        watchdog = graceful_shutdown(io_ctx, manager);
    });

    std::exception_ptr ep;
    boost::asio::co_spawn(io_ctx, manager->listen(), [&](std::exception_ptr e) {
        ep = std::move(e);
        signals.cancel();
        if (watchdog) {
            watchdog->cancel();
        }
    });

    io_ctx.run();

    if (ep) {
        std::rethrow_exception(ep);
    }
}

}  // namespace mcp

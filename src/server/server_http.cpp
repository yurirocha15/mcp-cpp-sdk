#include "run_control.hpp"

#include <mcp/core/async.hpp>
#include <mcp/core/constants.hpp>
#include <mcp/server/server.hpp>
#include <mcp/transport/http_session_manager.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/signal_set.hpp>

#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace mcp {

void Server::run_http(const std::string& host, uint16_t port,
                      const std::function<void(StreamableHttpSessionManager&)>& configure) {
    boost::asio::io_context io_ctx;
    const Executor executor(io_ctx.get_executor());

    auto manager = std::make_shared<StreamableHttpSessionManager>(executor, host, port, *this);
    if (configure) {
        configure(*manager);
    }

    // SIGINT, SIGTERM and stop() end the run the same way, once: the manager stops listening and
    // closes its connections, and the loop runs until the handlers still running have finished, or
    // until the deadline stops it.
    detail::ShutdownDeadline deadline;
    const auto shut_down = [manager, &io_ctx, &deadline] {
        manager->close();
        deadline.start(io_ctx, std::chrono::milliseconds(constants::shutdown::g_TIMEOUT_MS));
    };
    boost::asio::signal_set signals(io_ctx, SIGINT, SIGTERM);
    signals.async_wait([&shut_down](const boost::system::error_code& ec, int) {
        if (ec == boost::asio::error::operation_aborted) {
            return;
        }
        shut_down();
    });
    const auto control = run_control();
    const auto registration =
        control->add([&io_ctx, shut_down] { boost::asio::post(io_ctx, shut_down); }, executor.get());
    const detail::RunControl::BoundPort bound(*control, manager->port());

    std::exception_ptr ep;
    boost::asio::co_spawn(io_ctx, manager->listen(), [&](std::exception_ptr e) {
        ep = std::move(e);
        signals.cancel();
        // The listener is gone, whether it was closed or failed; nothing new can arrive.
        shut_down();
    });

    io_ctx.run();
    deadline.finish();

    if (ep) {
        std::rethrow_exception(ep);
    }
}

}  // namespace mcp

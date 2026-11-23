#include "mcp/detail/server/run_control.hpp"

#include <mcp/core/async.hpp>
#include <mcp/core/signal.hpp>
#include <mcp/server/server.hpp>
#include <mcp/transport/stdio.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/steady_timer.hpp>

#include <exception>
#include <iostream>
#include <memory>
#include <utility>

namespace mcp {

void Server::run_stdio() { run_stdio(std::cin, std::cout); }

void Server::run_stdio(std::istream& input, std::ostream& output) {
    auto loop = std::make_unique<boost::asio::io_context>();
    std::exception_ptr ep;
    {
        // What the run uses is destroyed at the end of this block, before its loop, which
        // detail::destroy_loop() destroys.
        boost::asio::io_context& io_ctx = *loop;
        auto executor = io_ctx.get_executor();

        auto transport = std::make_shared<StdioTransport>(executor, input, output);

        // SIGINT, SIGTERM and stop() end the run the same way, once.
        std::shared_ptr<boost::asio::steady_timer> watchdog;
        const auto shut_down = [transport, &io_ctx, &watchdog] {
            if (!watchdog) {
                watchdog = graceful_shutdown(io_ctx, transport);
            }
        };
        boost::asio::signal_set signals(io_ctx, SIGINT, SIGTERM);
        signals.async_wait([&shut_down](const boost::system::error_code& ec, int) {
            if (ec == boost::asio::error::operation_aborted) {
                return;
            }
            shut_down();
        });
        const auto registration = run_control()->add(
            [&io_ctx, shut_down] { boost::asio::post(io_ctx, shut_down); }, executor);

        boost::asio::co_spawn(
            io_ctx,
            [this, transport, executor]() mutable -> boost::asio::awaitable<void> {
                co_await run(transport, executor);
            },
            [&](std::exception_ptr e) {
                ep = std::move(e);
                signals.cancel();
                if (watchdog) {
                    watchdog->cancel();
                }
            });

        io_ctx.run();
    }
    detail::destroy_loop(std::move(loop));

    if (ep) {
        std::rethrow_exception(ep);
    }
}

}  // namespace mcp

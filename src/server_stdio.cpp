#include <mcp/detail/signal.hpp>
#include <mcp/server.hpp>
#include <mcp/transport/stdio.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>

#include <exception>
#include <iostream>
#include <memory>
#include <utility>

namespace mcp {

void Server::run_stdio() { run_stdio(std::cin, std::cout); }

void Server::run_stdio(std::istream& input, std::ostream& output) {
    boost::asio::io_context io_ctx;
    auto executor = io_ctx.get_executor();

    auto transport = std::make_shared<StdioTransport>(executor, input, output);

    boost::asio::signal_set signals(io_ctx, SIGINT, SIGTERM);
    signals.async_wait([transport, &io_ctx](const boost::system::error_code& ec, int) {
        if (ec == boost::asio::error::operation_aborted) {
            return;
        }
        detail::graceful_shutdown(io_ctx, transport);
    });

    std::exception_ptr ep;
    boost::asio::co_spawn(
        io_ctx,
        [this, transport, executor]() mutable -> Task<void> { co_await run(transport, executor); },
        [&](std::exception_ptr e) {
            ep = std::move(e);
            signals.cancel();
        });

    io_ctx.run();

    if (ep) {
        std::rethrow_exception(ep);
    }
}

}  // namespace mcp

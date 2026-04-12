#include <mcp/server.hpp>
#include <mcp/transport/stdio.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>

#include <exception>
#include <iostream>
#include <memory>

namespace mcp {

void Server::run_stdio() { run_stdio(std::cin, std::cout); }

void Server::run_stdio(std::istream& input, std::ostream& output) {
    boost::asio::io_context io_ctx;
    auto executor = io_ctx.get_executor();

    auto transport = std::make_unique<StdioTransport>(executor, input, output);

    boost::asio::signal_set signals(io_ctx, SIGINT, SIGTERM);
    signals.async_wait([&](const boost::system::error_code&, int) { io_ctx.stop(); });

    std::exception_ptr ep;
    boost::asio::co_spawn(
        io_ctx,
        [this, t = std::move(transport), executor]() mutable -> Task<void> {
            co_await run(std::move(t), executor);
        },
        [&](std::exception_ptr e) {
            ep = e;
            signals.cancel();
        });

    io_ctx.run();

    if (ep) {
        std::rethrow_exception(ep);
    }
}

}  // namespace mcp

#include <mcp/server.hpp>
#include <mcp/transport/stdio.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>

#include <atomic>
#include <exception>
#include <iostream>
#include <memory>

namespace mcp {

void Server::run_stdio() { run_stdio(std::cin, std::cout); }

void Server::run_stdio(std::istream& input, std::ostream& output) {
    boost::asio::io_context io_ctx;
    auto executor = io_ctx.get_executor();

    auto transport = std::make_unique<StdioTransport>(executor, input, output);
    auto* transport_ptr = transport.get();
    auto transport_alive = std::make_shared<std::atomic<bool>>(true);

    boost::asio::signal_set signals(io_ctx, SIGINT, SIGTERM);
    signals.async_wait([transport_ptr, transport_alive](const boost::system::error_code& ec, int) {
        if (ec == boost::asio::error::operation_aborted) {
            return;
        }
        if (transport_alive->load(std::memory_order_acquire)) {
            transport_ptr->close();
        }
    });

    std::exception_ptr ep;
    boost::asio::co_spawn(
        io_ctx,
        [this, t = std::move(transport), executor, transport_alive]() mutable -> Task<void> {
            struct Guard {
                std::shared_ptr<std::atomic<bool>> alive;
                ~Guard() { alive->store(false, std::memory_order_release); }
            } guard{transport_alive};
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

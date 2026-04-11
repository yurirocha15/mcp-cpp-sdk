#include <mcp/server.hpp>
#include <mcp/transport/http_server.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>

#include <cstdint>
#include <exception>
#include <memory>
#include <string>

namespace mcp {

void Server::run_http(const std::string& host, uint16_t port) {
    boost::asio::io_context io_ctx;
    auto executor = io_ctx.get_executor();

    auto transport = std::make_unique<HttpServerTransport>(executor, host, port);
    auto* transport_ptr = transport.get();

    boost::asio::signal_set signals(io_ctx, SIGINT, SIGTERM);
    signals.async_wait([&](const boost::system::error_code&, int) {
        transport_ptr->close();
        io_ctx.stop();
    });

    boost::asio::co_spawn(io_ctx, transport_ptr->listen(), boost::asio::detached);

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

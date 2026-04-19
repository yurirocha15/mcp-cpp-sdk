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
#include <utility>

namespace mcp {

void Server::run_http(const std::string& host, uint16_t port) {
    boost::asio::io_context io_ctx;
    auto executor = io_ctx.get_executor();

    auto transport = std::make_shared<HttpServerTransport>(executor, host, port);

    boost::asio::signal_set signals(io_ctx, SIGINT, SIGTERM);
    signals.async_wait([transport](const boost::system::error_code& ec, int) {
        if (ec == boost::asio::error::operation_aborted) {
            return;
        }
        transport->close();
    });

    boost::asio::co_spawn(io_ctx, transport->listen(), boost::asio::detached);

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

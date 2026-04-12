#include <mcp/transport/websocket.hpp>

#include <atomic>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core/buffers_to_string.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/websocket/stream.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace mcp {

namespace beast = boost::beast;
namespace asio = boost::asio;
using WsStream = beast::websocket::stream<beast::tcp_stream>;

// ============================================================================
// WebSocketServerTransport::Impl
// ============================================================================

struct WebSocketServerTransport::Impl {
    WsStream ws;
    asio::strand<asio::any_io_executor> strand;
    std::atomic<bool> closed{false};
    bool handshake_done{false};

    explicit Impl(asio::ip::tcp::socket socket)
        : ws(std::move(socket)), strand(asio::make_strand(ws.get_executor())) {
        ws.text(true);
    }
};

WebSocketServerTransport::WebSocketServerTransport(asio::ip::tcp::socket socket)
    : impl_(std::make_unique<Impl>(std::move(socket))) {}

WebSocketServerTransport::~WebSocketServerTransport() {
    if (!impl_->closed.load(std::memory_order_acquire)) {
        close();
    }
}

Task<std::string> WebSocketServerTransport::read_message() {
    if (impl_->closed.load(std::memory_order_acquire)) {
        throw std::runtime_error("WebSocketServerTransport is closed");
    }

    if (!impl_->handshake_done) {
        co_await impl_->ws.async_accept(asio::use_awaitable);
        impl_->handshake_done = true;
    }

    beast::flat_buffer buffer;
    co_await impl_->ws.async_read(buffer, asio::use_awaitable);
    co_return beast::buffers_to_string(buffer.data());
}

Task<void> WebSocketServerTransport::write_message(std::string_view message) {
    if (impl_->closed.load(std::memory_order_acquire)) {
        throw std::runtime_error("WebSocketServerTransport is closed");
    }

    if (!impl_->handshake_done) {
        co_await impl_->ws.async_accept(asio::use_awaitable);
        impl_->handshake_done = true;
    }

    co_await impl_->ws.async_write(asio::buffer(message.data(), message.size()), asio::use_awaitable);
}

void WebSocketServerTransport::close() {
    if (impl_->closed.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    beast::error_code ec;
    auto& socket = impl_->ws.next_layer().socket();
    socket.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
    socket.close(ec);
}

// ============================================================================
// WebSocketClientTransport::Impl
// ============================================================================

struct WebSocketClientTransport::Impl {
    asio::strand<asio::any_io_executor> strand;
    asio::ip::tcp::resolver resolver;
    WsStream ws;
    std::string host;
    std::string port;
    std::string path;
    std::atomic<bool> closed{false};
    bool connected{false};

    Impl(const asio::any_io_executor& executor, std::string host_arg, std::string port_arg,
         std::string path_arg)
        : strand(asio::make_strand(executor)),
          resolver(strand),
          ws(strand),
          host(std::move(host_arg)),
          port(std::move(port_arg)),
          path(std::move(path_arg)) {
        ws.text(true);
    }

    Task<void> ensure_connected() {
        if (connected) {
            co_return;
        }

        auto results = co_await resolver.async_resolve(host, port, asio::use_awaitable);
        co_await ws.next_layer().async_connect(*results.begin(), asio::use_awaitable);
        co_await ws.async_handshake(host + ":" + port, path, asio::use_awaitable);
        connected = true;
    }
};

WebSocketClientTransport::WebSocketClientTransport(const asio::any_io_executor& executor,
                                                   std::string host, std::string port, std::string path)
    : impl_(std::make_unique<Impl>(executor, std::move(host), std::move(port), std::move(path))) {}

WebSocketClientTransport::~WebSocketClientTransport() {
    impl_->closed.store(true, std::memory_order_release);
    if (impl_->connected) {
        beast::error_code ec;
        auto& socket = impl_->ws.next_layer().socket();
        socket.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
        socket.close(ec);
    }
}

Task<std::string> WebSocketClientTransport::read_message() {
    if (impl_->closed.load(std::memory_order_acquire)) {
        throw std::runtime_error("WebSocketClientTransport is closed");
    }

    co_await impl_->ensure_connected();

    beast::flat_buffer buffer;
    co_await impl_->ws.async_read(buffer, asio::use_awaitable);
    co_return beast::buffers_to_string(buffer.data());
}

Task<void> WebSocketClientTransport::write_message(std::string_view message) {
    if (impl_->closed.load(std::memory_order_acquire)) {
        throw std::runtime_error("WebSocketClientTransport is closed");
    }

    co_await impl_->ensure_connected();

    co_await impl_->ws.async_write(asio::buffer(message.data(), message.size()), asio::use_awaitable);
}

void WebSocketClientTransport::close() {
    if (impl_->closed.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    if (!impl_->connected) {
        return;
    }

    beast::error_code ec;
    auto& socket = impl_->ws.next_layer().socket();
    socket.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
    socket.close(ec);
}

}  // namespace mcp

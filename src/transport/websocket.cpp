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
#include <chrono>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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
    try {
        if (!impl_->closed.load(std::memory_order_acquire)) {
            close();
        }
    } catch (...) {
        // Swallow exceptions in destructor to prevent std::terminate.
        (void)0;
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
    (void)socket.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
    (void)socket.close(ec);
}

// ============================================================================
// WebSocketClientTransport::Impl
// ============================================================================

struct WebSocketClientTransport::Impl {
    enum class ConnectionState {
        Disconnected,
        Connecting,
        Connected,
    };

    asio::strand<asio::any_io_executor> strand;
    asio::ip::tcp::resolver resolver;
    WsStream ws;
    std::string host;
    std::string port;
    std::string path;
    std::atomic<bool> closed{false};
    std::atomic<ConnectionState> connection_state{ConnectionState::Disconnected};
    std::exception_ptr connect_error;
    std::vector<std::shared_ptr<asio::steady_timer>> connection_waiters;

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

    void remove_connection_waiter(const std::shared_ptr<asio::steady_timer>& waiter) {
        for (auto it = connection_waiters.begin(); it != connection_waiters.end(); ++it) {
            if (*it == waiter) {
                connection_waiters.erase(it);
                break;
            }
        }
    }

    void notify_connection_waiters() {
        for (const auto& waiter : connection_waiters) {
            waiter->cancel();
        }
        connection_waiters.clear();
    }

    Task<void> wait_for_connection() {
        auto waiter = std::make_shared<asio::steady_timer>(strand);
        waiter->expires_at(std::chrono::steady_clock::time_point::max());
        connection_waiters.push_back(waiter);

        try {
            co_await waiter->async_wait(asio::use_awaitable);
        } catch (const boost::system::system_error& err) {
            remove_connection_waiter(waiter);
            if (err.code() != asio::error::operation_aborted) {
                throw;
            }
        }

        remove_connection_waiter(waiter);

        if (connection_state.load(std::memory_order_acquire) == ConnectionState::Connected) {
            co_return;
        }

        if (connect_error) {
            std::rethrow_exception(connect_error);
        }

        throw std::runtime_error("WebSocket connection did not complete");
    }

    Task<void> ensure_connected() {
        if (connection_state.load(std::memory_order_acquire) == ConnectionState::Connected) {
            co_return;
        }

        if (connection_state.load(std::memory_order_acquire) == ConnectionState::Connecting) {
            co_await wait_for_connection();
            co_return;
        }

        connection_state.store(ConnectionState::Connecting, std::memory_order_release);
        connect_error = nullptr;

        try {
            auto results = co_await resolver.async_resolve(host, port, asio::use_awaitable);
            co_await ws.next_layer().async_connect(*results.begin(), asio::use_awaitable);
            co_await ws.async_handshake(host + ":" + port, path, asio::use_awaitable);

            connection_state.store(ConnectionState::Connected, std::memory_order_release);
            notify_connection_waiters();
        } catch (...) {
            connect_error = std::current_exception();
            connection_state.store(ConnectionState::Disconnected, std::memory_order_release);
            notify_connection_waiters();
            std::rethrow_exception(connect_error);
        }
    }
};

WebSocketClientTransport::WebSocketClientTransport(const asio::any_io_executor& executor,
                                                   std::string host, std::string port, std::string path)
    : impl_(std::make_unique<Impl>(executor, std::move(host), std::move(port), std::move(path))) {}

WebSocketClientTransport::~WebSocketClientTransport() {
    try {
        impl_->closed.store(true, std::memory_order_release);
        impl_->connection_state.store(Impl::ConnectionState::Disconnected, std::memory_order_release);
        impl_->notify_connection_waiters();
        if (impl_->ws.next_layer().socket().is_open()) {
            beast::error_code ec;
            auto& socket = impl_->ws.next_layer().socket();
            (void)socket.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
            (void)socket.close(ec);
        }
    } catch (...) {
        // Swallow exceptions in destructor to prevent std::terminate.
        (void)0;
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

    impl_->connect_error =
        std::make_exception_ptr(std::runtime_error("WebSocketClientTransport is closed"));
    impl_->connection_state.store(Impl::ConnectionState::Disconnected, std::memory_order_release);
    impl_->notify_connection_waiters();

    if (!impl_->ws.next_layer().socket().is_open()) {
        return;
    }

    beast::error_code ec;
    auto& socket = impl_->ws.next_layer().socket();
    (void)socket.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
    (void)socket.close(ec);
}

}  // namespace mcp

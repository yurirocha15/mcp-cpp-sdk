#pragma once

#include <mcp/transport.hpp>

#include <atomic>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core/buffers_to_string.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/websocket/stream.hpp>
#include <string>
#include <string_view>
#include <utility>

namespace mcp {

/**
 * @brief Transport implementation for WebSocket communication using Boost.Beast.
 *
 * @details Wraps a `boost::beast::websocket::stream<boost::beast::tcp_stream>`
 * that must already be connected and handshake-completed before construction.
 * Reads complete text frames via `async_read` and writes text messages via
 * `async_write`, both using `boost::asio::use_awaitable` for C++20 coroutine
 * integration.
 */
class WebSocketTransport final : public ITransport {
   public:
    /// @brief Concrete Boost.Beast WebSocket stream type wrapped by this transport.
    using WsStream = boost::beast::websocket::stream<boost::beast::tcp_stream>;

    /**
     * @brief Construct a WebSocketTransport from an already-connected WebSocket stream.
     *
     * @param ws A connected and handshake-completed WebSocket stream.
     *           Ownership is transferred to this transport.
     */
    explicit WebSocketTransport(WsStream ws)
        : ws_(std::move(ws)), strand_(boost::asio::make_strand(ws_.get_executor())) {
        ws_.text(true);
    }

    ~WebSocketTransport() override {
        if (!closed_.load(std::memory_order_acquire)) {
            close();
        }
    }

    WebSocketTransport(const WebSocketTransport&) = delete;
    WebSocketTransport& operator=(const WebSocketTransport&) = delete;
    WebSocketTransport(WebSocketTransport&&) = delete;
    WebSocketTransport& operator=(WebSocketTransport&&) = delete;

    /**
     * @brief Reads a complete text message from the WebSocket connection.
     *
     * @return A task that resolves to the received message as a string.
     * @throws std::runtime_error If the transport is closed.
     */
    Task<std::string> read_message() override {
        if (closed_.load(std::memory_order_acquire)) {
            throw std::runtime_error("WebSocketTransport is closed");
        }

        boost::beast::flat_buffer buffer;
        co_await ws_.async_read(buffer, boost::asio::use_awaitable);

        co_return boost::beast::buffers_to_string(buffer.data());
    }

    /**
     * @brief Writes a message to the WebSocket connection.
     *
     * @param message The message content to write.
     * @return A task that completes when the message is written.
     * @throws std::runtime_error If the transport is closed.
     */
    Task<void> write_message(std::string_view message) override {
        if (closed_.load(std::memory_order_acquire)) {
            throw std::runtime_error("WebSocketTransport is closed");
        }

        co_await ws_.async_write(boost::asio::buffer(message.data(), message.size()),
                                 boost::asio::use_awaitable);
    }

    /**
     * @brief Closes the WebSocket connection and underlying TCP socket.
     */
    void close() override {
        if (closed_.exchange(true, std::memory_order_acq_rel)) {
            return;
        }

        boost::beast::error_code ec;
        auto& socket = ws_.next_layer().socket();
        [[maybe_unused]] auto shutdown_ec =
            socket.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
        [[maybe_unused]] auto close_ec = socket.close(ec);
    }

   private:
    WsStream ws_;
    boost::asio::strand<boost::asio::any_io_executor> strand_;
    std::atomic<bool> closed_{false};
};

}  // namespace mcp

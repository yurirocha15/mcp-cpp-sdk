#pragma once

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/system/error_code.hpp>

#include <chrono>
#include <memory>

namespace mcp::detail {

/// A deadline on a run of socket operations that closes the socket when it passes, used in place
/// of beast::tcp_stream::expires_after().
///
/// Beast 1.86 completes a tcp_stream operation that starts after the stream's deadline by
/// dispatching a moved-from handler, which throws from a noexcept function and ends the process.
/// An HTTP exchange or a WebSocket handshake is several such operations in a row, so a deadline set
/// on the stream can pass between two of them. Closing the socket instead fails whichever operation
/// is pending or comes next, the same way close() does.
///
/// Create and destroy it on the executor the socket's operations run on, which is also the one
/// given here: the timer's handler runs there, so it never closes the socket concurrently with an
/// operation, and once the deadline is destroyed a firing already queued leaves the socket alone.
class SocketDeadline {
   public:
    SocketDeadline(const boost::asio::any_io_executor& executor, boost::asio::ip::tcp::socket& socket,
                   std::chrono::steady_clock::duration timeout)
        : state_(std::make_shared<State>(executor, socket)) {
        state_->timer.expires_after(timeout);
        state_->timer.async_wait([state = state_](const boost::system::error_code& error) {
            if (error || state->socket == nullptr) {
                return;
            }
            state->expired = true;
            boost::system::error_code ignored;
            state->socket->close(ignored);
        });
    }

    SocketDeadline(const SocketDeadline&) = delete;
    SocketDeadline& operator=(const SocketDeadline&) = delete;
    SocketDeadline(SocketDeadline&&) = delete;
    SocketDeadline& operator=(SocketDeadline&&) = delete;

    ~SocketDeadline() {
        state_->socket = nullptr;
        state_->timer.cancel();
    }

    /// Did the deadline pass and close the socket?
    [[nodiscard]] bool expired() const noexcept { return state_->expired; }

   private:
    struct State {
        State(const boost::asio::any_io_executor& executor, boost::asio::ip::tcp::socket& socket_arg)
            : timer(executor), socket(&socket_arg) {}

        boost::asio::steady_timer timer;
        boost::asio::ip::tcp::socket* socket;
        bool expired{false};
    };

    std::shared_ptr<State> state_;
};

}  // namespace mcp::detail

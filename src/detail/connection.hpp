#pragma once

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/system/error_code.hpp>

#include <cstddef>
#include <utility>
#include <variant>

namespace mcp::detail {

/// A connection that is plain TCP or TLS over TCP, chosen when it is made. It is a stream Beast
/// reads and writes HTTP on, whichever it is. The TCP layer stays reachable for what works on the
/// socket itself: connecting, deadlines that close it, probes.
class Connection {
   public:
    using executor_type = boost::asio::any_io_executor;
    using tcp_type = boost::beast::tcp_stream;
    using tls_type = boost::asio::ssl::stream<boost::beast::tcp_stream>;

    /// A plain connection.
    explicit Connection(const executor_type& executor) : stream_(std::in_place_index<0>, executor) {}

    /// A TLS connection on @p context, which must outlive the handshake (each stream holds a
    /// reference on the OpenSSL context, so the stream may outlive the Asio object).
    Connection(const executor_type& executor, boost::asio::ssl::context& context)
        : stream_(std::in_place_index<1>, executor, context) {}

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    Connection(Connection&&) = delete;
    Connection& operator=(Connection&&) = delete;
    ~Connection() = default;

    [[nodiscard]] executor_type get_executor() noexcept { return tcp().get_executor(); }

    /// The TCP layer: the whole stream of a plain connection, the transport under a TLS one.
    [[nodiscard]] tcp_type& tcp() noexcept {
        auto* tls = std::get_if<1>(&stream_);
        return tls != nullptr ? tls->next_layer() : std::get<0>(stream_);
    }

    [[nodiscard]] boost::asio::ip::tcp::socket& socket() noexcept { return tcp().socket(); }

    /// The TLS stream, or null for a plain connection.
    [[nodiscard]] tls_type* tls() noexcept { return std::get_if<1>(&stream_); }

    [[nodiscard]] bool is_tls() const noexcept { return stream_.index() == 1; }

    /// Whether the TLS handshake has finished; always true for a plain connection.
    [[nodiscard]] bool established() const noexcept { return !is_tls() || established_; }
    void mark_established() noexcept { established_ = true; }

    template <typename MutableBufferSequence, typename ReadToken>
    auto async_read_some(const MutableBufferSequence& buffers, ReadToken&& token) {
        return boost::asio::async_initiate<ReadToken, void(boost::system::error_code, std::size_t)>(
            InitiateRead{this}, token, buffers);
    }

    template <typename ConstBufferSequence, typename WriteToken>
    auto async_write_some(const ConstBufferSequence& buffers, WriteToken&& token) {
        return boost::asio::async_initiate<WriteToken, void(boost::system::error_code, std::size_t)>(
            InitiateWrite{this}, token, buffers);
    }

    /// A synchronous read, for checks on an idle connection with the socket in non-blocking mode.
    template <typename MutableBufferSequence>
    std::size_t read_some(const MutableBufferSequence& buffers, boost::system::error_code& error) {
        if (auto* tls = std::get_if<1>(&stream_)) {
            return tls->read_some(buffers, error);
        }
        return std::get<0>(stream_).read_some(buffers, error);
    }

   private:
    struct InitiateRead {
        Connection* self;
        template <typename Handler, typename Buffers>
        void operator()(Handler&& handler, const Buffers& buffers) const {
            if (auto* tls = std::get_if<1>(&self->stream_)) {
                tls->async_read_some(buffers, std::forward<Handler>(handler));
            } else {
                std::get<0>(self->stream_).async_read_some(buffers, std::forward<Handler>(handler));
            }
        }
    };

    struct InitiateWrite {
        Connection* self;
        template <typename Handler, typename Buffers>
        void operator()(Handler&& handler, const Buffers& buffers) const {
            if (auto* tls = std::get_if<1>(&self->stream_)) {
                tls->async_write_some(buffers, std::forward<Handler>(handler));
            } else {
                std::get<0>(self->stream_).async_write_some(buffers, std::forward<Handler>(handler));
            }
        }
    };

    std::variant<tcp_type, tls_type> stream_;
    bool established_{false};
};

/// Whether the peer has closed an idle connection, or sent something no request asked for. A
/// server closes a connection that sits idle between requests, and a request written into it is
/// never read. Over TLS the records that may arrive while idle, such as session tickets, are read
/// and dropped; a close_notify or any application data means the connection is not used again.
/// No other operation may be running on @p connection.
inline bool closed_by_peer(Connection& connection) {
    auto& socket = connection.socket();
    if (!socket.is_open()) {
        return true;
    }
    boost::system::error_code error;
    const bool was_non_blocking = socket.non_blocking();
    socket.non_blocking(true, error);
    if (error) {
        return false;
    }
    char byte = 0;
    const auto read = connection.read_some(boost::asio::buffer(&byte, 1), error);
    boost::system::error_code restore_error;
    socket.non_blocking(was_non_blocking, restore_error);
    return read != 0 || error != boost::asio::error::would_block;
}

/// Closes @p connection. A TLS connection whose handshake finished sends its close_notify first,
/// without waiting for the peer's. No other operation may be running on it; to end one that is,
/// close the socket instead.
inline void close_connection(Connection& connection) noexcept {
    auto& socket = connection.socket();
    if (!socket.is_open()) {
        return;
    }
    boost::system::error_code ignored;
    if (auto* tls = connection.tls(); tls != nullptr && connection.established()) {
        // Non-blocking, the shutdown writes the alert and stops where it would wait for the
        // peer's.
        socket.non_blocking(true, ignored);
        tls->shutdown(ignored);
    }
    socket.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ignored);
    socket.close(ignored);
}

}  // namespace mcp::detail

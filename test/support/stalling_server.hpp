#pragma once

#include <atomic>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/system/error_code.hpp>
#include <optional>
#include <utility>

/// Accepts one connection and holds it open without ever reading or writing on it: a peer that
/// stalls, which `close()` must be able to get away from without waiting for a timeout.
class StallingServer final {
   public:
    explicit StallingServer(boost::asio::io_context& io_ctx)
        : acceptor_(io_ctx, {boost::asio::ip::make_address("127.0.0.1"), 0}) {}

    [[nodiscard]] unsigned short port() const { return acceptor_.local_endpoint().port(); }

    /// Start accepting; the accepted socket is held as a member so the connection stays open (no
    /// FIN, no RST) until this server is destroyed.
    void accept_and_stall() {
        acceptor_.async_accept(
            [this](boost::system::error_code error, boost::asio::ip::tcp::socket socket) {
                if (!error) {
                    held_socket_ = std::move(socket);
                    accepted_.fetch_add(1);
                }
            });
    }

    /// Safe to poll from a thread other than the one running the io_context.
    [[nodiscard]] int accepted() const { return accepted_.load(); }

   private:
    boost::asio::ip::tcp::acceptor acceptor_;
    std::optional<boost::asio::ip::tcp::socket> held_socket_;
    std::atomic<int> accepted_{0};
};

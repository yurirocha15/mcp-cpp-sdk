#include "tls_accept.hpp"

#include "mcp/detail/net/socket_deadline.hpp"
#include "mcp/detail/net/tls.hpp"

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/system_error.hpp>

#include <algorithm>
#include <exception>
#include <optional>
#include <string>

namespace mcp::detail {

namespace net = boost::asio;

std::size_t resolved_max_pending_handshakes(std::size_t configured, std::size_t max_connections) {
    if (configured != 0) {
        return configured;
    }
    return std::max<std::size_t>(max_connections / 4, 1);
}

bool take_handshake_slot(std::atomic<std::size_t>& taken, std::size_t limit) noexcept {
    auto current = taken.load(std::memory_order_relaxed);
    do {
        if (current >= limit) {
            return false;
        }
    } while (!taken.compare_exchange_weak(current, current + 1, std::memory_order_relaxed));
    return true;
}

Task<void> accept_tls(Connection& connection, const net::any_io_executor& executor,
                      std::chrono::milliseconds timeout) {
    auto* tls = connection.tls();
    // The timer counts in a finer unit, so a limit past half its range would overflow.
    constexpr auto longest =
        std::chrono::duration_cast<std::chrono::milliseconds>(net::steady_timer::duration::max() / 2);
    std::optional<SocketDeadline> deadline;
    if (timeout > std::chrono::milliseconds::zero() && timeout <= longest) {
        deadline.emplace(executor, connection.socket(), timeout);
    }
    std::exception_ptr failure;
    boost::system::error_code error;
    try {
        co_await tls->async_handshake(net::ssl::stream_base::server, net::use_awaitable);
    } catch (const boost::system::system_error& handshake_error) {
        error = handshake_error.code();
        failure = std::current_exception();
    }
    if (failure) {
        const bool timed_out = deadline && deadline->expired();
        boost::system::error_code endpoint_error;
        const auto peer = connection.socket().remote_endpoint(endpoint_error);
        throw server_handshake_failure(
            error, tls->native_handle(),
            endpoint_error ? std::string("a client") : peer.address().to_string(), timed_out);
    }
    connection.mark_established();
}

Task<bool> accept_tls_for_listener(Connection& connection, const net::any_io_executor& executor,
                                   std::chrono::milliseconds timeout, std::atomic<std::size_t>& taken) {
    class Slot {
       public:
        explicit Slot(std::atomic<std::size_t>& taken_arg) : taken_(taken_arg) {}
        Slot(const Slot&) = delete;
        Slot& operator=(const Slot&) = delete;
        Slot(Slot&&) = delete;
        Slot& operator=(Slot&&) = delete;
        ~Slot() { taken_.fetch_sub(1, std::memory_order_relaxed); }

       private:
        std::atomic<std::size_t>& taken_;
    };
    const Slot slot(taken);
    bool accepted = true;
    try {
        co_await accept_tls(connection, executor, timeout);
    } catch (const TlsError&) {
        accepted = false;
    }
    if (!accepted) {
        close_connection(connection);
    }
    co_return accepted;
}

}  // namespace mcp::detail

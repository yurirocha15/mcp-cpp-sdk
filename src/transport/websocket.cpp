#include <mcp/transport/websocket.hpp>

#include "../detail/socket_deadline.hpp"

#include <algorithm>
#include <atomic>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core/buffers_to_string.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/http/read.hpp>
#include <boost/beast/http/string_body.hpp>
#include <boost/beast/http/write.hpp>
#include <boost/beast/websocket/stream.hpp>
#include <boost/system/system_error.hpp>
#include <chrono>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace mcp {

namespace beast = boost::beast;
namespace asio = boost::asio;
using WsStream = beast::websocket::stream<beast::tcp_stream>;

namespace {

// The timer counts in its own, finer duration, so a limit beyond half its range -- about 146 years
// -- would overflow on conversion or when added to the current time. Such a value, including
// milliseconds::max(), can only mean "no limit", so it becomes zero, which is how no limit is
// spelled here.
std::chrono::milliseconds effective_connect_timeout(std::chrono::milliseconds requested) {
    constexpr auto longest =
        std::chrono::duration_cast<std::chrono::milliseconds>(asio::steady_timer::duration::max() / 2);
    return requested > longest ? std::chrono::milliseconds::zero() : requested;
}

constexpr auto kNever = std::chrono::steady_clock::time_point::max();

struct OperationWaiter {
    explicit OperationWaiter(const asio::any_io_executor& executor) : signal(executor) {
        signal.expires_at(kNever);
    }

    void wake() noexcept { signal.cancel(); }

    asio::steady_timer signal;
    bool granted{false};
};

template <typename WaiterContainer>
void wake_all(WaiterContainer& waiters) noexcept {
    for (const auto& waiter : waiters) {
        waiter->wake();
    }
    waiters.clear();
}

template <typename WaiterContainer>
void remove_waiter(WaiterContainer& waiters, const std::shared_ptr<OperationWaiter>& waiter) noexcept {
    auto position = std::find(waiters.begin(), waiters.end(), waiter);
    if (position != waiters.end()) {
        waiters.erase(position);
    }
}

void require_open(const std::atomic<bool>& closed, std::string_view transport_name) {
    if (closed.load(std::memory_order_acquire)) {
        throw std::runtime_error(std::string(transport_name) + " is closed");
    }
}

std::exception_ptr closed_error(std::string_view transport_name) {
    return std::make_exception_ptr(std::runtime_error(std::string(transport_name) + " is closed"));
}

void close_socket(WsStream& ws) noexcept {
    beast::error_code ignored;
    auto& socket = ws.next_layer().socket();
    socket.cancel(ignored);
    socket.shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
    socket.close(ignored);
}

class ReadReservation {
   public:
    explicit ReadReservation(bool& active) : active_(active) { active_ = true; }
    ~ReadReservation() { active_ = false; }

    ReadReservation(const ReadReservation&) = delete;
    ReadReservation& operator=(const ReadReservation&) = delete;

   private:
    bool& active_;
};

class SerializedWriteGate {
   public:
    SerializedWriteGate(const asio::any_io_executor& executor, const std::atomic<bool>& closed,
                        std::string_view transport_name)
        : executor_(executor), closed_(closed), transport_name_(transport_name) {}

    Task<void> acquire() {
        require_open(closed_, transport_name_);
        if (!active_) {
            active_ = true;
            co_return;
        }

        auto waiter = std::make_shared<OperationWaiter>(executor_);
        waiters_.push_back(waiter);
        boost::system::error_code error;
        co_await waiter->signal.async_wait(asio::redirect_error(asio::use_awaitable, error));
        if (!waiter->granted) {
            remove_waiter(waiters_, waiter);
        }
        if (error && error != asio::error::operation_aborted) {
            if (waiter->granted) {
                release();
            }
            throw boost::system::system_error(error);
        }
        if (!waiter->granted) {
            require_open(closed_, transport_name_);
            throw std::runtime_error(transport_name_ + " write was interrupted");
        }
        if (closed_.load(std::memory_order_acquire)) {
            release();
            require_open(closed_, transport_name_);
        }
    }

    void release() noexcept {
        if (closed_.load(std::memory_order_acquire) || waiters_.empty()) {
            active_ = false;
            return;
        }

        auto waiter = std::move(waiters_.front());
        waiters_.pop_front();
        waiter->granted = true;
        waiter->wake();
    }

    void cancel() noexcept { wake_all(waiters_); }

   private:
    asio::any_io_executor executor_;
    const std::atomic<bool>& closed_;
    std::string transport_name_;
    std::deque<std::shared_ptr<OperationWaiter>> waiters_;
    bool active_{false};
};

}  // namespace

// ============================================================================
// WebSocketServerTransport::Impl
// ============================================================================

struct WebSocketServerTransport::Impl {
    enum class HandshakeState {
        Pending,
        Accepting,
        Open,
        Failed,
    };

    WsStream ws;
    asio::strand<asio::any_io_executor> strand;
    std::atomic<bool> closed{false};
    HandshakeState handshake_state{HandshakeState::Pending};
    std::exception_ptr handshake_error;
    std::vector<std::shared_ptr<OperationWaiter>> handshake_waiters;
    SerializedWriteGate write_gate;
    bool read_active{false};

    mutable std::mutex configuration_mutex;
    std::unordered_set<std::string> allowed_origins;
    bool allow_all_origins{false};
    /// Set once the handshake starts; the Origin policy is fixed from then on.
    bool configuration_locked{false};

    explicit Impl(asio::ip::tcp::socket socket)
        : ws(std::move(socket)),
          strand(asio::make_strand(ws.get_executor())),
          write_gate(strand, closed, "WebSocketServerTransport") {
        ws.text(true);
    }

    static void throw_if_closed(const std::shared_ptr<Impl>& state) {
        require_open(state->closed, "WebSocketServerTransport");
    }

    void ensure_configurable() const {
        if (configuration_locked || closed.load(std::memory_order_acquire)) {
            throw std::logic_error(
                "WebSocketServerTransport origins must be set before the first read or write");
        }
    }

    /// Fix the Origin policy for the handshake and report whether an upgrade carrying `origin`,
    /// or none when it is empty, may open the connection.
    bool lock_configuration_and_allow(const std::optional<std::string>& origin) {
        std::lock_guard lock(configuration_mutex);
        configuration_locked = true;
        return !origin || allow_all_origins || allowed_origins.contains(*origin);
    }

    /// Accept the WebSocket upgrade, refusing one whose Origin is not allowed. A browser sends the
    /// page's Origin with the upgrade and applies no cross-origin check to WebSockets itself, so
    /// without this any web page could open a connection to a local server.
    static Task<void> accept_upgrade(std::shared_ptr<Impl> state) {
        beast::flat_buffer buffer;
        beast::http::request<beast::http::string_body> upgrade;
        co_await beast::http::async_read(state->ws.next_layer(), buffer, upgrade, asio::use_awaitable);
        const auto origin_it = upgrade.find(beast::http::field::origin);
        const auto origin =
            origin_it == upgrade.end() ? std::nullopt : std::optional<std::string>(origin_it->value());
        if (!state->lock_configuration_and_allow(origin)) {
            beast::http::response<beast::http::string_body> refusal{beast::http::status::forbidden,
                                                                    upgrade.version()};
            refusal.set(beast::http::field::content_type, "text/plain");
            refusal.body() = "Origin not allowed";
            refusal.keep_alive(false);
            refusal.prepare_payload();
            co_await beast::http::async_write(state->ws.next_layer(), refusal, asio::use_awaitable);
            throw std::runtime_error("WebSocketServerTransport refused an upgrade from Origin " +
                                     *origin);
        }
        co_await state->ws.async_accept(upgrade, asio::use_awaitable);
    }

    static void notify_handshake_waiters(const std::shared_ptr<Impl>& state) {
        wake_all(state->handshake_waiters);
    }

    static Task<void> wait_for_handshake(std::shared_ptr<Impl> state) {
        auto waiter = std::make_shared<OperationWaiter>(state->strand);
        state->handshake_waiters.push_back(waiter);

        boost::system::error_code error;
        co_await waiter->signal.async_wait(asio::redirect_error(asio::use_awaitable, error));
        remove_waiter(state->handshake_waiters, waiter);
        if (error && error != asio::error::operation_aborted) {
            throw boost::system::system_error(error);
        }

        throw_if_closed(state);
        if (state->handshake_state == HandshakeState::Open) {
            co_return;
        }
        if (state->handshake_error) {
            std::rethrow_exception(state->handshake_error);
        }
        throw std::runtime_error("WebSocket server handshake did not complete");
    }

    static Task<void> ensure_handshake(std::shared_ptr<Impl> state) {
        throw_if_closed(state);
        if (state->handshake_state == HandshakeState::Open) {
            co_return;
        }
        if (state->handshake_state == HandshakeState::Accepting) {
            co_await wait_for_handshake(std::move(state));
            co_return;
        }
        if (state->handshake_state == HandshakeState::Failed) {
            std::rethrow_exception(state->handshake_error);
        }

        state->handshake_state = HandshakeState::Accepting;
        try {
            co_await accept_upgrade(state);
            throw_if_closed(state);
            state->handshake_state = HandshakeState::Open;
        } catch (...) {
            state->handshake_error = std::current_exception();
            state->handshake_state = HandshakeState::Failed;
            notify_handshake_waiters(state);
            throw;
        }
        notify_handshake_waiters(state);
    }

    static Task<std::string> read(std::shared_ptr<Impl> state) {
        throw_if_closed(state);
        if (state->read_active) {
            throw std::logic_error("WebSocketServerTransport supports only one outstanding read");
        }

        ReadReservation reservation(state->read_active);

        co_await ensure_handshake(state);
        beast::flat_buffer buffer;
        co_await state->ws.async_read(buffer, asio::use_awaitable);
        co_return beast::buffers_to_string(buffer.data());
    }

    static Task<void> write(std::shared_ptr<Impl> state, std::string message) {
        throw_if_closed(state);
        co_await ensure_handshake(state);
        co_await state->write_gate.acquire();
        try {
            co_await state->ws.async_write(asio::buffer(message), asio::use_awaitable);
        } catch (...) {
            state->write_gate.release();
            throw;
        }
        state->write_gate.release();
    }

    static void close_on_strand(const std::shared_ptr<Impl>& state) {
        state->handshake_error = closed_error("WebSocketServerTransport");
        state->handshake_state = HandshakeState::Failed;
        notify_handshake_waiters(state);
        state->write_gate.cancel();
        close_socket(state->ws);
    }
};

WebSocketServerTransport::WebSocketServerTransport(asio::ip::tcp::socket socket)
    : impl_(std::make_shared<Impl>(std::move(socket))) {}

WebSocketServerTransport::~WebSocketServerTransport() {
    try {
        close();
    } catch (...) {
        // Swallow exceptions in destructor to prevent std::terminate.
        (void)0;
    }
}

Task<std::string> WebSocketServerTransport::read_message() {
    auto state = impl_;
    return asio::co_spawn(state->strand, Impl::read(state), asio::use_awaitable);
}

Task<void> WebSocketServerTransport::write_message(std::string_view message) {
    auto state = impl_;
    auto owned_message = std::string(message);
    return asio::co_spawn(state->strand, Impl::write(state, std::move(owned_message)),
                          asio::use_awaitable);
}

void WebSocketServerTransport::set_allowed_origins(std::vector<std::string> origins) {
    std::lock_guard lock(impl_->configuration_mutex);
    impl_->ensure_configurable();
    impl_->allowed_origins.clear();
    impl_->allowed_origins.reserve(origins.size());
    for (auto& origin : origins) {
        impl_->allowed_origins.insert(std::move(origin));
    }
    impl_->allow_all_origins = false;
}

void WebSocketServerTransport::set_allow_all_origins(bool allow_all) {
    std::lock_guard lock(impl_->configuration_mutex);
    impl_->ensure_configurable();
    impl_->allow_all_origins = allow_all;
}

void WebSocketServerTransport::close() {
    auto state = impl_;
    if (state->closed.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    asio::post(state->strand, [state]() { Impl::close_on_strand(state); });
}

// ============================================================================
// WebSocketClientTransport::Impl
// ============================================================================

struct WebSocketClientTransport::Impl {
    enum class ConnectionState {
        Disconnected,
        Connecting,
        Connected,
        Failed,
    };

    std::chrono::milliseconds connect_timeout;
    asio::strand<asio::any_io_executor> strand;
    asio::ip::tcp::resolver resolver;
    WsStream ws;
    std::string host;
    std::string port;
    std::string path;
    std::atomic<bool> closed{false};
    ConnectionState connection_state{ConnectionState::Disconnected};
    std::exception_ptr connect_error;
    std::vector<std::shared_ptr<OperationWaiter>> connection_waiters;
    SerializedWriteGate write_gate;
    bool read_active{false};

    Impl(const asio::any_io_executor& executor, std::string host_arg, std::string port_arg,
         std::string path_arg, std::chrono::milliseconds connect_timeout_arg)
        : connect_timeout(effective_connect_timeout(connect_timeout_arg)),
          strand(asio::make_strand(executor)),
          resolver(strand),
          ws(strand),
          host(std::move(host_arg)),
          port(std::move(port_arg)),
          path(std::move(path_arg)),
          write_gate(strand, closed, "WebSocketClientTransport") {
        ws.text(true);
    }

    static void throw_if_closed(const std::shared_ptr<Impl>& state) {
        require_open(state->closed, "WebSocketClientTransport");
    }

    static void notify_connection_waiters(const std::shared_ptr<Impl>& state) {
        wake_all(state->connection_waiters);
    }

    static Task<void> wait_for_connection(std::shared_ptr<Impl> state) {
        auto waiter = std::make_shared<OperationWaiter>(state->strand);
        state->connection_waiters.push_back(waiter);
        boost::system::error_code error;
        co_await waiter->signal.async_wait(asio::redirect_error(asio::use_awaitable, error));
        remove_waiter(state->connection_waiters, waiter);
        if (error && error != asio::error::operation_aborted) {
            throw boost::system::system_error(error);
        }

        throw_if_closed(state);
        if (state->connection_state == ConnectionState::Connected) {
            co_return;
        }
        if (state->connect_error) {
            std::rethrow_exception(state->connect_error);
        }
        throw std::runtime_error("WebSocket connection did not complete");
    }

    static Task<void> ensure_connected(std::shared_ptr<Impl> state) {
        throw_if_closed(state);
        if (state->connection_state == ConnectionState::Connected) {
            co_return;
        }
        if (state->connection_state == ConnectionState::Connecting) {
            co_await wait_for_connection(std::move(state));
            co_return;
        }
        if (state->connection_state == ConnectionState::Failed) {
            std::rethrow_exception(state->connect_error);
        }

        state->connection_state = ConnectionState::Connecting;
        state->connect_error = nullptr;
        std::optional<detail::SocketDeadline> deadline;
        try {
            auto results =
                co_await state->resolver.async_resolve(state->host, state->port, asio::use_awaitable);
            // A close() that ran while the resolve could no longer be cancelled closed a socket
            // that was not open yet. From here to the socket opening inside async_connect()
            // nothing suspends, so a later close() runs on the strand after it and closes it.
            throw_if_closed(state);
            // One deadline covers the TCP connect and the handshake: a peer that accepts and then
            // says nothing would otherwise hold the pending call forever.
            if (state->connect_timeout > std::chrono::milliseconds::zero()) {
                deadline.emplace(state->strand, state->ws.next_layer().socket(),
                                 state->connect_timeout);
            }
            co_await state->ws.next_layer().async_connect(results, asio::use_awaitable);
            co_await state->ws.async_handshake(state->host + ":" + state->port, state->path,
                                               asio::use_awaitable);
            deadline.reset();
            throw_if_closed(state);
            state->connection_state = ConnectionState::Connected;
        } catch (...) {
            const bool timed_out = deadline && deadline->expired();
            deadline.reset();
            state->connect_error =
                timed_out ? std::make_exception_ptr(boost::system::system_error(beast::error::timeout))
                          : std::current_exception();
            state->connection_state = ConnectionState::Failed;
            notify_connection_waiters(state);
            std::rethrow_exception(state->connect_error);
        }
        notify_connection_waiters(state);
    }

    static Task<std::string> read(std::shared_ptr<Impl> state) {
        throw_if_closed(state);
        if (state->read_active) {
            throw std::logic_error("WebSocketClientTransport supports only one outstanding read");
        }

        ReadReservation reservation(state->read_active);

        co_await ensure_connected(state);
        beast::flat_buffer buffer;
        co_await state->ws.async_read(buffer, asio::use_awaitable);
        co_return beast::buffers_to_string(buffer.data());
    }

    static Task<void> write(std::shared_ptr<Impl> state, std::string message) {
        throw_if_closed(state);
        co_await ensure_connected(state);
        co_await state->write_gate.acquire();
        try {
            co_await state->ws.async_write(asio::buffer(message), asio::use_awaitable);
        } catch (...) {
            state->write_gate.release();
            throw;
        }
        state->write_gate.release();
    }

    static void close_on_strand(const std::shared_ptr<Impl>& state) {
        state->connect_error = closed_error("WebSocketClientTransport");
        state->connection_state = ConnectionState::Failed;
        notify_connection_waiters(state);
        state->write_gate.cancel();
        state->resolver.cancel();
        close_socket(state->ws);
    }
};

WebSocketClientTransport::WebSocketClientTransport(const asio::any_io_executor& executor,
                                                   std::string host, std::string port, std::string path,
                                                   std::chrono::milliseconds connect_timeout)
    : impl_(std::make_shared<Impl>(executor, std::move(host), std::move(port), std::move(path),
                                   connect_timeout)) {}

WebSocketClientTransport::~WebSocketClientTransport() {
    try {
        close();
    } catch (...) {
        // Swallow exceptions in destructor to prevent std::terminate.
        (void)0;
    }
}

Task<std::string> WebSocketClientTransport::read_message() {
    auto state = impl_;
    return asio::co_spawn(state->strand, Impl::read(state), asio::use_awaitable);
}

Task<void> WebSocketClientTransport::write_message(std::string_view message) {
    auto state = impl_;
    auto owned_message = std::string(message);
    return asio::co_spawn(state->strand, Impl::write(state, std::move(owned_message)),
                          asio::use_awaitable);
}

void WebSocketClientTransport::close() {
    auto state = impl_;
    if (state->closed.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    asio::post(state->strand, [state]() { Impl::close_on_strand(state); });
}

}  // namespace mcp

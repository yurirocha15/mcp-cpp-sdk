#include <mcp/core/async.hpp>
#include <mcp/transport/tls.hpp>
#include <mcp/transport/websocket.hpp>

#include "mcp/detail/constants.hpp"
#include "mcp/detail/net/connection.hpp"
#include "mcp/detail/net/network_failure.hpp"
#include "mcp/detail/net/socket_deadline.hpp"
#include "mcp/detail/net/tls.hpp"
#include "mcp/detail/text/diagnostic_text.hpp"
#include "mcp/detail/transport/host_policy.hpp"
#include "tls_accept.hpp"

#include <algorithm>
#include <atomic>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core/buffers_to_string.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http/read.hpp>
#include <boost/beast/http/string_body.hpp>
#include <boost/beast/http/write.hpp>
#include <boost/beast/websocket/stream.hpp>
#include <boost/system/system_error.hpp>
#include <chrono>
#include <cstdint>
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
using WsStream = beast::websocket::stream<detail::Connection>;

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

struct OperationWaiter {
    explicit OperationWaiter(const asio::any_io_executor& executor) : signal(executor) {
        signal.expires_at(mcp::detail::constants::websocket::g_NEVER);
    }

    void wake() noexcept {
        try {
            signal.cancel();
        } catch (...) {
            // steady_timer::cancel() is not noexcept; a failed wake-up has no caller to report to.
        }
    }

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
    auto position = std::ranges::find(waiters, waiter);
    if (position != waiters.end()) {
        waiters.erase(position);
    }
}

void require_open(const std::atomic<bool>& closed, std::string_view transport_name) {
    if (closed.load(std::memory_order_acquire)) {
        throw TransportError(TransportError::Kind::eClosed, std::string(transport_name) + " is closed");
    }
}

template <typename Context>
std::shared_ptr<const Context> require_context(std::shared_ptr<const Context> context) {
    if (!context) {
        throw std::invalid_argument("A WebSocket transport over TLS needs a TLS context");
    }
    return context;
}

std::exception_ptr closed_error(std::string_view transport_name) {
    return std::make_exception_ptr(
        TransportError(TransportError::Kind::eClosed, std::string(transport_name) + " is closed"));
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
    SerializedWriteGate(asio::any_io_executor executor, const std::atomic<bool>& closed,
                        std::string_view transport_name)
        : executor_(std::move(executor)), closed_(closed), transport_name_(transport_name) {}

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
            throw detail::network_failure(error, TransportError::Kind::eIo, transport_name_);
        }
        if (!waiter->granted) {
            require_open(closed_, transport_name_);
            throw TransportError(TransportError::Kind::eIo, transport_name_ + " write was interrupted");
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
    enum class HandshakeState : std::uint8_t {
        ePending,
        eAccepting,
        eOpen,
        eFailed,
    };

    /// Null for a plain connection. Holds the context the TLS stream was built on.
    std::shared_ptr<const TlsServerContext> tls;
    WsStream ws;
    asio::strand<asio::any_io_executor> strand;
    std::atomic<bool> closed{false};
    HandshakeState handshake_state{HandshakeState::ePending};
    std::exception_ptr handshake_error;
    std::vector<std::shared_ptr<OperationWaiter>> handshake_waiters;
    SerializedWriteGate write_gate;
    bool read_active{false};
    /// Copied from the configuration on the strand once the handshake is done.
    std::chrono::milliseconds effective_idle_timeout{};

    mutable std::mutex configuration_mutex;
    std::unordered_set<std::string> allowed_origins;
    bool allow_all_origins{false};
    detail::HostPolicy host_policy;
    /// Set once the handshake starts; the Origin policy and the limits are fixed from then on.
    bool configuration_locked{false};
    std::chrono::milliseconds handshake_timeout{mcp::constants::websocket::g_DEFAULT_HANDSHAKE_TIMEOUT};
    std::chrono::milliseconds idle_timeout{mcp::constants::websocket::g_DEFAULT_IDLE_TIMEOUT};
    std::size_t max_message_bytes{mcp::detail::constants::websocket::g_DEFAULT_MAX_MESSAGE_BYTES};

    Impl(asio::ip::tcp::socket socket, std::shared_ptr<const TlsServerContext> tls_arg)
        : tls(std::move(tls_arg)),
          ws(make_stream(std::move(socket), tls.get())),
          strand(asio::make_strand(ws.get_executor())),
          write_gate(strand, closed, "WebSocketServerTransport") {
        ws.text(true);
    }

    static WsStream make_stream(asio::ip::tcp::socket socket, const TlsServerContext* tls) {
        if (tls != nullptr) {
            return WsStream(std::move(socket), detail::TlsContextAccess::native(*tls));
        }
        return WsStream(std::move(socket));
    }

    static void throw_if_closed(const std::shared_ptr<Impl>& state) {
        require_open(state->closed, "WebSocketServerTransport");
    }

    void ensure_configurable() const {
        if (configuration_locked || closed.load(std::memory_order_acquire)) {
            throw std::logic_error(
                "WebSocketServerTransport must be configured before the first read or write");
        }
    }

    /// What the policies say of an upgrade.
    enum class Admission : std::uint8_t {
        eAllowed,
        eHostRefused,
        eOriginRefused,
    };

    /// Fix the policies for the handshake and say whether an upgrade naming `host` (none when
    /// absent) and carrying `origin` (none when absent) may open the connection.
    Admission lock_configuration_and_admit(const std::optional<std::string_view>& host,
                                           const std::optional<std::string>& origin) {
        boost::system::error_code endpoint_error;
        const auto local_endpoint = ws.next_layer().socket().local_endpoint(endpoint_error);
        const bool loopback_connection = endpoint_error || detail::is_loopback_endpoint(local_endpoint);
        std::scoped_lock lock(configuration_mutex);
        configuration_locked = true;
        if (!host_policy.allows(host, loopback_connection)) {
            return Admission::eHostRefused;
        }
        return !origin || allow_all_origins || allowed_origins.contains(*origin)
                   ? Admission::eAllowed
                   : Admission::eOriginRefused;
    }

    /// Accept the WebSocket upgrade, after the TLS handshake of a TLS connection, refusing one whose
    /// Host or Origin is not allowed. A browser sends the page's Origin with the upgrade and applies
    /// no cross-origin check to WebSockets itself, so without this any web page could open a
    /// connection to a local server.
    static Task<void> accept_upgrade(std::shared_ptr<Impl> state) {
        std::optional<detail::SocketDeadline> deadline;
        if (const auto timeout = effective_connect_timeout(state->handshake_timeout);
            timeout > std::chrono::milliseconds::zero()) {
            deadline.emplace(state->strand, state->ws.next_layer().socket(), timeout);
        }
        try {
            co_await read_and_answer_upgrade(state);
        } catch (const boost::system::system_error& error) {
            throw detail::network_failure(deadline && deadline->expired()
                                              ? boost::system::error_code(beast::error::timeout)
                                              : error.code(),
                                          TransportError::Kind::eConnect, "WebSocketServerTransport");
        } catch (...) {
            if (deadline && deadline->expired()) {
                throw detail::network_failure(beast::error::timeout, TransportError::Kind::eConnect,
                                              "WebSocketServerTransport");
            }
            throw;
        }
    }

    static Task<void> read_and_answer_upgrade(std::shared_ptr<Impl> state) {
        if (state->tls) {
            // Within the deadline accept_upgrade() set.
            co_await detail::accept_tls(state->ws.next_layer(), state->strand,
                                        std::chrono::milliseconds::zero());
        }
        beast::flat_buffer buffer;
        beast::http::request<beast::http::string_body> upgrade;
        co_await beast::http::async_read(state->ws.next_layer(), buffer, upgrade, asio::use_awaitable);
        const auto origin_it = upgrade.find(beast::http::field::origin);
        const auto origin =
            origin_it == upgrade.end() ? std::nullopt : std::optional<std::string>(origin_it->value());
        std::optional<std::string_view> host;
        if (upgrade.count(beast::http::field::host) == 1) {
            const auto value = upgrade[beast::http::field::host];
            host = std::string_view(value.data(), value.size());
        }
        const auto admission = state->lock_configuration_and_admit(host, origin);
        if (admission != Admission::eAllowed) {
            const bool host_refused = admission == Admission::eHostRefused;
            beast::http::response<beast::http::string_body> refusal{beast::http::status::forbidden,
                                                                    upgrade.version()};
            refusal.set(beast::http::field::content_type, "text/plain");
            refusal.body() = host_refused ? "Host not allowed" : "Origin not allowed";
            refusal.keep_alive(false);
            refusal.prepare_payload();
            co_await beast::http::async_write(state->ws.next_layer(), refusal, asio::use_awaitable);
            throw TransportError(
                TransportError::Kind::eConnect,
                host_refused ? "WebSocketServerTransport refused an upgrade for Host " +
                                   detail::sanitize_for_diagnostics(host.value_or(""))
                             : "WebSocketServerTransport refused an upgrade from Origin " + *origin);
        }
        co_await state->ws.async_accept(upgrade, asio::use_awaitable);
        std::scoped_lock lock(state->configuration_mutex);
        state->ws.read_message_max(state->max_message_bytes);
        state->effective_idle_timeout = effective_connect_timeout(state->idle_timeout);
        // A peer that answers the pings stays connected however quiet the session is.
        beast::websocket::stream_base::timeout timeouts{};
        timeouts.handshake_timeout = beast::websocket::stream_base::none();
        timeouts.idle_timeout = state->effective_idle_timeout > std::chrono::milliseconds::zero()
                                    ? std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                          state->effective_idle_timeout)
                                    : beast::websocket::stream_base::none();
        timeouts.keep_alive_pings = true;
        state->ws.set_option(timeouts);
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
            throw detail::network_failure(error, TransportError::Kind::eConnect,
                                          "WebSocketServerTransport");
        }

        throw_if_closed(state);
        if (state->handshake_state == HandshakeState::eOpen) {
            co_return;
        }
        if (state->handshake_error) {
            std::rethrow_exception(state->handshake_error);
        }
        throw TransportError(TransportError::Kind::eConnect,
                             "WebSocket server handshake did not complete");
    }

    static Task<void> ensure_handshake(std::shared_ptr<Impl> state) {
        throw_if_closed(state);
        if (state->handshake_state == HandshakeState::eOpen) {
            co_return;
        }
        if (state->handshake_state == HandshakeState::eAccepting) {
            co_await wait_for_handshake(std::move(state));
            co_return;
        }
        if (state->handshake_state == HandshakeState::eFailed) {
            std::rethrow_exception(state->handshake_error);
        }

        state->handshake_state = HandshakeState::eAccepting;
        try {
            co_await accept_upgrade(state);
            throw_if_closed(state);
            state->handshake_state = HandshakeState::eOpen;
        } catch (...) {
            state->handshake_error = std::current_exception();
            state->handshake_state = HandshakeState::eFailed;
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
        try {
            co_await state->ws.async_read(buffer, asio::use_awaitable);
        } catch (const boost::system::system_error& error) {
            throw detail::network_failure(error.code(), TransportError::Kind::eIo,
                                          "WebSocketServerTransport");
        }
        co_return beast::buffers_to_string(buffer.data());
    }

    static Task<void> write(std::shared_ptr<Impl> state, std::string message) {
        throw_if_closed(state);
        co_await ensure_handshake(state);
        co_await state->write_gate.acquire();
        // A peer that stopped reading answers no ping either, but the read that notices may not be
        // running: the server stops reading at its limit on requests in flight.
        std::optional<detail::SocketDeadline> deadline;
        if (state->effective_idle_timeout > std::chrono::milliseconds::zero()) {
            deadline.emplace(state->strand, state->ws.next_layer().socket(),
                             state->effective_idle_timeout);
        }
        try {
            co_await state->ws.async_write(asio::buffer(message), asio::use_awaitable);
        } catch (const boost::system::system_error& error) {
            const bool timed_out = deadline && deadline->expired();
            deadline.reset();
            state->write_gate.release();
            throw detail::network_failure(
                timed_out ? boost::system::error_code(beast::error::timeout) : error.code(),
                TransportError::Kind::eIo, "WebSocketServerTransport");
        } catch (...) {
            deadline.reset();
            state->write_gate.release();
            throw;
        }
        deadline.reset();
        state->write_gate.release();
    }

    static void close_on_strand(const std::shared_ptr<Impl>& state) {
        state->handshake_error = closed_error("WebSocketServerTransport");
        state->handshake_state = HandshakeState::eFailed;
        notify_handshake_waiters(state);
        state->write_gate.cancel();
        close_socket(state->ws);
    }
};

WebSocketServerTransport::WebSocketServerTransport(asio::ip::tcp::socket socket)
    : impl_(std::make_shared<Impl>(std::move(socket), nullptr)) {}

WebSocketServerTransport::WebSocketServerTransport(asio::ip::tcp::socket socket,
                                                   std::shared_ptr<const TlsServerContext> tls)
    : impl_(std::make_shared<Impl>(std::move(socket), require_context(std::move(tls)))) {}

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
    std::scoped_lock lock(impl_->configuration_mutex);
    impl_->ensure_configurable();
    impl_->allowed_origins.clear();
    impl_->allowed_origins.reserve(origins.size());
    for (auto& origin : origins) {
        impl_->allowed_origins.insert(std::move(origin));
    }
    impl_->allow_all_origins = false;
}

void WebSocketServerTransport::set_allow_all_origins(bool allow_all) {
    std::scoped_lock lock(impl_->configuration_mutex);
    impl_->ensure_configurable();
    impl_->allow_all_origins = allow_all;
}

void WebSocketServerTransport::set_allowed_hosts(const std::vector<std::string>& hosts) {
    detail::HostPolicy policy(hosts);
    std::scoped_lock lock(impl_->configuration_mutex);
    impl_->ensure_configurable();
    impl_->host_policy = std::move(policy);
}

void WebSocketServerTransport::set_handshake_timeout(std::chrono::milliseconds timeout) {
    std::scoped_lock lock(impl_->configuration_mutex);
    impl_->ensure_configurable();
    impl_->handshake_timeout = timeout;
}

void WebSocketServerTransport::set_idle_timeout(std::chrono::milliseconds timeout) {
    std::scoped_lock lock(impl_->configuration_mutex);
    impl_->ensure_configurable();
    impl_->idle_timeout = timeout;
}

void WebSocketServerTransport::set_max_message_bytes(std::size_t bytes) {
    std::scoped_lock lock(impl_->configuration_mutex);
    impl_->ensure_configurable();
    impl_->max_message_bytes = bytes;
}

void WebSocketServerTransport::close() {
    auto state = impl_;
    if (state->closed.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    asio::post(state->strand, [state] { Impl::close_on_strand(state); });
}

// ============================================================================
// WebSocketClientTransport::Impl
// ============================================================================

struct WebSocketClientTransport::Impl {
    enum class ConnectionState : std::uint8_t {
        eDisconnected,
        eConnecting,
        eConnected,
        eFailed,
    };

    std::chrono::milliseconds connect_timeout;
    asio::strand<asio::any_io_executor> strand;
    asio::ip::tcp::resolver resolver;
    /// Replaced by a TLS stream when the first connection starts, if TLS is set.
    std::unique_ptr<WsStream> ws;
    std::string host;
    std::string port;
    std::string path;
    std::atomic<bool> closed{false};
    ConnectionState connection_state{ConnectionState::eDisconnected};
    std::exception_ptr connect_error;
    std::vector<std::shared_ptr<OperationWaiter>> connection_waiters;
    SerializedWriteGate write_gate;
    bool read_active{false};
    /// The certificate name of a TLS connection being made, and the Host of its upgrade request.
    std::optional<std::string> tls_name;
    std::string authority;
    std::atomic<std::size_t> max_message_bytes{
        mcp::detail::constants::websocket::g_DEFAULT_MAX_MESSAGE_BYTES};

    mutable std::mutex configuration_mutex;
    /// Whether the connection is made over TLS, with `tls_context` (null: the default context).
    bool use_tls{false};
    std::shared_ptr<const TlsClientContext> tls_context;
    /// The name the server certificate is checked against; empty for the host.
    std::string server_name;
    /// Set when the first connection starts; the TLS settings are fixed from then on.
    bool configuration_locked{false};

    Impl(const asio::any_io_executor& executor, std::string host_arg, std::string port_arg,
         std::string path_arg, std::chrono::milliseconds connect_timeout_arg)
        : connect_timeout(effective_connect_timeout(connect_timeout_arg)),
          strand(asio::make_strand(executor)),
          resolver(strand),
          ws(std::make_unique<WsStream>(strand)),
          host(std::move(host_arg)),
          port(std::move(port_arg)),
          path(std::move(path_arg)),
          write_gate(strand, closed, "WebSocketClientTransport") {
        ws->text(true);
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
            throw detail::network_failure(error, TransportError::Kind::eConnect,
                                          "WebSocketClientTransport");
        }

        throw_if_closed(state);
        if (state->connection_state == ConnectionState::eConnected) {
            co_return;
        }
        if (state->connect_error) {
            std::rethrow_exception(state->connect_error);
        }
        throw TransportError(TransportError::Kind::eConnect, "WebSocket connection did not complete");
    }

    void ensure_configurable() const {
        if (configuration_locked || closed.load(std::memory_order_acquire)) {
            throw std::logic_error(
                "WebSocketClientTransport must be configured before the first read or write");
        }
    }

    /// Fixes the TLS settings and builds the stream they ask for. Runs on the strand, before
    /// anything else uses the stream. Returns the name to check the certificate against, or
    /// nothing for a plain connection.
    std::optional<std::string> lock_configuration_and_prepare_stream() {
        std::scoped_lock lock(configuration_mutex);
        configuration_locked = true;
        if (!use_tls) {
            return std::nullopt;
        }
        const auto context = tls_context ? tls_context : detail::default_client_context();
        ws = std::make_unique<WsStream>(strand, detail::TlsContextAccess::native(*context));
        // The stream holds the OpenSSL context; the object is kept for as long as the stream.
        tls_context = context;
        ws->text(true);
        return server_name.empty() ? detail::tls_server_name(host) : server_name;
    }

    static Task<void> ensure_connected(std::shared_ptr<Impl> state) {
        throw_if_closed(state);
        if (state->connection_state == ConnectionState::eConnected) {
            co_return;
        }
        if (state->connection_state == ConnectionState::eConnecting) {
            co_await wait_for_connection(std::move(state));
            co_return;
        }
        if (state->connection_state == ConnectionState::eFailed) {
            std::rethrow_exception(state->connect_error);
        }

        state->connection_state = ConnectionState::eConnecting;
        state->connect_error = nullptr;
        // Held by the state rather than the coroutine frame: GCC 11 mishandles a std::string that
        // lives across a suspension point.
        state->tls_name = state->lock_configuration_and_prepare_stream();
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
                deadline.emplace(state->strand, state->ws->next_layer().socket(),
                                 state->connect_timeout);
            }
            co_await asio::async_connect(state->ws->next_layer().socket(), results,
                                         asio::use_awaitable);
            if (state->tls_name) {
                co_await handshake_tls(state);
            }
            state->authority = detail::authority_of(detail::tls_server_name(state->host), state->port,
                                                    state->tls_name.has_value());
            co_await state->ws->async_handshake(state->authority, state->path, asio::use_awaitable);
            deadline.reset();
            throw_if_closed(state);
            state->ws->read_message_max(state->max_message_bytes.load(std::memory_order_acquire));
            state->connection_state = ConnectionState::eConnected;
        } catch (const boost::system::system_error& error) {
            const bool timed_out = deadline && deadline->expired();
            deadline.reset();
            fail_connection(
                state, std::make_exception_ptr(detail::network_failure(
                           timed_out ? boost::system::error_code(beast::error::timeout) : error.code(),
                           TransportError::Kind::eConnect, "WebSocketClientTransport")));
        } catch (...) {
            const bool timed_out = deadline && deadline->expired();
            deadline.reset();
            fail_connection(state, timed_out
                                       ? std::make_exception_ptr(detail::network_failure(
                                             beast::error::timeout, TransportError::Kind::eConnect,
                                             "WebSocketClientTransport"))
                                       : std::current_exception());
        }
        notify_connection_waiters(state);
    }

    /// Records the connection's @p failure for the calls waiting on it and throws it.
    [[noreturn]] static void fail_connection(const std::shared_ptr<Impl>& state,
                                             std::exception_ptr failure) {
        state->connect_error = std::move(failure);
        state->connection_state = ConnectionState::eFailed;
        notify_connection_waiters(state);
        std::rethrow_exception(state->connect_error);
    }

    static Task<void> handshake_tls(std::shared_ptr<Impl> state) {
        auto& connection = state->ws->next_layer();
        auto* tls = connection.tls();
        detail::prepare_client_ssl(tls->native_handle(), *state->tls_name);
        std::exception_ptr failure;
        boost::system::error_code error;
        try {
            co_await tls->async_handshake(asio::ssl::stream_base::client, asio::use_awaitable);
        } catch (const boost::system::system_error& handshake_error) {
            error = handshake_error.code();
            failure = std::current_exception();
        }
        if (failure) {
            // A deadline that passed is reported as a timeout by the caller.
            throw detail::handshake_failure(error, tls->native_handle(), *state->tls_name, false);
        }
        connection.mark_established();
    }

    static Task<std::string> read(std::shared_ptr<Impl> state) {
        throw_if_closed(state);
        if (state->read_active) {
            throw std::logic_error("WebSocketClientTransport supports only one outstanding read");
        }

        ReadReservation reservation(state->read_active);

        co_await ensure_connected(state);
        beast::flat_buffer buffer;
        try {
            co_await state->ws->async_read(buffer, asio::use_awaitable);
        } catch (const boost::system::system_error& error) {
            throw detail::network_failure(error.code(), TransportError::Kind::eIo,
                                          "WebSocketClientTransport");
        }
        co_return beast::buffers_to_string(buffer.data());
    }

    static Task<void> write(std::shared_ptr<Impl> state, std::string message) {
        throw_if_closed(state);
        co_await ensure_connected(state);
        co_await state->write_gate.acquire();
        try {
            co_await state->ws->async_write(asio::buffer(message), asio::use_awaitable);
        } catch (const boost::system::system_error& error) {
            state->write_gate.release();
            throw detail::network_failure(error.code(), TransportError::Kind::eIo,
                                          "WebSocketClientTransport");
        } catch (...) {
            state->write_gate.release();
            throw;
        }
        state->write_gate.release();
    }

    static void close_on_strand(const std::shared_ptr<Impl>& state) {
        state->connect_error = closed_error("WebSocketClientTransport");
        state->connection_state = ConnectionState::eFailed;
        notify_connection_waiters(state);
        state->write_gate.cancel();
        state->resolver.cancel();
        close_socket(*state->ws);
    }
};

WebSocketClientTransport::WebSocketClientTransport(const Executor& executor, std::string host,
                                                   std::string port, std::string path,
                                                   std::chrono::milliseconds connect_timeout)
    : impl_(std::make_shared<Impl>(executor.get(), std::move(host), std::move(port), std::move(path),
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

void WebSocketClientTransport::set_max_message_bytes(std::size_t bytes) {
    impl_->max_message_bytes.store(bytes, std::memory_order_release);
}

void WebSocketClientTransport::set_tls(const TlsClientOptions& options) {
    set_tls(TlsClientContext::create(options));
}

void WebSocketClientTransport::set_tls(std::shared_ptr<const TlsClientContext> context) {
    std::scoped_lock lock(impl_->configuration_mutex);
    impl_->ensure_configurable();
    impl_->use_tls = true;
    impl_->tls_context = std::move(context);
}

void WebSocketClientTransport::set_server_name(std::string name) {
    name = detail::tls_server_name(std::move(name));
    if (name.empty()) {
        throw std::invalid_argument("The TLS server name is empty");
    }
    std::scoped_lock lock(impl_->configuration_mutex);
    impl_->ensure_configurable();
    impl_->server_name = std::move(name);
}

void WebSocketClientTransport::close() {
    auto state = impl_;
    if (state->closed.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    asio::post(state->strand, [state] { Impl::close_on_strand(state); });
}

}  // namespace mcp

#include "../detail/accept_retry.hpp"
#include "../detail/beast_string.hpp"
#include "../detail/http_limits.hpp"
#include "../detail/http_request_metadata.hpp"
#include "../detail/jsonrpc_envelope.hpp"
#include "../detail/memory_transport_read.hpp"
#include "../detail/modern_methods.hpp"
#include "../detail/peer_json.hpp"
#include "../detail/principal.hpp"
#include "../detail/socket_probe.hpp"
#include "../detail/strand_executor.hpp"

#include <mcp/core/constants.hpp>
#include <mcp/detail/secure_random.hpp>
#include <mcp/transport/http_session_manager.hpp>
#include <mcp/transport/memory.hpp>

#include <boost/asio/bind_executor.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <deque>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace mcp {

namespace beast = boost::beast;
namespace http = boost::beast::http;

namespace detail_session_mgr {

// ============================================================================
// Per-session runtime state
// ============================================================================

struct PendingResponse {
    std::shared_ptr<boost::asio::steady_timer> ready_timer;
    std::optional<std::string> response_body;
    /// Events of this POST's stream the connection serving it has not taken yet.
    SseEventList stream_events;
    bool response_ready{false};
    /// Names the stream that answers this POST in the session's event store.
    std::string stream;
    /// Which connection serves the stream: a resume takes it over from the one before.
    std::uint64_t generation{0};
    /// No connection serves the stream; its events wait in the event store for a resume.
    bool detached{false};
};

struct PendingResult {
    std::optional<std::string> response_body;
    SseEventList stream_events;
};

struct SessionRuntime;

/// A stream of a session of protocol revision 2025-11-25 or earlier that a connection writes as
/// its events come: the answer to a POST, or the stream a GET opened.
struct LegacyStream {
    std::shared_ptr<SessionRuntime> session;
    /// The key of the POST's request, or empty for the GET stream.
    std::string request_key;
    /// The PendingResponse::generation or SessionRuntime::get_generation this connection serves.
    std::uint64_t generation{0};
    /// Woken, on the connection's strand, when the stream has something new.
    std::shared_ptr<boost::asio::steady_timer> signal;
    /// Events to write before any other: those a resumed stream missed, or the priming event.
    SseEventList replay;
    /// Opens the stream at once rather than at its first event, as a GET does.
    bool open_at_once{false};
};

/// Appends one SSE event carrying @p data, a JSON-RPC message or nothing, to @p out.
inline void append_sse_event(std::string& out, const std::string& id, const std::string& data) {
    out += "id: ";
    out += id;
    out += "\ndata: ";
    out += data;
    out += "\n\n";
}

/// The answer to a 2026-07-28 request while it is produced: the notifications its handler sends
/// and, once the server has answered, the response. Filled from the dispatch executor and read on
/// the connection's strand, which owns `signal`.
struct ModernExchange : std::enable_shared_from_this<ModernExchange> {
    ModernExchange(boost::asio::any_io_executor strand_arg, std::size_t max_queued_arg,
                   bool streams_arg)
        : strand(std::move(strand_arg)),
          signal(strand),
          max_queued(max_queued_arg),
          streams(streams_arg) {
        signal.expires_at(std::chrono::steady_clock::time_point::max());
    }

    /// Wakes the connection; posted because `signal` and `woken` belong to the strand.
    void wake() {
        boost::asio::post(strand, [self = shared_from_this()] { self->wake_on_strand(); });
    }

    /// The connection may be busy writing rather than waiting, when cancelling the timer does
    /// nothing; `woken` keeps the wake for its next wait.
    void wake_on_strand() {
        woken = true;
        signal.cancel();
    }

    /// Queues a notification for the stream, or returns false when the queue is full.
    bool push_notification(std::string message) {
        {
            std::lock_guard lock(mutex);
            if (notifications.size() >= max_queued) {
                return false;
            }
            notifications.push_back(std::move(message));
        }
        wake();
        return true;
    }

    /// Cancels the request and wakes a handler that waits for its cancellation.
    void cancel() {
        cancelled->store(true, std::memory_order_relaxed);
        std::function<void()> hook;
        {
            std::lock_guard lock(mutex);
            hook = on_cancel;
        }
        if (hook) {
            hook();
        }
    }

    /// Registers the function to call after the request is cancelled. The caller reads
    /// `cancelled` once registered, since a cancellation before it did not call the function.
    void set_on_cancel(std::function<void()> hook) {
        std::lock_guard lock(mutex);
        on_cancel = std::move(hook);
    }

    void finish(std::string response_wire) {
        {
            std::lock_guard lock(mutex);
            response = std::move(response_wire);
        }
        wake();
    }

    boost::asio::any_io_executor strand;
    boost::asio::steady_timer signal;
    bool woken{false};
    const std::size_t max_queued;
    /// Whether the answer may become an SSE stream: the client accepts one over HTTP/1.1.
    const bool streams;
    std::shared_ptr<std::atomic<bool>> cancelled = std::make_shared<std::atomic<bool>>(false);

    std::mutex mutex;
    std::deque<std::string> notifications;  // guarded by mutex
    std::optional<std::string> response;    // guarded by mutex
    std::function<void()> on_cancel;        // guarded by mutex
};

struct SessionRuntime {
    std::string session_id;
    /// The principal of the request that created the session. Only it may use the session.
    std::string principal;

    std::shared_ptr<MemoryTransport> client_transport;

    mutable std::mutex protocol_mutex;
    std::string negotiated_protocol_version{std::string(g_LATEST_LEGACY_PROTOCOL_VERSION)};

    mutable std::mutex response_mutex;
    EventStore event_store;

    std::unordered_map<std::string, PendingResponse> pending_responses;
    /// Messages sent while several POSTs wait, without an id yet: each is stored, and given its
    /// id, once the next answer names the stream it travels on.
    std::vector<std::string> unassigned_stream_events;
    /// Counts the streams of this session, to name each one.
    std::uint64_t streams{0};

    /// The stream a GET opened, for messages that belong to no request: its name in the event
    /// store (empty until a GET opens it), the timer that wakes the connection serving it (null
    /// while none does), the events that connection has not taken yet, and which connection serves
    /// it.
    std::string get_stream;
    std::shared_ptr<boost::asio::steady_timer> get_timer;
    SseEventList get_events;
    std::uint64_t get_generation{0};
    /// The session is over, and so are its streams.
    bool ended{false};

    /// A response has succeeded on this session, so its initialize did.
    std::atomic<bool> established{false};
    /// When a request last named this session, as a steady_clock count.
    std::atomic<std::chrono::steady_clock::rep> last_used{
        std::chrono::steady_clock::now().time_since_epoch().count()};

    void touch() {
        last_used.store(std::chrono::steady_clock::now().time_since_epoch().count(),
                        std::memory_order_relaxed);
    }

    [[nodiscard]] bool idle_for(std::chrono::milliseconds timeout) {
        const std::chrono::steady_clock::duration used{last_used.load(std::memory_order_relaxed)};
        if (std::chrono::steady_clock::now().time_since_epoch() - used < timeout) {
            return false;
        }
        std::lock_guard lock(response_mutex);
        return pending_responses.empty();
    }

    explicit SessionRuntime(
        std::string id, std::size_t event_store_capacity = constants::g_event_store_default_capacity)
        : session_id(std::move(id)), event_store(event_store_capacity) {}

    [[nodiscard]] std::string protocol_version() const {
        std::lock_guard lock(protocol_mutex);
        return negotiated_protocol_version;
    }

    void set_protocol_version(std::string version) {
        std::lock_guard lock(protocol_mutex);
        negotiated_protocol_version = std::move(version);
    }
};

// ============================================================================
// Helpers
// ============================================================================

inline std::string generate_session_id() { return detail::generate_secure_session_id(); }

inline bool is_initialize_request(const nlohmann::json& request_json) {
    return request_json.is_object() && request_json.contains("method") &&
           request_json.at("method").is_string() &&
           request_json.at("method").get<std::string>() == "initialize";
}

inline std::optional<std::string> initialize_protocol_version(const nlohmann::json& request_json) {
    if (!request_json.is_object() || !request_json.contains("params") ||
        !request_json.at("params").is_object()) {
        return std::nullopt;
    }

    const auto& params = request_json.at("params");
    if (!params.contains("protocolVersion") || !params.at("protocolVersion").is_string()) {
        return std::nullopt;
    }

    return params.at("protocolVersion").get<std::string>();
}

inline std::string_view protocol_header_value(const StringRequest::const_iterator& header_it) {
    return {header_it->value().data(), header_it->value().size()};
}

/// Whether a request on a session may name @p version in MCP-Protocol-Version: the version the
/// session negotiated, or any other version initialize can negotiate, as the TypeScript, Go and
/// Rust SDKs accept.
inline bool session_accepts_version(std::string_view version, std::string_view negotiated) {
    return version == negotiated || is_legacy_protocol_version(version);
}

inline StringResponse make_json_response(const StringRequest& request, http::status status_code,
                                         std::string body) {
    StringResponse response{status_code, request.version()};
    response.set(http::field::server, "mcp-cpp-sdk");
    response.set(http::field::content_type, "application/json");
    response.keep_alive(request.keep_alive());
    response.body() = std::move(body);
    response.prepare_payload();
    return response;
}

inline StringResponse make_error_response(const StringRequest& request, http::status status_code,
                                          std::string_view error_message) {
    nlohmann::json error_body = nlohmann::json::object();
    error_body["error"] = std::string(error_message);
    return make_json_response(request, status_code, error_body.dump());
}

inline StringResponse make_jsonrpc_error_response(const StringRequest& request,
                                                  http::status status_code, int error_code,
                                                  std::string_view error_message) {
    // It names no request, so `id` is left out, as MCP's schema has it.
    nlohmann::json error_body = {{"jsonrpc", "2.0"},
                                 {"error", {{"code", error_code}, {"message", error_message}}}};
    return make_json_response(request, status_code, error_body.dump());
}

/// A JSON-RPC error answering the request @p id.
inline StringResponse make_jsonrpc_error_response(const StringRequest& request,
                                                  http::status status_code, int error_code,
                                                  std::string_view error_message,
                                                  const nlohmann::json& id) {
    nlohmann::json error_body = {
        {"jsonrpc", "2.0"}, {"error", {{"code", error_code}, {"message", error_message}}}, {"id", id}};
    return make_json_response(request, status_code, error_body.dump());
}

inline StringResponse make_empty_json_response(const StringRequest& request, http::status status_code) {
    auto response = make_json_response(request, status_code, "");
    response.content_length(0);
    return response;
}

inline StringResponse make_sse_response(const StringRequest& request, const SseEventList& events) {
    std::string sse_body;
    for (const auto& [id, data] : events) {
        append_sse_event(sse_body, id, data);
    }
    StringResponse response{http::status::ok, request.version()};
    response.set(http::field::server, "mcp-cpp-sdk");
    response.set(http::field::content_type, "text/event-stream");
    response.set(http::field::cache_control, "no-cache");
    response.keep_alive(request.keep_alive());
    response.body() = std::move(sse_body);
    response.prepare_payload();
    return response;
}

}  // namespace detail_session_mgr

// ============================================================================
// Impl
// ============================================================================

struct StreamableHttpSessionManager::Impl {
    using Connection = beast::tcp_stream;
    using SessionMap =
        std::unordered_map<std::string, std::shared_ptr<detail_session_mgr::SessionRuntime>>;

    std::string host;
    unsigned short port;
    boost::asio::any_io_executor executor;
    boost::asio::any_io_executor tool_executor_;
    boost::asio::strand<boost::asio::any_io_executor> listener_strand;
    boost::asio::steady_timer expiry_timer{listener_strand};
    boost::asio::ip::tcp::acceptor acceptor;

    StreamableHttpSessionManager::ServerFactory factory;
    StreamableHttpSessionManager::CustomRequestHandler custom_handler;
    std::size_t event_store_capacity;
    std::shared_ptr<std::atomic<bool>> json_only_mode_ = std::make_shared<std::atomic<bool>>(false);
    bool stateless_json_mode_{false};
    bool allow_all_origins_{false};
    std::unordered_set<std::string> allowed_origins_;
    BearerTokenValidator bearer_token_validator_;
    AsyncBearerTokenValidator async_bearer_token_validator_;
    std::size_t max_request_body_bytes_{constants::g_default_max_request_body_bytes};
    BearerChallengeConfig bearer_challenge_;
    /// Rendered once when the challenge or the metadata changes, so serving a 401 never formats.
    std::string www_authenticate_value_{"Bearer"};
    std::optional<ProtectedResourceMetadataConfig> protected_resource_metadata_;
    std::string protected_resource_metadata_body_;
    /// The resolved serving path: `path` when set, else derived from `resource`.
    std::string protected_resource_metadata_path_;
    std::unordered_set<std::string> unauthenticated_paths_;

    std::atomic<bool> closed{false};
    mutable std::mutex configuration_mutex_;
    bool listening_started_{false};
    mutable std::mutex connections_mutex_;
    std::unordered_set<std::shared_ptr<Connection>> active_connections_;
    mutable std::shared_mutex sessions_mutex_;
    SessionMap sessions;
    /// Guarded by configuration_mutex_.
    SessionLimits session_limits;
    /// Guarded by configuration_mutex_.
    HttpConnectionLimits connection_limits_;
    detail::HttpListenerBudget budget_;
    // Answers every 2026-07-28 request, which belongs to no session. Built from the factory on the
    // first one unless set_modern_server() supplied it, so a peer that needs no session cannot make
    // the factory run per request.
    std::mutex modern_server_mutex_;
    std::shared_ptr<Server> modern_server_;
    std::optional<ServerProtocolPolicy> protocol_policy_;
    std::atomic<std::uint64_t> header_mismatch_rejections_{0};
    StreamOptions stream_options_;
    struct StreamCounters {
        std::atomic<std::uint64_t> streams_opened{0};
        std::atomic<std::uint64_t> disconnect_cancels{0};
        std::atomic<std::uint64_t> half_close_probes{0};
        std::atomic<std::uint64_t> dropped_notifications{0};
    };
    std::shared_ptr<StreamCounters> stream_counters_ = std::make_shared<StreamCounters>();

    /// A request handled by handle_request: a complete response, the answer to a 2026-07-28
    /// request that serve_modern_exchange writes as it is produced, or a stream of a legacy session
    /// that serve_legacy_stream writes as its events come.
    using Reply = std::variant<StringResponse, std::shared_ptr<detail_session_mgr::ModernExchange>,
                               std::shared_ptr<detail_session_mgr::LegacyStream>>;

    Impl(const boost::asio::any_io_executor& exec, std::string host_arg, unsigned short port_arg,
         StreamableHttpSessionManager::ServerFactory factory_arg, std::size_t capacity)
        : host(std::move(host_arg)),
          port(port_arg),
          executor(exec),
          listener_strand(boost::asio::make_strand(exec)),
          acceptor(listener_strand),
          factory(std::move(factory_arg)),
          event_store_capacity(capacity) {
        boost::system::error_code ec;
        const auto bind_address = boost::asio::ip::make_address(host, ec);
        if (ec) {
            throw std::runtime_error("Invalid bind address: " + host);
        }

        const auto endpoint = boost::asio::ip::tcp::endpoint(bind_address, port);
        (void)acceptor.open(endpoint.protocol(), ec);
        if (ec) {
            throw std::runtime_error("Failed to open HTTP acceptor: " + ec.message());
        }

        (void)acceptor.set_option(boost::asio::socket_base::reuse_address(true), ec);
        if (ec) {
            throw std::runtime_error("Failed to set reuse_address: " + ec.message());
        }

        (void)acceptor.bind(endpoint, ec);
        if (ec) {
            throw std::runtime_error("Failed to bind HTTP acceptor: " + ec.message());
        }

        (void)acceptor.listen(boost::asio::socket_base::max_listen_connections, ec);
        if (ec) {
            throw std::runtime_error("Failed to listen on HTTP acceptor: " + ec.message());
        }
    }

    void ensure_configurable() const {
        if (listening_started_ || closed.load(std::memory_order_acquire)) {
            throw std::logic_error(
                "StreamableHttpSessionManager configuration must be set before listen()");
        }
    }

    bool begin_listening() {
        std::lock_guard lock(configuration_mutex_);
        if (closed.load(std::memory_order_acquire)) {
            return false;
        }
        if (listening_started_) {
            throw std::logic_error("StreamableHttpSessionManager::listen() may only be called once");
        }
        listening_started_ = true;
        return true;
    }

    static void close_connection_now(const std::shared_ptr<Connection>& connection) {
        boost::system::error_code ignored;
        (void)connection->socket().cancel(ignored);
        (void)connection->socket().shutdown(boost::asio::ip::tcp::socket::shutdown_both, ignored);
        (void)connection->socket().close(ignored);
    }

    static void request_connection_close(const std::shared_ptr<Connection>& connection) {
        boost::asio::post(connection->get_executor(),
                          [connection]() { close_connection_now(connection); });
    }

    bool register_connection(const std::shared_ptr<Connection>& connection) {
        std::lock_guard lock(connections_mutex_);
        if (closed.load(std::memory_order_acquire)) {
            return false;
        }
        active_connections_.insert(connection);
        return true;
    }

    void unregister_connection(const std::shared_ptr<Connection>& connection) {
        std::lock_guard lock(connections_mutex_);
        active_connections_.erase(connection);
    }

    void close_active_connections() {
        std::vector<std::shared_ptr<Connection>> connections;
        {
            std::lock_guard lock(connections_mutex_);
            connections.reserve(active_connections_.size());
            connections.insert(connections.end(), active_connections_.begin(),
                               active_connections_.end());
        }
        for (const auto& connection : connections) {
            request_connection_close(connection);
        }
    }

    // Session lifecycle

    /// A Server from the factory, with the protocol policy applied when one is set.
    std::unique_ptr<Server> build_server(const boost::asio::any_io_executor& server_executor) const {
        auto server = factory(server_executor);
        if (server && protocol_policy_) {
            server->set_protocol_policy(*protocol_policy_);
        }
        // The server's own work runs on the HTTP executor; only a tool handler, which may block,
        // moves to the tool executor.
        if (server && tool_executor_) {
            server->set_handler_executor(tool_executor_);
        }
        return server;
    }

    std::shared_ptr<Server> modern_server() {
        std::lock_guard lock(modern_server_mutex_);
        if (!modern_server_) {
            modern_server_ = build_server(executor);
        }
        return modern_server_;
    }

    std::shared_ptr<detail_session_mgr::SessionRuntime> create_session(
        boost::asio::any_io_executor conn_strand, std::string principal) {
        if (closed.load(std::memory_order_acquire)) {
            return nullptr;
        }

        auto session_id = detail_session_mgr::generate_session_id();

        auto client_mem = std::make_shared<MemoryTransport>(conn_strand);
        auto server_mem = std::make_shared<MemoryTransport>(conn_strand);
        client_mem->set_peer(server_mem);
        server_mem->set_peer(client_mem);
        auto server = build_server(executor);

        auto runtime =
            std::make_shared<detail_session_mgr::SessionRuntime>(session_id, event_store_capacity);
        runtime->client_transport = client_mem;
        runtime->principal = std::move(principal);

        {
            std::unique_lock lock(sessions_mutex_);
            if (closed.load(std::memory_order_acquire)) {
                client_mem->close();
                return nullptr;
            }
            sessions.emplace(session_id, runtime);
        }

        std::shared_ptr<ITransport> server_transport = server_mem;
        auto server_exec = executor;

        boost::asio::co_spawn(
            conn_strand,
            [srv = std::move(server), transport = std::move(server_transport),
             exec = server_exec]() mutable -> Task<void> {
                co_await srv->run(std::move(transport), exec);
            },
            boost::asio::detached);

        boost::asio::co_spawn(
            conn_strand,
            read_outbound_messages(runtime, json_only_mode_, stream_options_.max_queued_notifications,
                                   stream_counters_),
            boost::asio::detached);
        return runtime;
    }

    std::shared_ptr<detail_session_mgr::SessionRuntime> find_session(const std::string& session_id) {
        std::shared_lock lock(sessions_mutex_);
        auto it = sessions.find(session_id);
        if (it == sessions.end()) {
            return nullptr;
        }
        it->second->touch();
        return it->second;
    }

    bool at_session_limit() const {
        SessionLimits limits;
        {
            std::lock_guard lock(configuration_mutex_);
            limits = session_limits;
        }
        std::shared_lock lock(sessions_mutex_);
        return sessions.size() >= limits.max_sessions;
    }

    /// Ends the sessions nobody has used for the idle timeout, as a client may leave without a
    /// DELETE. Runs for as long as the manager listens.
    static Task<void> expire_idle_sessions(std::shared_ptr<Impl> impl) {
        for (;;) {
            SessionLimits limits;
            {
                std::lock_guard lock(impl->configuration_mutex_);
                limits = impl->session_limits;
            }
            constexpr auto g_longest_sweep = std::chrono::seconds(30);
            constexpr auto g_shortest_sweep = std::chrono::milliseconds(10);
            const auto sweep = limits.idle_timeout > std::chrono::milliseconds::zero()
                                   ? std::chrono::milliseconds(limits.idle_timeout / 4)
                                   : std::chrono::milliseconds(g_longest_sweep);
            impl->expiry_timer.expires_after(
                std::clamp<std::chrono::milliseconds>(sweep, g_shortest_sweep, g_longest_sweep));
            boost::system::error_code ignored;
            co_await impl->expiry_timer.async_wait(
                boost::asio::redirect_error(boost::asio::use_awaitable, ignored));
            if (impl->closed.load(std::memory_order_acquire)) {
                co_return;
            }
            if (limits.idle_timeout <= std::chrono::milliseconds::zero()) {
                continue;
            }
            std::vector<std::string> idle;
            {
                std::shared_lock lock(impl->sessions_mutex_);
                for (const auto& [id, session] : impl->sessions) {
                    if (session->idle_for(limits.idle_timeout)) {
                        idle.push_back(id);
                    }
                }
            }
            for (const auto& id : idle) {
                impl->destroy_session(id);
            }
        }
    }

    void destroy_session(const std::string& session_id) {
        std::shared_ptr<detail_session_mgr::SessionRuntime> session;
        {
            std::unique_lock lock(sessions_mutex_);
            auto it = sessions.find(session_id);
            if (it == sessions.end()) {
                return;
            }
            session = std::move(it->second);
            sessions.erase(it);
        }

        end_streams(*session);
        if (session->client_transport) {
            session->client_transport->close();
        }
    }

    /// Ends every stream of @p session: a POST still waiting is answered with an error, and the
    /// GET stream closes.
    static void end_streams(detail_session_mgr::SessionRuntime& session) {
        std::vector<std::shared_ptr<boost::asio::steady_timer>> timers;
        {
            std::lock_guard lock(session.response_mutex);
            timers.reserve(session.pending_responses.size() + 1);
            for (auto& [key, pending] : session.pending_responses) {
                (void)key;
                pending.response_ready = true;
                timers.push_back(pending.ready_timer);
            }
            session.ended = true;
            if (session.get_timer) {
                timers.push_back(session.get_timer);
            }
        }
        for (const auto& timer : timers) {
            signal_timer(timer);
        }
    }

    // HTTP connection / request handling

    /// A session belongs to the principal that created it, when a validator checks the tokens.
    /// Without one the Authorization header is not checked here, and a client may change it.
    bool session_belongs_to(const detail_session_mgr::SessionRuntime& session,
                            const std::string& principal) const {
        return (!bearer_token_validator_ && !async_bearer_token_validator_) ||
               session.principal == principal;
    }

    HttpConnectionLimits current_connection_limits() const {
        std::lock_guard lock(configuration_mutex_);
        return connection_limits_;
    }

    Task<void> handle_connection(const std::shared_ptr<Connection>& connection,
                                 boost::asio::any_io_executor conn_strand) {
        namespace beast = boost::beast;

        auto& stream = *connection;
        beast::flat_buffer request_buffer;
        std::unique_ptr<Server> stateless_server;
        // Set once a watch has read the client's end of the stream. That end is consumed: a later
        // read on this connection would wait for an event that already happened.
        bool client_finished_sending = false;
        // The principal the bearer token validator named for the request being served.
        std::optional<std::string> principal_id;
        const detail::RefuseBeforeBody refuse =
            [this, &principal_id](const StringRequest& head) -> Task<std::optional<StringResponse>> {
            co_return co_await refuse_before_body(head, principal_id);
        };

        for (;;) {
            const auto limits = current_connection_limits();
            principal_id.reset();
            detail::RequestParser parser;
            parser.body_limit(max_request_body_bytes_);
            detail::BudgetLease bytes(budget_.bytes, limits.max_request_bytes);
            if (client_finished_sending) {
                // Only what the watch read ahead can still arrive: serve the requests it holds
                // whole, and end the connection at the first that is not.
                boost::system::error_code parse_error;
                // Without eager, put() stops after the header and a whole buffered body would
                // read as an incomplete request.
                parser.eager(true);
                request_buffer.consume(parser.put(request_buffer.data(), parse_error));
                if (parse_error == http::error::body_limit) {
                    // The parser cannot resynchronize after a truncated body, so the connection
                    // ends with this answer rather than reading another request from it.
                    auto too_large = detail::make_limit_response(StringRequest{parser.get().base()},
                                                                 http::status::payload_too_large,
                                                                 "Request body too large");
                    co_await detail::write_final_response(stream, std::move(too_large));
                    break;
                }
                if (parse_error || !parser.is_done()) {
                    break;
                }
                const StringRequest head{parser.get().base()};
                if (auto refusal = co_await refuse_before_body(head, principal_id)) {
                    co_await detail::write_final_response(stream, std::move(*refusal));
                    break;
                }
                if (!bytes.grow_to(parser.get().body().size())) {
                    auto busy = detail::make_limit_response(head, http::status::service_unavailable,
                                                            "Server busy");
                    co_await detail::write_final_response(stream, std::move(busy));
                    break;
                }
            } else if (!co_await detail::read_request_within_limits(stream, request_buffer, parser,
                                                                    limits, max_request_body_bytes_,
                                                                    bytes, refuse)) {
                break;
            }
            StringRequest request = detail::take_request(parser);

            if (closed.load(std::memory_order_acquire)) {
                break;
            }

            detail::BudgetLease slot(budget_.requests, limits.max_requests);
            if (!slot.grow_to(1)) {
                auto busy = detail_session_mgr::make_error_response(
                    request, http::status::service_unavailable, "Too many requests");
                const bool keep_alive = busy.keep_alive();
                co_await http::async_write(stream, busy, boost::asio::use_awaitable);
                if (!keep_alive) {
                    break;
                }
                continue;
            }
            auto reply = co_await handle_request(request, conn_strand, stateless_server, bytes,
                                                 std::move(principal_id));
            if (closed.load(std::memory_order_acquire)) {
                if (auto* exchange = std::get_if<1>(&reply)) {
                    (*exchange)->cancel();
                }
                break;
            }
            if (auto* exchange = std::get_if<1>(&reply)) {
                const bool keep_serving = co_await serve_modern_exchange(
                    stream, request_buffer, request, *exchange, client_finished_sending);
                if (!keep_serving) {
                    break;
                }
                continue;
            }
            if (auto* legacy = std::get_if<2>(&reply)) {
                if (!co_await serve_legacy_stream(stream, request, *legacy)) {
                    break;
                }
                continue;
            }
            auto& response = std::get<0>(reply);
            const bool keep_connection_alive = response.keep_alive();
            if (!co_await detail::write_response_within_limits(stream, response, limits) ||
                !keep_connection_alive) {
                break;
            }
        }

        boost::system::error_code shutdown_error;
        (void)stream.socket().shutdown(boost::asio::ip::tcp::socket::shutdown_send, shutdown_error);
    }

    /// @brief Render the challenge a 401 will carry, borrowing the metadata URL when unset.
    std::string render_challenge(const BearerChallengeConfig& challenge) const {
        auto effective = challenge;
        if (effective.resource_metadata.empty() && protected_resource_metadata_.has_value()) {
            effective.resource_metadata =
                protected_resource_metadata_url(*protected_resource_metadata_);
        }
        return format_www_authenticate(effective);
    }

    bool is_unauthenticated_path(const StringRequest& request) const {
        if (unauthenticated_paths_.empty()) {
            return false;
        }
        const auto path = http_request_path(detail::to_std_string_view(request.target()));
        return unauthenticated_paths_.contains(std::string(path));
    }

    /// @brief Answer a GET for the configured RFC 9728 document, which needs no bearer token.
    std::optional<StringResponse> serve_protected_resource_metadata(
        const StringRequest& request) const {
        if (!protected_resource_metadata_.has_value() || request.method() != http::verb::get) {
            return std::nullopt;
        }
        if (http_request_path(detail::to_std_string_view(request.target())) !=
            protected_resource_metadata_path_) {
            return std::nullopt;
        }
        return detail_session_mgr::make_json_response(request, http::status::ok,
                                                      protected_resource_metadata_body_);
    }

    static std::string_view request_authorization(const StringRequest& request) {
        const auto authorization_it = request.find(http::field::authorization);
        if (authorization_it == request.end()) {
            return {};
        }
        return detail::to_std_string_view(authorization_it->value());
    }

    /// Origin and bearer checks: they need only the headers, so a request they refuse never has its
    /// body read. Sets @p principal_id to the principal the validator names, if it names one.
    Task<std::optional<StringResponse>> refuse_before_body(const StringRequest& request,
                                                           std::optional<std::string>& principal_id) {
        // A closed transport answers nothing more, so a request is not worth a token check.
        if (closed.load(std::memory_order_acquire)) {
            co_return std::nullopt;
        }
        const auto origin_it = request.find(http::field::origin);
        if (origin_it != request.end() && !allow_all_origins_ &&
            !allowed_origins_.contains(std::string(origin_it->value()))) {
            co_return detail_session_mgr::make_error_response(request, http::status::forbidden,
                                                              "Origin not allowed");
        }

        const bool unauthenticated_path =
            is_unauthenticated_path(request) || serve_protected_resource_metadata(request).has_value();
        if (!unauthenticated_path && (bearer_token_validator_ || async_bearer_token_validator_)) {
            const auto token = http_bearer_token(request_authorization(request));
            // The synchronous path neither copies the token nor suspends; only an async validator
            // pays for either. A validator that throws has failed to decide, which is the server's
            // failure rather than the client's: answered 500, not with a challenge that would send
            // the client into re-authorization.
            std::optional<TokenValidation> validation;
            bool validator_failed = false;
            try {
                if (token.empty()) {
                    validation = TokenValidation(false);
                } else if (async_bearer_token_validator_) {
                    validation = co_await async_bearer_token_validator_(std::string(token));
                } else {
                    validation = bearer_token_validator_(token);
                }
            } catch (...) {
                validator_failed = true;
            }
            if (validator_failed) {
                co_return detail_session_mgr::make_error_response(
                    request, http::status::internal_server_error, "Bearer token validation failed");
            }
            if (!validation->accepted) {
                auto response = detail_session_mgr::make_error_response(
                    request, http::status::unauthorized, "Invalid bearer token");
                response.set(http::field::www_authenticate, www_authenticate_value_);
                co_return response;
            }
            principal_id = std::move(validation->principal_id);
        }

        co_return std::nullopt;
    }

    Task<Reply> handle_request(const StringRequest& request, boost::asio::any_io_executor conn_strand,
                               std::unique_ptr<Server>& stateless_server, detail::BudgetLease& bytes,
                               std::optional<std::string> principal_id) {
        if (closed.load(std::memory_order_acquire)) {
            co_return detail_session_mgr::make_error_response(
                request, http::status::service_unavailable, "Transport closed");
        }

        if (auto document = serve_protected_resource_metadata(request)) {
            co_return std::move(*document);
        }

        if (custom_handler) {
            auto custom_response = custom_handler(request);
            if (custom_response.has_value()) {
                co_return std::move(*custom_response);
            }
        }

        if (is_unauthenticated_path(request)) {
            // An exempt path is excluded from MCP dispatch, not merely excused from the bearer
            // check. Serving MCP here would answer it with no authentication at all, so once the
            // metadata route and the custom handler have both declined, nothing is left to answer
            // it.
            co_return detail_session_mgr::make_error_response(request, http::status::not_found,
                                                              "Not found");
        }

        if (request.method() == http::verb::post) {
            co_return co_await handle_post(request, conn_strand, stateless_server, bytes,
                                           std::move(principal_id));
        }

        // A 2026-07-28 client has no session to read from or end, so GET and DELETE are not part
        // of its protocol. A request naming a session is a legacy client's, unless no legacy
        // client is served, and then no session can exist.
        const bool serves_legacy = !protocol_policy_ || protocol_policy_->serve_legacy;
        if (const auto version = detail::http_header(request, "MCP-Protocol-Version");
            !serves_legacy || (version && !is_legacy_protocol_version(*version) &&
                               request.find("Mcp-Session-Id") == request.end())) {
            auto response = detail_session_mgr::make_error_response(
                request, http::status::method_not_allowed, "Method not allowed");
            response.set(http::field::allow, "POST");
            co_return response;
        }

        if (stateless_json_mode_) {
            auto response = detail_session_mgr::make_error_response(
                request, http::status::method_not_allowed, "Method not allowed");
            response.set(http::field::allow, "POST");
            co_return response;
        }

        if (request.method() == http::verb::get) {
            co_return co_await handle_get(
                request, detail::request_principal(principal_id, request_authorization(request)), bytes,
                conn_strand);
        }

        if (request.method() == http::verb::delete_) {
            co_return co_await handle_delete(
                request, detail::request_principal(principal_id, request_authorization(request)));
        }

        auto response = detail_session_mgr::make_error_response(
            request, http::status::method_not_allowed, "Method not allowed");
        response.set(http::field::allow, "GET, POST, DELETE");
        co_return response;
    }

    StringResponse initialize_not_served_response(const StringRequest& request,
                                                  const nlohmann::json& request_json) const {
        auto served = served_protocol_versions(protocol_policy_.value_or(ServerProtocolPolicy{}));
        const auto id = request_json.value("id", nlohmann::json(nullptr));
        nlohmann::json error_body = {{"jsonrpc", "2.0"},
                                     {"error",
                                      {{"code", g_METHOD_NOT_FOUND},
                                       {"message", detail::initialize_not_served_message(served)},
                                       {"data", {{"supported", std::move(served)}}}}}};
        if (detail::is_request_id(id)) {
            error_body["id"] = id;
        }
        return detail_session_mgr::make_json_response(request, http::status::not_found,
                                                      error_body.dump());
    }

    bool has_valid_protocol_header(const StringRequest& request) const {
        const auto protocol_header_it = request.find("MCP-Protocol-Version");
        return protocol_header_it == request.end() ||
               is_legacy_protocol_version(
                   detail_session_mgr::protocol_header_value(protocol_header_it));
    }

    /// Dispatching on another executor is spelled as a named coroutine rather than a lambda handed
    /// to co_spawn. A closure is built in the caller's frame and carries its captures there, and on
    /// GCC 11 an object that lives in a coroutine frame across a suspension can be corrupted -- see
    /// the SSO note in docs/contributing.rst. Taking the arguments by value moves them into this
    /// coroutine's own frame instead, leaving nothing of the request in the caller's.
    static Task<std::string> dispatch_on_executor(Server* server, nlohmann::json request_json,
                                                  std::string principal) {
        RequestContext request_context;
        request_context.principal = std::move(principal);
        co_return co_await server->dispatch_request_direct(std::move(request_json),
                                                           std::move(request_context));
    }

    Task<StringResponse> handle_stateless_post(const StringRequest& request,
                                               nlohmann::json request_json,
                                               std::unique_ptr<Server>& stateless_server,
                                               std::string principal) {
        if (closed.load(std::memory_order_acquire)) {
            co_return detail_session_mgr::make_error_response(
                request, http::status::service_unavailable, "Transport closed");
        }

        const bool is_initialize = detail_session_mgr::is_initialize_request(request_json);
        if (!is_initialize && !has_valid_protocol_header(request)) {
            co_return detail_session_mgr::make_error_response(request, http::status::bad_request,
                                                              "Invalid MCP-Protocol-Version header");
        }

        if (!request_json.contains("id") || !request_json.contains("method")) {
            co_return detail_session_mgr::make_empty_json_response(request, http::status::accepted);
        }

        const auto dispatch_executor = executor;
        if (!stateless_server) {
            stateless_server = build_server(dispatch_executor);
        }

        auto response_body = co_await boost::asio::co_spawn(
            dispatch_executor,
            dispatch_on_executor(stateless_server.get(), std::move(request_json), std::move(principal)),
            boost::asio::use_awaitable);
        co_return detail_session_mgr::make_json_response(request, http::status::ok,
                                                         std::move(response_body));
    }

    /// Serves a 2026-07-28 request, which carries everything it needs and belongs to no session:
    /// its mirrored headers are checked, it is dispatched to the shared modern server, and its
    /// JSON-RPC errors are answered with the HTTP status the specification gives them. No session is
    /// created or looked up, and Mcp-Session-Id is neither read nor sent.
    ///
    /// @p check_headers is false for a request that names no 2026-07-28 version and reaches here
    /// only because legacy clients are not served; the server refuses it.
    Task<Reply> handle_modern_post(const StringRequest& request, nlohmann::json request_json,
                                   bool check_headers, boost::asio::any_io_executor conn_strand,
                                   std::string principal) {
        if (closed.load(std::memory_order_acquire)) {
            co_return detail_session_mgr::make_error_response(
                request, http::status::service_unavailable, "Transport closed");
        }

        // 2026-07-28 defines no notification a client sends over HTTP, so one is accepted and
        // dropped.
        if (!request_json.contains("id")) {
            co_return detail_session_mgr::make_empty_json_response(request, http::status::accepted);
        }
        if (!detail::is_request_id(request_json.at("id"))) {
            co_return detail_session_mgr::make_jsonrpc_error_response(
                request, http::status::bad_request, g_INVALID_REQUEST,
                "JSON-RPC request id must be a string or an integer within int64_t");
        }

        if (check_headers) {
            if (auto mismatch = detail::modern_header_mismatch(request, request_json)) {
                header_mismatch_rejections_.fetch_add(1, std::memory_order_relaxed);
                co_return detail_session_mgr::make_jsonrpc_error_response(
                    request, http::status::bad_request, g_HEADER_MISMATCH,
                    "Header mismatch: " + *mismatch, request_json.at("id"));
            }
            if (auto refusal = detail::missing_modern_version(request_json)) {
                co_return detail_session_mgr::make_jsonrpc_error_response(
                    request, http::status::bad_request, g_INVALID_PARAMS, std::move(*refusal),
                    request_json.at("id"));
            }
        }

        const auto accept_it = request.find(http::field::accept);
        const bool streams = !json_only_mode_->load(std::memory_order_acquire) &&
                             request.version() >= 11 && accept_it != request.end() &&
                             detail::to_std_string_view(accept_it->value()).find("text/event-stream") !=
                                 std::string_view::npos;
        auto exchange = std::make_shared<detail_session_mgr::ModernExchange>(
            conn_strand, stream_options_.max_queued_notifications, streams);

        RequestContext request_context;
        request_context.cancelled = exchange->cancelled;
        request_context.principal = std::move(principal);
        if (check_headers) {
            request_context.param_headers = detail::mcp_param_headers(request);
        }
        request_context.watch_cancel = [weak = std::weak_ptr(exchange)](std::function<void()> hook) {
            if (auto target = weak.lock()) {
                target->set_on_cancel(std::move(hook));
            }
        };
        if (streams) {
            request_context.notifications = [weak = std::weak_ptr(exchange),
                                             counters = stream_counters_](std::string_view message) {
                if (auto target = weak.lock();
                    target && !target->push_notification(std::string(message))) {
                    counters->dropped_notifications.fetch_add(1, std::memory_order_relaxed);
                }
                return notification_queued();
            };
        }

        // The server answers on the dispatch executor while the connection watches the client, so
        // a client that goes away cancels the request.
        const auto dispatch_executor = executor;
        auto request_id = request_json.at("id");
        boost::asio::co_spawn(
            dispatch_executor,
            dispatch_modern(modern_server(), std::move(request_json), std::move(request_context)),
            [exchange, request_id = std::move(request_id)](std::exception_ptr error, std::string wire) {
                if (error) {
                    wire = nlohmann::json{{"jsonrpc", "2.0"},
                                          {"id", request_id},
                                          {"error",
                                           {{"code", g_INTERNAL_ERROR}, {"message", "Internal error"}}}}
                               .dump();
                }
                exchange->finish(std::move(wire));
            });
        co_return exchange;
    }

    static Task<void> notification_queued() { co_return; }

    /// Takes the server by value, so it outlives a connection that ends before the answer does.
    static Task<std::string> dispatch_modern(std::shared_ptr<Server> server,
                                             nlohmann::json request_json,
                                             RequestContext request_context) {
        co_return co_await server->dispatch_request_direct(std::move(request_json),
                                                           std::move(request_context));
    }

    /// Bytes a watch reads ahead while a request runs; they belong to the client's next request.
    static constexpr std::size_t g_watch_read_size = 4096;
    /// Read-ahead held at once; past this the watch stops until the request is answered.
    static constexpr std::size_t g_max_read_ahead = 64 * 1024;

    static Task<bool> write_chunk(Connection& stream, const std::string& text) {
        boost::system::error_code error;
        co_await boost::asio::async_write(
            stream, http::make_chunk(boost::asio::buffer(text)),
            boost::asio::redirect_error(boost::asio::use_awaitable, error));
        co_return !error;
    }

    static Task<bool> open_stream(Connection& stream, const StringRequest& request) {
        http::response<http::empty_body> header{http::status::ok, request.version()};
        header.set(http::field::server, "mcp-cpp-sdk");
        header.set(http::field::content_type, "text/event-stream");
        header.set(http::field::cache_control, "no-cache");
        header.set("X-Accel-Buffering", "no");
        header.keep_alive(request.keep_alive());
        header.chunked(true);
        http::response_serializer<http::empty_body> serializer{header};
        boost::system::error_code error;
        co_await http::async_write_header(
            stream, serializer, boost::asio::redirect_error(boost::asio::use_awaitable, error));
        co_return !error;
    }

    /// Waits for a wake for at most @p timeout; returns true when the time ran out.
    static Task<bool> wait_for_signal(detail_session_mgr::ModernExchange& exchange,
                                      std::optional<std::chrono::milliseconds> timeout) {
        if (exchange.woken) {
            exchange.woken = false;
            co_return false;
        }
        if (timeout) {
            exchange.signal.expires_after(*timeout);
        } else {
            exchange.signal.expires_at(std::chrono::steady_clock::time_point::max());
        }
        boost::system::error_code error;
        co_await exchange.signal.async_wait(
            boost::asio::redirect_error(boost::asio::use_awaitable, error));
        exchange.woken = false;
        co_return !error;
    }

    /// Whether the client reset a connection whose sending side it had already closed. epoll ends
    /// the wait for a socket error on a reset, but kqueue and the Windows reactor end it only for
    /// out-of-band data, so the socket is asked instead: for an error it has recorded, and with a
    /// peek, which past the client's end of stream reads end of file until a reset.
    static bool peer_reset(Connection& stream) {
        return detail::probe_peer(stream.socket()) == detail::PeerState::eReset;
    }

    /// Writes the answer to a 2026-07-28 request as it is produced. Plain JSON, with the status the
    /// specification gives its error, unless a notification arrives first and the client accepts
    /// SSE: then an SSE stream carrying the notifications and finally the response. Meanwhile one
    /// read on the connection watches the client. Bytes it reads are the client's next request and
    /// stay in @p buffer; a reset, or a failed write, means the client has gone and cancels the
    /// request. A half-close is not a disconnect, since the client may still be reading: the
    /// stream is opened and probed until a write fails or the answer is complete. Without a stream
    /// the socket is checked for a reset at the same interval.
    ///
    /// @return Whether the connection may carry another request.
    Task<bool> serve_modern_exchange(Connection& stream, beast::flat_buffer& buffer,
                                     const StringRequest& request,
                                     std::shared_ptr<detail_session_mgr::ModernExchange> exchange,
                                     bool& client_finished_sending) {
        struct Watch {
            bool armed{false};
            bool completed{false};
            boost::system::error_code error;
        };
        auto watch = std::make_shared<Watch>();
        const auto arm_watch = [&stream, &buffer, &watch, &exchange] {
            watch->armed = true;
            watch->completed = false;
            stream.async_read_some(
                buffer.prepare(g_watch_read_size),
                boost::asio::bind_executor(
                    exchange->strand, [watch, exchange, &buffer](const boost::system::error_code& error,
                                                                 std::size_t bytes) {
                        if (!error) {
                            buffer.commit(bytes);
                        }
                        watch->armed = false;
                        watch->completed = true;
                        watch->error = error;
                        exchange->wake_on_strand();
                    }));
        };

        // Once the client's end of the stream is read there is nothing more to read, but a reset
        // or this side closing the socket still ends the wait, and so wakes the loop.
        const auto arm_error_wait = [&stream, &watch, &exchange] {
            watch->armed = true;
            watch->completed = false;
            stream.socket().async_wait(
                boost::asio::ip::tcp::socket::wait_error,
                boost::asio::bind_executor(
                    exchange->strand, [watch, exchange](const boost::system::error_code& error) {
                        watch->armed = false;
                        watch->completed = true;
                        watch->error = error ? error : boost::asio::error::connection_reset;
                        exchange->wake_on_strand();
                    }));
        };

        bool stream_open = false;
        bool half_closed = client_finished_sending;
        // The client sent more than the server will hold, so the read that watches it has stopped.
        bool read_ahead_full = false;
        bool client_gone = false;
        if (half_closed) {
            arm_error_wait();
        } else {
            arm_watch();
        }

        for (;;) {
            std::deque<std::string> notifications;
            std::optional<std::string> response;
            {
                std::lock_guard lock(exchange->mutex);
                notifications.swap(exchange->notifications);
                response.swap(exchange->response);
            }

            if (closed.load(std::memory_order_acquire)) {
                client_gone = true;
                break;
            }

            if (!notifications.empty() && exchange->streams) {
                if (!stream_open) {
                    const bool opened = co_await open_stream(stream, request);
                    if (!opened) {
                        client_gone = true;
                        break;
                    }
                    stream_open = true;
                    stream_counters_->streams_opened.fetch_add(1, std::memory_order_relaxed);
                }
                std::string events;
                for (const auto& notification : notifications) {
                    events += "data: ";
                    events += notification;
                    events += "\n\n";
                }
                const bool written = co_await write_chunk(stream, events);
                if (!written) {
                    client_gone = true;
                    break;
                }
            }

            if (response) {
                if (watch->armed) {
                    boost::system::error_code ignored;
                    (void)stream.socket().cancel(ignored);
                    while (watch->armed) {
                        co_await wait_for_signal(*exchange, std::nullopt);
                    }
                }
                if (watch->completed && watch->error == boost::asio::error::eof) {
                    half_closed = true;
                    client_finished_sending = true;
                }
                const auto error_code = detail::serialized_response_error_code(*response);
                if (error_code == g_HEADER_MISMATCH) {
                    header_mismatch_rejections_.fetch_add(1, std::memory_order_relaxed);
                }
                if (stream_open) {
                    const bool written = co_await write_chunk(stream, "data: " + *response + "\n\n");
                    boost::system::error_code error;
                    if (written) {
                        co_await boost::asio::async_write(
                            stream, http::make_chunk_last(),
                            boost::asio::redirect_error(boost::asio::use_awaitable, error));
                    }
                    co_return written && !error && request.keep_alive() &&
                        (!half_closed || buffer.size() != 0);
                }
                const auto status =
                    error_code ? detail::http_status_for_error(*error_code) : http::status::ok;
                auto plain =
                    detail_session_mgr::make_json_response(request, status, std::move(*response));
                boost::system::error_code error;
                co_await http::async_write(
                    stream, plain, boost::asio::redirect_error(boost::asio::use_awaitable, error));
                co_return !error && plain.keep_alive() && (!half_closed || buffer.size() != 0);
            }

            if (watch->completed) {
                watch->completed = false;
                if (!watch->error) {
                    if (buffer.size() < g_max_read_ahead) {
                        arm_watch();
                    } else {
                        read_ahead_full = true;
                        arm_error_wait();
                    }
                } else if (watch->error == boost::asio::error::eof) {
                    half_closed = true;
                    client_finished_sending = true;
                    arm_error_wait();
                } else {
                    client_gone = true;
                    break;
                }
            }

            // A stream's keep-alives find a client that left; anything else has to ask the socket.
            const bool probe_socket = half_closed || (read_ahead_full && !stream_open);
            std::optional<std::chrono::milliseconds> timeout;
            if (probe_socket) {
                timeout = stream_options_.disconnect_probe_interval;
            } else if (stream_open) {
                timeout = stream_options_.keepalive_interval;
            }
            const bool signalled = co_await wait_for_signal(*exchange, timeout);
            if (!signalled) {
                continue;
            }
            if (half_closed && exchange->streams) {
                if (!stream_open) {
                    const bool opened = co_await open_stream(stream, request);
                    if (!opened) {
                        client_gone = true;
                        break;
                    }
                    stream_open = true;
                    stream_counters_->streams_opened.fetch_add(1, std::memory_order_relaxed);
                }
                stream_counters_->half_close_probes.fetch_add(1, std::memory_order_relaxed);
                const bool probed = co_await write_chunk(stream, ": probe\n\n");
                if (!probed) {
                    client_gone = true;
                    break;
                }
            } else if (probe_socket) {
                if (peer_reset(stream)) {
                    client_gone = true;
                    break;
                }
            } else if (stream_open) {
                const bool kept_alive = co_await write_chunk(stream, ": keep-alive\n\n");
                if (!kept_alive) {
                    client_gone = true;
                    break;
                }
            }
        }

        if (client_gone) {
            exchange->cancel();
            stream_counters_->disconnect_cancels.fetch_add(1, std::memory_order_relaxed);
        }
        if (watch->armed) {
            boost::system::error_code ignored;
            (void)stream.socket().cancel(ignored);
            while (watch->armed) {
                co_await wait_for_signal(*exchange, std::nullopt);
            }
        }
        co_return false;
    }

    /// Leaves the stream of @p legacy without a connection, unless a resume took it over: its
    /// events then wait in the event store.
    static void detach_stream(const detail_session_mgr::LegacyStream& legacy) {
        auto& session = *legacy.session;
        std::lock_guard lock(session.response_mutex);
        if (legacy.request_key.empty()) {
            if (session.get_generation == legacy.generation) {
                session.get_timer.reset();
                session.get_events.clear();
            }
            return;
        }
        const auto pending = session.pending_responses.find(legacy.request_key);
        if (pending != session.pending_responses.end() &&
            pending->second.generation == legacy.generation) {
            pending->second.detached = true;
            pending->second.stream_events.clear();
        }
    }

    /// Writes a stream of a session of 2025-11-25 or earlier as its events come: the events of a
    /// POST and finally its response, or the GET stream until the session ends. A POST answered
    /// before anything else was sent gets one complete response, as when nothing streams. A
    /// client that goes away cancels nothing, since the specification does not make a disconnect
    /// a cancellation: the stream's events wait in the event store, where a GET with
    /// Last-Event-ID resumes it.
    ///
    /// @return Whether the connection may carry another request.
    Task<bool> serve_legacy_stream(Connection& stream, const StringRequest& request,
                                   std::shared_ptr<detail_session_mgr::LegacyStream> legacy) {
        auto& session = *legacy->session;
        const bool get_stream = legacy->request_key.empty();
        SseEventList events = std::move(legacy->replay);
        bool stream_open = false;
        for (;;) {
            bool finished = false;
            bool replaced = false;
            std::optional<std::string> response_body;
            {
                std::lock_guard lock(session.response_mutex);
                SseEventList* waiting = nullptr;
                if (get_stream) {
                    replaced = session.get_generation != legacy->generation;
                    if (!replaced) {
                        waiting = &session.get_events;
                        finished = session.ended;
                        if (finished) {
                            session.get_timer.reset();
                        }
                    }
                } else {
                    const auto pending = session.pending_responses.find(legacy->request_key);
                    if (pending == session.pending_responses.end()) {
                        finished = true;
                    } else if (pending->second.generation != legacy->generation) {
                        replaced = true;
                    } else {
                        waiting = &pending->second.stream_events;
                        if (pending->second.response_ready) {
                            finished = true;
                            response_body = std::move(pending->second.response_body);
                        }
                    }
                }
                if (waiting != nullptr) {
                    for (auto& event : *waiting) {
                        events.push_back(std::move(event));
                    }
                    waiting->clear();
                }
                if (finished && !get_stream && !replaced) {
                    session.pending_responses.erase(legacy->request_key);
                }
            }
            if (replaced || closed.load(std::memory_order_acquire)) {
                // A resume serves the stream now, or the transport is closing.
                co_return false;
            }

            if (finished && !stream_open && !get_stream) {
                StringResponse response;
                if (!response_body.has_value()) {
                    response = detail_session_mgr::make_error_response(
                        request, http::status::internal_server_error,
                        "Missing response body for request");
                } else if (!events.empty()) {
                    response = detail_session_mgr::make_sse_response(request, events);
                } else {
                    response = detail_session_mgr::make_json_response(request, http::status::ok,
                                                                      std::move(*response_body));
                }
                response.set("Mcp-Session-Id", session.session_id);
                boost::system::error_code error;
                co_await http::async_write(
                    stream, response, boost::asio::redirect_error(boost::asio::use_awaitable, error));
                co_return !error && response.keep_alive();
            }

            if (!stream_open && (legacy->open_at_once || !events.empty())) {
                http::response<http::empty_body> header{http::status::ok, request.version()};
                header.set(http::field::server, "mcp-cpp-sdk");
                header.set(http::field::content_type, "text/event-stream");
                header.set(http::field::cache_control, "no-cache");
                header.set("X-Accel-Buffering", "no");
                header.set("Mcp-Session-Id", session.session_id);
                header.keep_alive(request.keep_alive());
                header.chunked(true);
                http::response_serializer<http::empty_body> serializer{header};
                boost::system::error_code error;
                co_await http::async_write_header(
                    stream, serializer, boost::asio::redirect_error(boost::asio::use_awaitable, error));
                if (error) {
                    detach_stream(*legacy);
                    co_return false;
                }
                stream_open = true;
            }

            if (!events.empty()) {
                std::string text;
                for (const auto& [id, data] : events) {
                    detail_session_mgr::append_sse_event(text, id, data);
                }
                events.clear();
                if (!co_await write_chunk(stream, text)) {
                    detach_stream(*legacy);
                    co_return false;
                }
            }

            if (finished) {
                boost::system::error_code error;
                co_await boost::asio::async_write(
                    stream, http::make_chunk_last(),
                    boost::asio::redirect_error(boost::asio::use_awaitable, error));
                co_return !error && request.keep_alive();
            }

            // An open stream carries a comment now and then, which keeps intermediaries from
            // closing it and finds a client that left.
            if (stream_open) {
                legacy->signal->expires_after(stream_options_.keepalive_interval);
            } else {
                legacy->signal->expires_at(std::chrono::steady_clock::time_point::max());
            }
            boost::system::error_code wait_error;
            co_await legacy->signal->async_wait(
                boost::asio::redirect_error(boost::asio::use_awaitable, wait_error));
            if (!wait_error && stream_open && !co_await write_chunk(stream, ": keep-alive\n\n")) {
                detach_stream(*legacy);
                co_return false;
            }
        }
    }

    std::variant<std::shared_ptr<detail_session_mgr::SessionRuntime>, StringResponse>
    resolve_session_for_post(const StringRequest& request, const nlohmann::json& request_json,
                             boost::asio::any_io_executor conn_strand, std::string principal) {
        if (closed.load(std::memory_order_acquire)) {
            return detail_session_mgr::make_error_response(request, http::status::service_unavailable,
                                                           "Transport closed");
        }

        const auto session_header_it = request.find("Mcp-Session-Id");
        if (session_header_it == request.end()) {
            if (!detail_session_mgr::is_initialize_request(request_json)) {
                return detail_session_mgr::make_jsonrpc_error_response(
                    request, http::status::bad_request, g_INVALID_REQUEST,
                    "Missing Mcp-Session-Id header; only initialize is allowed without a session");
            }
            if (at_session_limit()) {
                return detail_session_mgr::make_error_response(
                    request, http::status::service_unavailable, "Too many sessions");
            }
            return create_session(conn_strand, std::move(principal));
        }

        std::string session_id_value(session_header_it->value());
        auto session = find_session(session_id_value);
        if (session == nullptr || !session_belongs_to(*session, principal)) {
            return detail_session_mgr::make_jsonrpc_error_response(
                request, http::status::not_found, g_INVALID_REQUEST, "Session not found");
        }
        return session;
    }

    Task<StringResponse> wait_for_response(
        detail_session_mgr::SessionRuntime* session, const StringRequest& request,
        const std::string& request_id_key,
        std::shared_ptr<boost::asio::steady_timer> timer_signal) const {
        for (;;) {
            if (closed.load(std::memory_order_acquire)) {
                consume_pending_response(session, request_id_key);
                co_return detail_session_mgr::make_error_response(
                    request, http::status::internal_server_error,
                    "Transport closed while waiting response");
            }

            bool response_ready = false;
            {
                std::lock_guard lock(session->response_mutex);
                auto pending_it = session->pending_responses.find(request_id_key);
                if (pending_it == session->pending_responses.end()) {
                    co_return detail_session_mgr::make_error_response(
                        request, http::status::internal_server_error, "Response lost");
                }
                response_ready = pending_it->second.response_ready;
            }
            if (response_ready) {
                break;
            }

            // Cancelling the timer is how a response is signalled, so cancellation is the expected
            // end of every wait. It is taken as an error code rather than thrown and caught on every
            // request.
            boost::system::error_code wait_error;
            co_await timer_signal->async_wait(
                boost::asio::redirect_error(boost::asio::use_awaitable, wait_error));
            if (wait_error && wait_error != boost::asio::error::operation_aborted) {
                throw boost::system::system_error(wait_error);
            }
        }

        auto result = consume_pending_response(session, request_id_key);
        if (!result.response_body.has_value()) {
            co_return detail_session_mgr::make_error_response(
                request, http::status::internal_server_error, "Missing response body for request");
        }

        const auto accept_it = request.find(http::field::accept);
        const bool client_accepts_sse =
            accept_it != request.end() &&
            detail::to_std_string_view(accept_it->value()).find("text/event-stream") !=
                std::string_view::npos;

        if (!json_only_mode_->load(std::memory_order_acquire) && client_accepts_sse &&
            !result.stream_events.empty()) {
            auto response = detail_session_mgr::make_sse_response(request, result.stream_events);
            response.set("Mcp-Session-Id", session->session_id);
            co_return response;
        }

        auto response = detail_session_mgr::make_json_response(request, http::status::ok,
                                                               std::move(*result.response_body));
        response.set("Mcp-Session-Id", session->session_id);
        co_return response;
    }

    /// @p principal_id is the principal the bearer token validator named, if it named one. Requests
    /// are attributed to it, or else to a digest of their Authorization header. A request served
    /// without a session carries that principal; a session belongs to the principal that created it.
    Task<Reply> handle_post(const StringRequest& request, boost::asio::any_io_executor conn_strand,
                            std::unique_ptr<Server>& stateless_server, detail::BudgetLease& bytes,
                            std::optional<std::string> principal_id) {
        if (closed.load(std::memory_order_acquire)) {
            co_return detail_session_mgr::make_error_response(
                request, http::status::service_unavailable, "Transport closed");
        }

        auto request_json = detail::parse_peer_json(request.body());
        if (request_json.is_discarded() || !request_json.is_object()) {
            co_return detail_session_mgr::make_error_response(request, http::status::bad_request,
                                                              "Invalid JSON-RPC payload");
        }
        // Held until the answer is written: the parsed message can be far larger than its body.
        if (!bytes.grow_to(request.body().size() + detail::json_memory_cost(request_json))) {
            co_return detail_session_mgr::make_error_response(
                request, http::status::service_unavailable, "Server busy");
        }

        const auto protocol_header_it = request.find("MCP-Protocol-Version");
        if (detail::is_modern_http_request(request_json,
                                           detail::http_header(request, "MCP-Protocol-Version"))) {
            co_return co_await handle_modern_post(
                request, std::move(request_json), true, conn_strand,
                detail::request_principal(principal_id, request_authorization(request)));
        }
        if (protocol_policy_ && !protocol_policy_->serve_legacy) {
            co_return co_await handle_modern_post(
                request, std::move(request_json), false, conn_strand,
                detail::request_principal(principal_id, request_authorization(request)));
        }

        const bool is_initialize = detail_session_mgr::is_initialize_request(request_json);
        if (is_initialize && protocol_header_it != request.end()) {
            const auto version = detail_session_mgr::protocol_header_value(protocol_header_it);
            if (is_modern_protocol_version(version)) {
                // initialize does not exist in 2026-07-28; the error names the versions served.
                co_return initialize_not_served_response(request, request_json);
            }
            if (!is_legacy_protocol_version(version)) {
                co_return detail_session_mgr::make_error_response(
                    request, http::status::bad_request, "Invalid MCP-Protocol-Version header");
            }
        }

        if (stateless_json_mode_) {
            co_return co_await handle_stateless_post(
                request, std::move(request_json), stateless_server,
                detail::request_principal(principal_id, request_authorization(request)));
        }

        // The Server answers a request whose id is neither a string nor an integer within int64_t
        // without an id, since it has none it may echo, so that answer could never be matched to
        // this request.
        if (request_json.contains("id") && request_json.contains("method") &&
            !detail::is_request_id(request_json.at("id"))) {
            co_return detail_session_mgr::make_jsonrpc_error_response(
                request, http::status::bad_request, g_INVALID_REQUEST,
                "JSON-RPC request id must be a string or an integer within int64_t");
        }

        auto session_var = resolve_session_for_post(
            request, request_json, conn_strand,
            detail::request_principal(principal_id, request_authorization(request)));
        if (std::holds_alternative<StringResponse>(session_var)) {
            co_return std::get<StringResponse>(session_var);
        }
        auto session = std::get<std::shared_ptr<detail_session_mgr::SessionRuntime>>(session_var);
        if (session == nullptr || session->client_transport == nullptr) {
            co_return detail_session_mgr::make_error_response(
                request, http::status::service_unavailable, "Transport closed");
        }

        if (!is_initialize) {
            if (protocol_header_it != request.end() &&
                !detail_session_mgr::session_accepts_version(
                    detail_session_mgr::protocol_header_value(protocol_header_it),
                    session->protocol_version())) {
                co_return detail_session_mgr::make_error_response(
                    request, http::status::bad_request, "Invalid MCP-Protocol-Version header");
            }
        } else if (request.find("Mcp-Session-Id") == request.end()) {
            // Only the initialize that creates a session negotiates its version. The Server refuses
            // an initialize on a session that already has one, and that refusal must not change
            // the version the client and the session agreed on.
            const auto requested_protocol_version =
                detail_session_mgr::initialize_protocol_version(request_json)
                    .value_or(std::string(g_LATEST_LEGACY_PROTOCOL_VERSION));
            session->set_protocol_version(
                protocol_policy_
                    ? negotiate_protocol_version(requested_protocol_version, *protocol_policy_)
                    : std::string(negotiate_protocol_version(requested_protocol_version)));
        }

        // A session exists to serve the client that initialized it, so one whose initialize did
        // not succeed is not kept: nobody will come back for it.
        const bool created_here = is_initialize && request.find("Mcp-Session-Id") == request.end();
        if (!request_json.contains("id") || !request_json.contains("method")) {
            if (created_here) {
                destroy_session(session->session_id);
                co_return detail_session_mgr::make_jsonrpc_error_response(
                    request, http::status::bad_request, g_INVALID_REQUEST,
                    "initialize must be a request");
            }
            co_await session->client_transport->write_json(std::move(request_json));
            auto response =
                detail_session_mgr::make_empty_json_response(request, http::status::accepted);
            response.set("Mcp-Session-Id", session->session_id);
            co_return response;
        }

        const auto request_id_key = request_json.at("id").dump();

        auto timer_signal = std::make_shared<boost::asio::steady_timer>(conn_strand);
        timer_signal->expires_at(std::chrono::steady_clock::time_point::max());

        {
            std::lock_guard lock(session->response_mutex);
            if (session->pending_responses.contains(request_id_key)) {
                co_return detail_session_mgr::make_error_response(request, http::status::bad_request,
                                                                  "Request id already pending");
            }
            session->pending_responses.emplace(
                request_id_key,
                detail_session_mgr::PendingResponse{
                    timer_signal, std::nullopt, {}, false, std::to_string(++session->streams)});
        }

        co_await session->client_transport->write_json(std::move(request_json));

        // A client that takes SSE over HTTP/1.1 gets the request's events as they come, so a
        // request of the server's own reaches it while the handler waits for the answer.
        const auto accept_it = request.find(http::field::accept);
        if (!is_initialize && request.version() >= 11 &&
            !json_only_mode_->load(std::memory_order_acquire) && accept_it != request.end() &&
            detail::to_std_string_view(accept_it->value()).find("text/event-stream") !=
                std::string_view::npos) {
            auto legacy = std::make_shared<detail_session_mgr::LegacyStream>();
            legacy->session = std::move(session);
            legacy->request_key = request_id_key;
            legacy->signal = std::move(timer_signal);
            co_return legacy;
        }

        auto response =
            co_await wait_for_response(session.get(), request, request_id_key, timer_signal);
        if (created_here && !session->established.load(std::memory_order_acquire)) {
            destroy_session(session->session_id);
        }
        co_return response;
    }

    Task<StringResponse> handle_delete(const StringRequest& request, std::string principal) {
        const auto session_header_it = request.find("Mcp-Session-Id");
        if (session_header_it == request.end()) {
            co_return detail_session_mgr::make_error_response(request, http::status::bad_request,
                                                              "Missing Mcp-Session-Id header");
        }

        std::string session_id_value(session_header_it->value());
        auto session = find_session(session_id_value);
        if (session == nullptr || !session_belongs_to(*session, principal)) {
            co_return detail_session_mgr::make_jsonrpc_error_response(
                request, http::status::not_found, g_INVALID_REQUEST, "Session not found");
        }

        const auto protocol_header_it = request.find("MCP-Protocol-Version");
        if (protocol_header_it != request.end() &&
            !detail_session_mgr::session_accepts_version(
                detail_session_mgr::protocol_header_value(protocol_header_it),
                session->protocol_version())) {
            co_return detail_session_mgr::make_error_response(request, http::status::bad_request,
                                                              "Invalid MCP-Protocol-Version header");
        }

        destroy_session(session_id_value);
        co_return detail_session_mgr::make_json_response(request, http::status::ok, "{}");
    }

    /// A GET opens the session's stream for messages that belong to no request, or with
    /// Last-Event-ID resumes a stream: one still going on is served live from there, and one that
    /// ended is replayed.
    Task<Reply> handle_get(const StringRequest& request, std::string principal,
                           detail::BudgetLease& bytes, boost::asio::any_io_executor conn_strand) {
        const auto session_header_it = request.find("Mcp-Session-Id");
        if (session_header_it == request.end()) {
            co_return detail_session_mgr::make_error_response(request, http::status::bad_request,
                                                              "Missing Mcp-Session-Id header");
        }

        std::string session_id_value(session_header_it->value());
        auto session = find_session(session_id_value);
        if (session == nullptr || !session_belongs_to(*session, principal)) {
            co_return detail_session_mgr::make_jsonrpc_error_response(
                request, http::status::not_found, g_INVALID_REQUEST, "Session not found");
        }

        const auto protocol_header_it = request.find("MCP-Protocol-Version");
        if (protocol_header_it != request.end() &&
            !detail_session_mgr::session_accepts_version(
                detail_session_mgr::protocol_header_value(protocol_header_it),
                session->protocol_version())) {
            co_return detail_session_mgr::make_error_response(request, http::status::bad_request,
                                                              "Invalid MCP-Protocol-Version header");
        }
        if (!detail::get_accepts_event_stream(request)) {
            co_return detail_session_mgr::make_error_response(
                request, http::status::not_acceptable,
                "Not Acceptable: the client must accept text/event-stream");
        }

        auto signal = std::make_shared<boost::asio::steady_timer>(conn_strand);
        signal->expires_at(std::chrono::steady_clock::time_point::max());
        auto legacy = std::make_shared<detail_session_mgr::LegacyStream>();
        legacy->signal = signal;
        legacy->open_at_once = true;

        const auto last_event_id_it = request.find("Last-Event-ID");
        if (last_event_id_it != request.end()) {
            auto last_id = std::string(last_event_id_it->value());
            std::optional<SseEventList> missed_events;
            bool live = false;
            // The connection that served the stream until now, woken to see it was replaced.
            std::shared_ptr<boost::asio::steady_timer> replaced;
            {
                std::lock_guard lock(session->response_mutex);
                missed_events = session->event_store.events_after(last_id);
                const auto stream_name = session->event_store.stream_of(last_id);
                if (missed_events.has_value() && stream_name.has_value()) {
                    // A stream still going on is taken over from the connection that served it,
                    // which may not have noticed yet that its client left.
                    if (*stream_name == session->get_stream && !session->ended) {
                        live = true;
                        legacy->generation = ++session->get_generation;
                        replaced = std::move(session->get_timer);
                        session->get_timer = signal;
                        session->get_events.clear();
                    } else {
                        for (auto& [key, pending] : session->pending_responses) {
                            if (pending.stream != *stream_name || pending.response_ready) {
                                continue;
                            }
                            live = true;
                            legacy->request_key = key;
                            legacy->generation = ++pending.generation;
                            pending.detached = false;
                            replaced = std::move(pending.ready_timer);
                            pending.ready_timer = signal;
                            pending.stream_events.clear();
                            break;
                        }
                    }
                }
            }
            if (replaced) {
                signal_timer(replaced);
            }
            if (!missed_events.has_value()) {
                co_return detail_session_mgr::make_error_response(
                    request, http::status::gone, "Event ID has been evicted from store");
            }
            // Each replay holds its own copy of the events until the client has read it, so the
            // copies count against what requests may hold.
            std::size_t replay_bytes = 0;
            for (const auto& [id, data] : *missed_events) {
                replay_bytes += 2 * (id.size() + data.size());
            }
            if (!bytes.grow_to(replay_bytes)) {
                if (live) {
                    detach_stream(*legacy);
                }
                co_return detail_session_mgr::make_error_response(
                    request, http::status::service_unavailable, "Server busy");
            }
            if (!live) {
                // An empty stream is an answer here too: the client asked to resume one.
                co_return detail_session_mgr::make_sse_response(request, *missed_events);
            }
            legacy->session = std::move(session);
            legacy->replay = std::move(*missed_events);
            co_return legacy;
        }

        const bool primes = session->protocol_version() >= "2025-11-25";
        {
            std::lock_guard lock(session->response_mutex);
            if (session->get_timer != nullptr) {
                // Each message goes on one stream, so a session has one GET stream at a time.
                co_return detail_session_mgr::make_error_response(
                    request, http::status::conflict, "This session already has a GET stream");
            }
            if (session->get_stream.empty()) {
                session->get_stream = std::to_string(++session->streams);
            }
            legacy->generation = ++session->get_generation;
            session->get_timer = signal;
            session->get_events.clear();
            // An event with an id and no data lets a 2025-11-25 client resume a stream that has
            // carried nothing yet.
            if (primes) {
                auto id = session->event_store.append(std::string(), session->get_stream);
                legacy->replay.emplace_back(std::move(id), std::string());
            }
        }
        legacy->session = std::move(session);
        co_return legacy;
    }

    static void signal_timer(const std::shared_ptr<boost::asio::steady_timer>& timer) {
        boost::asio::dispatch(timer->get_executor(), [timer]() { (void)timer->cancel(); });
    }

    /// Routes a message the session's server wrote: a response to the stream of its POST, which
    /// it ends; a message written as related to a request still waiting, to that request's stream;
    /// and any other message to the GET stream, or, when no GET opened one, to the stream of the
    /// only POST waiting. With several waiting it travels on the stream of the next response.
    /// Every event is stored in the event store, so a resume finds it; a connection serving the
    /// stream also gets it, up to @p max_queued not yet written.
    static void dispatch_response_to_pending(detail_session_mgr::SessionRuntime* session,
                                             std::string message, bool json_only,
                                             const std::string* related_request, std::size_t max_queued,
                                             StreamCounters& counters) {
        // Routing needs only the id and whether a result or error is present, so the message is
        // scanned rather than parsed; the id alone is parsed, to key it as its request was keyed.
        bool is_response = false;
        std::string request_id_key;
        if (const auto envelope = detail::scan_jsonrpc_envelope(message)) {
            is_response = !envelope->id.empty() && envelope->has_result_or_error;
            if (is_response) {
                const auto id = nlohmann::json::parse(envelope->id, nullptr, false);
                if (id.is_discarded()) {
                    return;
                }
                request_id_key = id.dump();
            }
        } else {
            const auto message_json = nlohmann::json::parse(message, nullptr, false);
            if (message_json.is_discarded() || !message_json.is_object()) {
                return;
            }
            is_response = message_json.contains("id") &&
                          (message_json.contains("result") || message_json.contains("error"));
            if (is_response) {
                request_id_key = message_json.at("id").dump();
            }
        }
        std::shared_ptr<boost::asio::steady_timer> timer_to_signal;

        {
            std::lock_guard lock(session->response_mutex);

            // An event is stored under the stream it is sent on, so a resume of one stream replays
            // nothing that another stream carried.
            const auto send_on = [session, max_queued, &counters, &timer_to_signal](
                                     detail_session_mgr::PendingResponse& pending, std::string event) {
                auto event_id = session->event_store.append(event, pending.stream);
                if (pending.detached) {
                    return;
                }
                if (pending.stream_events.size() >= max_queued) {
                    counters.dropped_notifications.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
                pending.stream_events.emplace_back(std::move(event_id), std::move(event));
                timer_to_signal = pending.ready_timer;
            };

            if (!is_response) {
                if (json_only) {
                    return;
                }
                const auto related = related_request != nullptr
                                         ? session->pending_responses.find(*related_request)
                                         : session->pending_responses.end();
                if (related != session->pending_responses.end()) {
                    send_on(related->second, std::move(message));
                } else if (!session->get_stream.empty()) {
                    auto event_id = session->event_store.append(message, session->get_stream);
                    if (session->get_timer) {
                        if (session->get_events.size() >= max_queued) {
                            counters.dropped_notifications.fetch_add(1, std::memory_order_relaxed);
                        } else {
                            session->get_events.emplace_back(std::move(event_id), std::move(message));
                            timer_to_signal = session->get_timer;
                        }
                    }
                } else if (session->pending_responses.size() == 1) {
                    send_on(session->pending_responses.begin()->second, std::move(message));
                } else if (!session->pending_responses.empty()) {
                    session->unassigned_stream_events.push_back(std::move(message));
                }
            } else {
                auto pending_it = session->pending_responses.find(request_id_key);
                if (pending_it == session->pending_responses.end()) {
                    session->unassigned_stream_events.clear();
                    return;
                }

                if (!json_only) {
                    for (auto& event : session->unassigned_stream_events) {
                        send_on(pending_it->second, std::move(event));
                    }
                    session->unassigned_stream_events.clear();
                    // The response always reaches the connection, which needs it to end the stream.
                    auto event_id = session->event_store.append(message, pending_it->second.stream);
                    if (!pending_it->second.detached) {
                        pending_it->second.stream_events.emplace_back(std::move(event_id), message);
                    }
                }

                if (pending_it->second.detached) {
                    // Nobody waits for it: a resume finds the response in the event store.
                    session->pending_responses.erase(pending_it);
                    return;
                }
                pending_it->second.response_body = std::move(message);
                pending_it->second.response_ready = true;
                timer_to_signal = pending_it->second.ready_timer;
            }
        }

        if (timer_to_signal) {
            signal_timer(timer_to_signal);
        }
    }

    static Task<void> read_outbound_messages(
        std::shared_ptr<detail_session_mgr::SessionRuntime> session,
        std::shared_ptr<std::atomic<bool>> json_only_mode, std::size_t max_queued,
        std::shared_ptr<StreamCounters> counters) {
        try {
            for (;;) {
                auto message = co_await detail::MemoryTransportAccess::read_related_until_closed(
                    *session->client_transport);
                if (!message) {
                    break;
                }
                if (!session->established.load(std::memory_order_acquire)) {
                    // The server's own message, read without the depth limit on peer input: an
                    // initialize result nested deeper than a peer may send still establishes the
                    // session its client was given.
                    bool answered = false;
                    if (const auto envelope = detail::scan_jsonrpc_envelope(message->message)) {
                        answered = envelope->has_result;
                    } else {
                        const auto parsed = detail::parse_peer_json(message->message);
                        answered = parsed.is_object() && parsed.contains("result");
                    }
                    session->established.store(answered, std::memory_order_release);
                }
                dispatch_response_to_pending(session.get(), std::move(message->message),
                                             json_only_mode->load(std::memory_order_acquire),
                                             message->related_request.get(), max_queued, *counters);
            }
        } catch (...) {
            // Any other failure ends the loop too, and releases the waiters below.
            (void)0;
        }
        end_streams(*session);
    }

    static detail_session_mgr::PendingResult consume_pending_response(
        detail_session_mgr::SessionRuntime* session, const std::string& request_id_key) {
        std::lock_guard lock(session->response_mutex);
        auto pending_it = session->pending_responses.find(request_id_key);
        if (pending_it == session->pending_responses.end()) {
            return {};
        }
        detail_session_mgr::PendingResult result{std::move(pending_it->second.response_body),
                                                 std::move(pending_it->second.stream_events)};
        session->pending_responses.erase(pending_it);
        return result;
    }
};  // namespace mcp

// ============================================================================
// StreamableHttpSessionManager public methods
// ============================================================================

StreamableHttpSessionManager::StreamableHttpSessionManager(const boost::asio::any_io_executor& executor,
                                                           std::string host, unsigned short port,
                                                           ServerFactory factory,
                                                           std::size_t event_store_capacity)
    : impl_(std::make_shared<Impl>(executor, std::move(host), port, std::move(factory),
                                   event_store_capacity)) {}

StreamableHttpSessionManager::~StreamableHttpSessionManager() {
    try {
        close();
    } catch (...) {
        // Ignore exceptions in destructor
        (void)0;
    }
}

void StreamableHttpSessionManager::set_custom_request_handler(CustomRequestHandler handler) {
    std::lock_guard lock(impl_->configuration_mutex_);
    impl_->ensure_configurable();
    impl_->custom_handler = std::move(handler);
}

void StreamableHttpSessionManager::set_allowed_origins(std::vector<std::string> origins) {
    std::lock_guard lock(impl_->configuration_mutex_);
    impl_->ensure_configurable();
    impl_->allowed_origins_.clear();
    impl_->allowed_origins_.reserve(origins.size());
    for (auto& origin : origins) {
        impl_->allowed_origins_.insert(std::move(origin));
    }
    impl_->allow_all_origins_ = false;
}

void StreamableHttpSessionManager::set_allow_all_origins(bool allow_all) {
    std::lock_guard lock(impl_->configuration_mutex_);
    impl_->ensure_configurable();
    impl_->allow_all_origins_ = allow_all;
}

void StreamableHttpSessionManager::set_connection_limits(const HttpConnectionLimits& limits) {
    if (limits.max_requests == 0 || limits.max_request_bytes == 0) {
        throw std::invalid_argument("HTTP connection limits must allow a request");
    }
    std::lock_guard lock(impl_->configuration_mutex_);
    impl_->connection_limits_ = limits;
}

void StreamableHttpSessionManager::set_bearer_token_validator(BearerTokenValidator validator) {
    std::lock_guard lock(impl_->configuration_mutex_);
    impl_->ensure_configurable();
    if (validator && impl_->async_bearer_token_validator_) {
        throw std::logic_error("StreamableHttpSessionManager accepts one bearer token validator");
    }
    impl_->bearer_token_validator_ = std::move(validator);
}

void StreamableHttpSessionManager::set_async_bearer_token_validator(
    AsyncBearerTokenValidator validator) {
    std::lock_guard lock(impl_->configuration_mutex_);
    impl_->ensure_configurable();
    if (validator && impl_->bearer_token_validator_) {
        throw std::logic_error("StreamableHttpSessionManager accepts one bearer token validator");
    }
    impl_->async_bearer_token_validator_ = std::move(validator);
}

void StreamableHttpSessionManager::set_max_request_body_bytes(std::size_t max_bytes) {
    std::lock_guard lock(impl_->configuration_mutex_);
    impl_->ensure_configurable();
    if (max_bytes == 0) {
        throw std::invalid_argument("Maximum request body size must be greater than zero");
    }
    impl_->max_request_body_bytes_ = max_bytes;
}

void StreamableHttpSessionManager::set_bearer_challenge(BearerChallengeConfig challenge) {
    std::lock_guard lock(impl_->configuration_mutex_);
    impl_->ensure_configurable();
    auto rendered = impl_->render_challenge(challenge);
    impl_->bearer_challenge_ = std::move(challenge);
    impl_->www_authenticate_value_ = std::move(rendered);
}

void StreamableHttpSessionManager::set_protected_resource_metadata(
    ProtectedResourceMetadataConfig metadata) {
    std::lock_guard lock(impl_->configuration_mutex_);
    impl_->ensure_configurable();
    if (metadata.resource.empty()) {
        throw std::invalid_argument("Protected-resource metadata requires a resource URL");
    }

    auto metadata_url = protected_resource_metadata_url(metadata);
    auto document = format_protected_resource_metadata(metadata);
    auto challenge = impl_->bearer_challenge_;
    if (challenge.resource_metadata.empty()) {
        challenge.resource_metadata = std::move(metadata_url);
    }
    auto rendered = format_www_authenticate(challenge);

    auto document_path = protected_resource_metadata_path(metadata);

    impl_->protected_resource_metadata_ = std::move(metadata);
    impl_->protected_resource_metadata_path_ = std::move(document_path);
    impl_->protected_resource_metadata_body_ = std::move(document);
    impl_->www_authenticate_value_ = std::move(rendered);
}

void StreamableHttpSessionManager::set_unauthenticated_paths(std::vector<std::string> paths) {
    std::lock_guard lock(impl_->configuration_mutex_);
    impl_->ensure_configurable();
    impl_->unauthenticated_paths_.clear();
    impl_->unauthenticated_paths_.reserve(paths.size());
    for (auto& path : paths) {
        impl_->unauthenticated_paths_.insert(std::move(path));
    }
}

void StreamableHttpSessionManager::set_session_limits(const SessionLimits& limits) {
    if (limits.max_sessions == 0) {
        throw std::invalid_argument("The session limit must be at least 1");
    }
    std::lock_guard lock(impl_->configuration_mutex_);
    impl_->session_limits = limits;
}

std::size_t StreamableHttpSessionManager::session_count() const {
    std::shared_lock lock(impl_->sessions_mutex_);
    return impl_->sessions.size();
}

void StreamableHttpSessionManager::set_json_only(bool json_only) {
    impl_->json_only_mode_->store(json_only, std::memory_order_release);
}

void StreamableHttpSessionManager::set_stateless_json_mode(bool enabled) {
    std::lock_guard lock(impl_->configuration_mutex_);
    impl_->ensure_configurable();
    impl_->stateless_json_mode_ = enabled;
    if (enabled) {
        impl_->json_only_mode_->store(true, std::memory_order_release);
    }
}

void StreamableHttpSessionManager::set_tool_executor(const boost::asio::any_io_executor& exec) {
    std::lock_guard lock(impl_->configuration_mutex_);
    impl_->ensure_configurable();
    impl_->tool_executor_ = exec;
}

void StreamableHttpSessionManager::set_protocol_policy(ServerProtocolPolicy policy) {
    policy.validate();
    std::lock_guard lock(impl_->configuration_mutex_);
    impl_->ensure_configurable();
    impl_->protocol_policy_ = std::move(policy);
}

void StreamableHttpSessionManager::set_modern_server(std::shared_ptr<Server> server) {
    if (!server) {
        throw std::invalid_argument("The modern server must not be null");
    }
    std::lock_guard lock(impl_->configuration_mutex_);
    impl_->ensure_configurable();
    std::lock_guard server_lock(impl_->modern_server_mutex_);
    impl_->modern_server_ = std::move(server);
}

std::shared_ptr<Server> StreamableHttpSessionManager::modern_server() { return impl_->modern_server(); }

std::uint64_t StreamableHttpSessionManager::header_mismatch_rejections() const {
    return impl_->header_mismatch_rejections_.load(std::memory_order_relaxed);
}

void StreamableHttpSessionManager::set_stream_options(StreamOptions options) {
    if (options.keepalive_interval.count() <= 0 || options.disconnect_probe_interval.count() <= 0) {
        throw std::invalid_argument("Stream intervals must be positive");
    }
    if (options.max_queued_notifications == 0) {
        throw std::invalid_argument("Stream notification queue limit must be at least 1");
    }
    std::lock_guard lock(impl_->configuration_mutex_);
    impl_->ensure_configurable();
    impl_->stream_options_ = options;
}

StreamStats StreamableHttpSessionManager::stream_stats() const {
    const auto& counters = *impl_->stream_counters_;
    return {counters.streams_opened.load(std::memory_order_relaxed),
            counters.disconnect_cancels.load(std::memory_order_relaxed),
            counters.half_close_probes.load(std::memory_order_relaxed),
            counters.dropped_notifications.load(std::memory_order_relaxed)};
}

void StreamableHttpSessionManager::close() {
    auto impl = impl_;
    {
        std::lock_guard lock(impl->configuration_mutex_);
        if (impl->closed.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
    }

    boost::asio::post(impl->listener_strand, [impl]() {
        boost::system::error_code ec;
        (void)impl->acceptor.cancel(ec);
        (void)impl->acceptor.close(ec);
        impl->expiry_timer.cancel();
    });
    impl->close_active_connections();

    std::vector<std::shared_ptr<detail_session_mgr::SessionRuntime>> active_sessions;
    {
        std::unique_lock lock(impl->sessions_mutex_);
        active_sessions.reserve(impl->sessions.size());
        for (auto& [id, session] : impl->sessions) {
            (void)id;
            active_sessions.push_back(std::move(session));
        }
        impl->sessions.clear();
    }

    for (const auto& session : active_sessions) {
        std::vector<std::shared_ptr<boost::asio::steady_timer>> timers;
        {
            std::lock_guard lock(session->response_mutex);
            timers.reserve(session->pending_responses.size());
            for (auto& [key, pending] : session->pending_responses) {
                (void)key;
                pending.response_ready = true;
                timers.push_back(pending.ready_timer);
            }
        }
        for (const auto& timer : timers) {
            Impl::signal_timer(timer);
        }
        if (session->client_transport) {
            session->client_transport->close();
        }
    }

    // No retry can reach the modern server through this endpoint any more.
    std::shared_ptr<Server> modern_server;
    {
        std::lock_guard lock(impl->modern_server_mutex_);
        modern_server = impl->modern_server_;
    }
    if (modern_server) {
        modern_server->abandon_waiting_handlers();
    }
}

Task<void> StreamableHttpSessionManager::listen() {
    auto impl = impl_;
    (void)impl->begin_listening();
    boost::asio::co_spawn(impl->listener_strand, Impl::expire_idle_sessions(impl),
                          boost::asio::detached);
    return boost::asio::co_spawn(impl->listener_strand, listen_impl(impl), boost::asio::use_awaitable);
}

Task<void> StreamableHttpSessionManager::listen_impl(std::shared_ptr<Impl> impl) {
    for (;;) {
        if (impl->closed.load(std::memory_order_acquire)) {
            co_return;
        }

        boost::asio::ip::tcp::socket socket(impl->listener_strand);
        bool retry = false;
        try {
            socket = co_await impl->acceptor.async_accept(boost::asio::use_awaitable);
        } catch (const boost::system::system_error& err) {
            if (impl->closed.load(std::memory_order_acquire) ||
                err.code() == boost::asio::error::operation_aborted) {
                co_return;
            }
            if (!detail::accept_failure_can_pass(err.code())) {
                throw;
            }
            retry = true;
        }
        if (retry) {
            boost::asio::steady_timer pause(impl->listener_strand, detail::g_accept_retry_delay);
            boost::system::error_code ignored;
            co_await pause.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, ignored));
            continue;
        }

        const auto max_connections =
            detail::resolved_max_connections(impl->current_connection_limits().max_connections);
        if (impl->budget_.connections.fetch_add(1, std::memory_order_relaxed) >= max_connections) {
            // Closed at once, so the descriptor is free again for a connection that can be served.
            impl->budget_.connections.fetch_sub(1, std::memory_order_relaxed);
            boost::system::error_code ignored;
            (void)socket.close(ignored);
            continue;
        }
        auto conn_strand = detail::make_strand_executor(impl->executor);
        auto native_handle = socket.release();
        boost::asio::ip::tcp::socket conn_socket(conn_strand, boost::asio::ip::tcp::v4(),
                                                 native_handle);
        auto connection = std::make_shared<Impl::Connection>(std::move(conn_socket));
        if (!impl->register_connection(connection)) {
            impl->budget_.connections.fetch_sub(1, std::memory_order_relaxed);
            Impl::request_connection_close(connection);
            co_return;
        }
        boost::asio::co_spawn(
            conn_strand,
            [impl, connection, conn_strand]() mutable -> Task<void> {
                try {
                    co_await impl->handle_connection(connection, conn_strand);
                } catch (...) {
                    impl->unregister_connection(connection);
                    impl->budget_.connections.fetch_sub(1, std::memory_order_relaxed);
                    throw;
                }
                impl->unregister_connection(connection);
                impl->budget_.connections.fetch_sub(1, std::memory_order_relaxed);
            },
            [](const std::exception_ptr&) {
                // Connection errors (EOF, client disconnect) are normal;
                // handled per-connection, not propagated to the accept loop.
            });
    }
}

}  // namespace mcp

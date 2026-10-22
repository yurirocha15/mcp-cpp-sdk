#include "../detail/beast_string.hpp"
#include "../detail/http_request_metadata.hpp"
#include "../detail/jsonrpc_envelope.hpp"
#include "../detail/memory_transport_read.hpp"
#include "../detail/modern_methods.hpp"
#include "../detail/peer_json.hpp"

#include <mcp/core/constants.hpp>
#include <mcp/detail/secure_random.hpp>
#include <mcp/transport/http_session_manager.hpp>
#include <mcp/transport/memory.hpp>

#include <boost/asio/bind_executor.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
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
    SseEventList stream_events;
    bool response_ready{false};
};

struct PendingResult {
    std::optional<std::string> response_body;
    SseEventList stream_events;
};

/// The answer to a 2026-07-28 request while it is produced: the notifications its handler sends
/// and, once the server has answered, the response. Filled from the dispatch executor and read on
/// the connection's strand, which owns `signal`.
struct ModernExchange : std::enable_shared_from_this<ModernExchange> {
    ModernExchange(boost::asio::strand<boost::asio::any_io_executor> strand_arg,
                   std::size_t max_queued_arg, bool streams_arg)
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

    void finish(std::string response_wire) {
        {
            std::lock_guard lock(mutex);
            response = std::move(response_wire);
        }
        wake();
    }

    boost::asio::strand<boost::asio::any_io_executor> strand;
    boost::asio::steady_timer signal;
    bool woken{false};
    const std::size_t max_queued;
    /// Whether the answer may become an SSE stream: the client accepts one over HTTP/1.1.
    const bool streams;
    std::shared_ptr<std::atomic<bool>> cancelled = std::make_shared<std::atomic<bool>>(false);

    std::mutex mutex;
    std::deque<std::string> notifications;  // guarded by mutex
    std::optional<std::string> response;    // guarded by mutex
};

struct SessionRuntime {
    std::string session_id;

    std::shared_ptr<MemoryTransport> client_transport;

    mutable std::mutex protocol_mutex;
    std::string negotiated_protocol_version{std::string(g_LATEST_LEGACY_PROTOCOL_VERSION)};

    mutable std::mutex response_mutex;
    EventStore event_store;

    std::unordered_map<std::string, PendingResponse> pending_responses;
    SseEventList unassigned_stream_events;

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
    nlohmann::json error_body = {{"jsonrpc", "2.0"},
                                 {"error", {{"code", error_code}, {"message", error_message}}},
                                 {"id", nullptr}};
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
        sse_body += "id: ";
        sse_body += id;
        sse_body += "\ndata: ";
        sse_body += data;
        sse_body += "\n\n";
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

    /// A request handled by handle_request: a complete response, or the answer to a 2026-07-28
    /// request that serve_modern_exchange writes as it is produced.
    using Reply = std::variant<StringResponse, std::shared_ptr<detail_session_mgr::ModernExchange>>;

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
        return server;
    }

    std::shared_ptr<Server> modern_server() {
        std::lock_guard lock(modern_server_mutex_);
        if (!modern_server_) {
            modern_server_ = build_server(tool_executor_ ? tool_executor_ : executor);
        }
        return modern_server_;
    }

    std::shared_ptr<detail_session_mgr::SessionRuntime> create_session(
        boost::asio::strand<boost::asio::any_io_executor> conn_strand) {
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

        {
            std::unique_lock lock(sessions_mutex_);
            if (closed.load(std::memory_order_acquire)) {
                client_mem->close();
                return nullptr;
            }
            sessions.emplace(session_id, runtime);
        }

        std::shared_ptr<ITransport> server_transport = server_mem;
        auto server_exec = tool_executor_ ? tool_executor_ : executor;

        boost::asio::co_spawn(
            conn_strand,
            [srv = std::move(server), transport = std::move(server_transport),
             exec = server_exec]() mutable -> Task<void> {
                co_await srv->run(std::move(transport), exec);
            },
            boost::asio::detached);

        boost::asio::co_spawn(conn_strand, read_outbound_messages(runtime, json_only_mode_),
                              boost::asio::detached);
        return runtime;
    }

    std::shared_ptr<detail_session_mgr::SessionRuntime> find_session(const std::string& session_id) {
        std::shared_lock lock(sessions_mutex_);
        auto it = sessions.find(session_id);
        if (it == sessions.end()) {
            return nullptr;
        }
        return it->second;
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
            signal_timer(timer);
        }
        if (session->client_transport) {
            session->client_transport->close();
        }
    }

    // HTTP connection / request handling

    /// @brief Report a body that exceeded max_request_body_bytes_, ignoring a dead peer.
    static Task<void> write_payload_too_large(Connection& stream) {
        StringResponse response{http::status::payload_too_large, 11};
        response.set(http::field::server, "mcp-cpp-sdk");
        response.set(http::field::content_type, "application/json");
        response.keep_alive(false);
        response.body() = nlohmann::json{{"error", "Request body too large"}}.dump();
        response.prepare_payload();
        try {
            co_await http::async_write(stream, response, boost::asio::use_awaitable);
        } catch (const boost::system::system_error&) {
            // The peer that overran the limit may already be gone; nothing more to report to it.
            (void)0;
        }
    }

    Task<void> handle_connection(const std::shared_ptr<Connection>& connection,
                                 boost::asio::strand<boost::asio::any_io_executor> conn_strand) {
        namespace beast = boost::beast;

        auto& stream = *connection;
        beast::flat_buffer request_buffer;
        std::unique_ptr<Server> stateless_server;
        // Set once a watch has read the client's end of the stream. That end is consumed: a later
        // read on this connection would wait for an event that already happened.
        bool client_finished_sending = false;

        for (;;) {
            http::request_parser<http::string_body> parser;
            parser.body_limit(max_request_body_bytes_);
            bool body_too_large = false;
            try {
                if (client_finished_sending) {
                    // Only what the watch read ahead can still arrive: serve the requests it holds
                    // whole, and end the connection at the first that is not.
                    boost::system::error_code parse_error;
                    // Without eager, put() stops after the header and a whole buffered body would
                    // read as an incomplete request.
                    parser.eager(true);
                    request_buffer.consume(parser.put(request_buffer.data(), parse_error));
                    if (parse_error == http::error::body_limit) {
                        throw boost::system::system_error(parse_error);
                    }
                    if (parse_error || !parser.is_done()) {
                        break;
                    }
                } else {
                    co_await http::async_read(stream, request_buffer, parser,
                                              boost::asio::use_awaitable);
                }
            } catch (const boost::system::system_error& err) {
                if (err.code() == boost::asio::error::eof ||
                    err.code() == boost::asio::error::connection_reset ||
                    err.code() == boost::asio::error::operation_aborted) {
                    break;
                }
                if (err.code() != http::error::body_limit) {
                    throw;
                }
                body_too_large = true;
            }

            if (body_too_large) {
                // The parser cannot resynchronize after a truncated body, so the connection ends
                // with this answer rather than reading another request from it.
                co_await write_payload_too_large(stream);
                break;
            }
            StringRequest request = parser.release();

            if (closed.load(std::memory_order_acquire)) {
                break;
            }

            auto reply = co_await handle_request(request, conn_strand, stateless_server);
            if (closed.load(std::memory_order_acquire)) {
                if (auto* exchange = std::get_if<1>(&reply)) {
                    (*exchange)->cancelled->store(true, std::memory_order_relaxed);
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
            auto& response = std::get<0>(reply);
            const bool keep_connection_alive = response.keep_alive();
            co_await http::async_write(stream, response, boost::asio::use_awaitable);

            if (!keep_connection_alive) {
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

    Task<Reply> handle_request(const StringRequest& request,
                               boost::asio::strand<boost::asio::any_io_executor> conn_strand,
                               std::unique_ptr<Server>& stateless_server) {
        if (closed.load(std::memory_order_acquire)) {
            co_return detail_session_mgr::make_error_response(
                request, http::status::service_unavailable, "Transport closed");
        }

        const auto origin_it = request.find(http::field::origin);
        if (origin_it != request.end() && !allow_all_origins_ &&
            !allowed_origins_.contains(std::string(origin_it->value()))) {
            co_return detail_session_mgr::make_error_response(request, http::status::forbidden,
                                                              "Origin not allowed");
        }

        if (auto document = serve_protected_resource_metadata(request)) {
            co_return std::move(*document);
        }

        const bool unauthenticated_path = is_unauthenticated_path(request);
        if (!unauthenticated_path && (bearer_token_validator_ || async_bearer_token_validator_)) {
            const auto authorization_it = request.find(http::field::authorization);
            const auto token =
                authorization_it == request.end()
                    ? std::string_view{}
                    : http_bearer_token(detail::to_std_string_view(authorization_it->value()));
            // The synchronous path neither copies the token nor suspends; only an async validator
            // pays for either. A validator that throws has failed to decide, which is the server's
            // failure rather than the client's: answered 500, not with a challenge that would send
            // the client into re-authorization.
            bool accepted = false;
            bool validator_failed = false;
            try {
                if (token.empty()) {
                    accepted = false;
                } else if (async_bearer_token_validator_) {
                    accepted = co_await async_bearer_token_validator_(std::string(token));
                } else {
                    accepted = bearer_token_validator_(token);
                }
            } catch (...) {
                validator_failed = true;
            }
            if (validator_failed) {
                co_return detail_session_mgr::make_error_response(
                    request, http::status::internal_server_error, "Bearer token validation failed");
            }
            if (!accepted) {
                auto response = detail_session_mgr::make_error_response(
                    request, http::status::unauthorized, "Invalid bearer token");
                response.set(http::field::www_authenticate, www_authenticate_value_);
                co_return response;
            }
        }

        if (custom_handler) {
            auto custom_response = custom_handler(request);
            if (custom_response.has_value()) {
                co_return std::move(*custom_response);
            }
        }

        if (unauthenticated_path) {
            // An exempt path is excluded from MCP dispatch, not merely excused from the bearer
            // check. Serving MCP here would answer it with no authentication at all, so once the
            // metadata route and the custom handler have both declined, nothing is left to answer
            // it.
            co_return detail_session_mgr::make_error_response(request, http::status::not_found,
                                                              "Not found");
        }

        if (request.method() == http::verb::post) {
            co_return co_await handle_post(request, conn_strand, stateless_server);
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
            co_return co_await handle_get(request);
        }

        if (request.method() == http::verb::delete_) {
            co_return co_await handle_delete(request);
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
        nlohmann::json error_body = {
            {"jsonrpc", "2.0"},
            {"error",
             {{"code", g_METHOD_NOT_FOUND},
              {"message", detail::initialize_not_served_message(served)},
              {"data", {{"supported", std::move(served)}}}}},
            {"id", id.is_string() || id.is_number_integer() ? id : nlohmann::json(nullptr)}};
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
    static Task<std::string> dispatch_on_executor(Server* server, nlohmann::json request_json) {
        co_return co_await server->dispatch_request_direct(std::move(request_json));
    }

    Task<StringResponse> handle_stateless_post(const StringRequest& request,
                                               nlohmann::json request_json,
                                               std::unique_ptr<Server>& stateless_server) {
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

        const auto dispatch_executor = tool_executor_ ? tool_executor_ : executor;
        if (!stateless_server) {
            stateless_server = build_server(dispatch_executor);
        }

        auto response_body = co_await boost::asio::co_spawn(
            dispatch_executor, dispatch_on_executor(stateless_server.get(), std::move(request_json)),
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
                                   bool check_headers,
                                   boost::asio::strand<boost::asio::any_io_executor> conn_strand) {
        if (closed.load(std::memory_order_acquire)) {
            co_return detail_session_mgr::make_error_response(
                request, http::status::service_unavailable, "Transport closed");
        }

        // 2026-07-28 defines no notification a client sends over HTTP, so one is accepted and
        // dropped.
        if (!request_json.contains("id")) {
            co_return detail_session_mgr::make_empty_json_response(request, http::status::accepted);
        }
        if (!request_json.at("id").is_string() && !request_json.at("id").is_number_integer()) {
            co_return detail_session_mgr::make_jsonrpc_error_response(
                request, http::status::bad_request, g_INVALID_REQUEST,
                "JSON-RPC request id must be a string or an integer");
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
        if (check_headers) {
            request_context.param_headers = detail::mcp_param_headers(request);
        }
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
        const auto dispatch_executor = tool_executor_ ? tool_executor_ : executor;
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

    /// Writes the answer to a 2026-07-28 request as it is produced. Plain JSON, with the status the
    /// specification gives its error, unless a notification arrives first and the client accepts
    /// SSE: then an SSE stream carrying the notifications and finally the response. Meanwhile one
    /// read on the connection watches the client. Bytes it reads are the client's next request and
    /// stay in @p buffer; a reset, or a failed write, means the client has gone and cancels the
    /// request. A half-close is not a disconnect, since the client may still be reading: the
    /// stream is opened and probed until a write fails or the answer is complete.
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
                const auto error_code = detail::response_error_code(*response);
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

            std::optional<std::chrono::milliseconds> timeout;
            if (half_closed && exchange->streams) {
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
            } else if (stream_open) {
                const bool kept_alive = co_await write_chunk(stream, ": keep-alive\n\n");
                if (!kept_alive) {
                    client_gone = true;
                    break;
                }
            }
        }

        if (client_gone) {
            exchange->cancelled->store(true, std::memory_order_relaxed);
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

    std::variant<std::shared_ptr<detail_session_mgr::SessionRuntime>, StringResponse>
    resolve_session_for_post(const StringRequest& request, const nlohmann::json& request_json,
                             boost::asio::strand<boost::asio::any_io_executor> conn_strand) {
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
            return create_session(conn_strand);
        }

        std::string session_id_value(session_header_it->value());
        auto session = find_session(session_id_value);
        if (session == nullptr) {
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

    Task<Reply> handle_post(const StringRequest& request,
                            boost::asio::strand<boost::asio::any_io_executor> conn_strand,
                            std::unique_ptr<Server>& stateless_server) {
        if (closed.load(std::memory_order_acquire)) {
            co_return detail_session_mgr::make_error_response(
                request, http::status::service_unavailable, "Transport closed");
        }

        auto request_json = detail::parse_peer_json(request.body());
        if (request_json.is_discarded() || !request_json.is_object()) {
            co_return detail_session_mgr::make_error_response(request, http::status::bad_request,
                                                              "Invalid JSON-RPC payload");
        }

        const auto protocol_header_it = request.find("MCP-Protocol-Version");
        if (detail::is_modern_http_request(request_json,
                                           detail::http_header(request, "MCP-Protocol-Version"))) {
            co_return co_await handle_modern_post(request, std::move(request_json), true, conn_strand);
        }
        if (protocol_policy_ && !protocol_policy_->serve_legacy) {
            co_return co_await handle_modern_post(request, std::move(request_json), false, conn_strand);
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
            co_return co_await handle_stateless_post(request, std::move(request_json),
                                                     stateless_server);
        }

        // The Server answers a request whose id is neither a string nor an integer with a null id,
        // since it has none it may echo, so that answer could never be matched to this request.
        if (request_json.contains("id") && request_json.contains("method") &&
            !request_json.at("id").is_string() && !request_json.at("id").is_number_integer()) {
            co_return detail_session_mgr::make_jsonrpc_error_response(
                request, http::status::bad_request, g_INVALID_REQUEST,
                "JSON-RPC request id must be a string or an integer");
        }

        auto session_var = resolve_session_for_post(request, request_json, conn_strand);
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
                detail_session_mgr::protocol_header_value(protocol_header_it) !=
                    session->protocol_version()) {
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

        if (!request_json.contains("id") || !request_json.contains("method")) {
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
                detail_session_mgr::PendingResponse{timer_signal, std::nullopt, {}, false});
        }

        co_await session->client_transport->write_json(std::move(request_json));

        co_return co_await wait_for_response(session.get(), request, request_id_key, timer_signal);
    }

    Task<StringResponse> handle_delete(const StringRequest& request) {
        const auto session_header_it = request.find("Mcp-Session-Id");
        if (session_header_it == request.end()) {
            co_return detail_session_mgr::make_error_response(request, http::status::bad_request,
                                                              "Missing Mcp-Session-Id header");
        }

        std::string session_id_value(session_header_it->value());
        auto session = find_session(session_id_value);
        if (session == nullptr) {
            co_return detail_session_mgr::make_jsonrpc_error_response(
                request, http::status::not_found, g_INVALID_REQUEST, "Session not found");
        }

        const auto protocol_header_it = request.find("MCP-Protocol-Version");
        if (protocol_header_it != request.end() &&
            detail_session_mgr::protocol_header_value(protocol_header_it) !=
                session->protocol_version()) {
            co_return detail_session_mgr::make_error_response(request, http::status::bad_request,
                                                              "Invalid MCP-Protocol-Version header");
        }

        destroy_session(session_id_value);
        co_return detail_session_mgr::make_json_response(request, http::status::ok, "{}");
    }

    Task<StringResponse> handle_get(const StringRequest& request) {
        const auto session_header_it = request.find("Mcp-Session-Id");
        if (session_header_it == request.end()) {
            co_return detail_session_mgr::make_error_response(request, http::status::bad_request,
                                                              "Missing Mcp-Session-Id header");
        }

        std::string session_id_value(session_header_it->value());
        auto session = find_session(session_id_value);
        if (session == nullptr) {
            co_return detail_session_mgr::make_jsonrpc_error_response(
                request, http::status::not_found, g_INVALID_REQUEST, "Session not found");
        }

        const auto protocol_header_it = request.find("MCP-Protocol-Version");
        if (protocol_header_it != request.end() &&
            detail_session_mgr::protocol_header_value(protocol_header_it) !=
                session->protocol_version()) {
            co_return detail_session_mgr::make_error_response(request, http::status::bad_request,
                                                              "Invalid MCP-Protocol-Version header");
        }

        const auto last_event_id_it = request.find("Last-Event-ID");
        if (last_event_id_it != request.end()) {
            auto last_id = std::string(last_event_id_it->value());
            std::optional<SseEventList> missed_events;
            {
                std::lock_guard lock(session->response_mutex);
                missed_events = session->event_store.events_after(last_id);
            }
            if (!missed_events.has_value()) {
                co_return detail_session_mgr::make_error_response(
                    request, http::status::gone, "Event ID has been evicted from store");
            }
            if (!missed_events->empty()) {
                co_return detail_session_mgr::make_sse_response(request, *missed_events);
            }
        }

        co_return detail_session_mgr::make_empty_json_response(request, http::status::ok);
    }

    static void signal_timer(const std::shared_ptr<boost::asio::steady_timer>& timer) {
        boost::asio::post(timer->get_executor(), [timer]() { (void)timer->cancel(); });
    }

    static void dispatch_response_to_pending(detail_session_mgr::SessionRuntime* session,
                                             std::string message, bool json_only) {
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

            std::optional<std::pair<std::string, std::string>> stream_event;
            if (!json_only) {
                auto event_id = session->event_store.append(message);
                stream_event.emplace(std::move(event_id), message);
            }

            if (!is_response) {
                if (stream_event.has_value()) {
                    if (session->pending_responses.size() == 1) {
                        session->pending_responses.begin()->second.stream_events.push_back(
                            std::move(*stream_event));
                    } else if (!session->pending_responses.empty()) {
                        session->unassigned_stream_events.push_back(std::move(*stream_event));
                    }
                }
                return;
            }

            auto pending_it = session->pending_responses.find(request_id_key);
            if (pending_it == session->pending_responses.end()) {
                session->unassigned_stream_events.clear();
                return;
            }

            if (!json_only) {
                auto& events = pending_it->second.stream_events;
                events.insert(events.end(),
                              std::make_move_iterator(session->unassigned_stream_events.begin()),
                              std::make_move_iterator(session->unassigned_stream_events.end()));
                session->unassigned_stream_events.clear();
                events.push_back(std::move(*stream_event));
            }

            pending_it->second.response_body = std::move(message);
            pending_it->second.response_ready = true;
            timer_to_signal = pending_it->second.ready_timer;
        }

        signal_timer(timer_to_signal);
    }

    static Task<void> read_outbound_messages(
        std::shared_ptr<detail_session_mgr::SessionRuntime> session,
        std::shared_ptr<std::atomic<bool>> json_only_mode) {
        try {
            for (;;) {
                auto message = co_await detail::MemoryTransportAccess::read_message_until_closed(
                    *session->client_transport);
                if (!message) {
                    break;
                }
                dispatch_response_to_pending(session.get(), std::move(*message),
                                             json_only_mode->load(std::memory_order_acquire));
            }
        } catch (...) {
            // Any other failure ends the loop too, and releases the waiters below.
            (void)0;
        }
        {
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
                signal_timer(timer);
            }
        }
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
};

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
}

Task<void> StreamableHttpSessionManager::listen() {
    auto impl = impl_;
    (void)impl->begin_listening();
    return boost::asio::co_spawn(impl->listener_strand, listen_impl(impl), boost::asio::use_awaitable);
}

Task<void> StreamableHttpSessionManager::listen_impl(std::shared_ptr<Impl> impl) {
    for (;;) {
        if (impl->closed.load(std::memory_order_acquire)) {
            co_return;
        }

        boost::asio::ip::tcp::socket socket(impl->listener_strand);
        try {
            socket = co_await impl->acceptor.async_accept(boost::asio::use_awaitable);
        } catch (const boost::system::system_error& err) {
            if (impl->closed.load(std::memory_order_acquire) ||
                err.code() == boost::asio::error::operation_aborted) {
                co_return;
            }
            throw;
        }

        auto conn_strand = boost::asio::make_strand(impl->executor);
        auto native_handle = socket.release();
        boost::asio::ip::tcp::socket conn_socket(conn_strand, boost::asio::ip::tcp::v4(),
                                                 native_handle);
        auto connection = std::make_shared<Impl::Connection>(std::move(conn_socket));
        if (!impl->register_connection(connection)) {
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
                    throw;
                }
                impl->unregister_connection(connection);
            },
            [](const std::exception_ptr&) {
                // Connection errors (EOF, client disconnect) are normal;
                // handled per-connection, not propagated to the accept loop.
            });
    }
}

}  // namespace mcp

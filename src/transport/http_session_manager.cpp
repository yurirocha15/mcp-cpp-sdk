#include "../detail/accept_retry.hpp"
#include "../detail/beast_string.hpp"
#include "../detail/http_limits.hpp"
#include "../detail/jsonrpc_envelope.hpp"
#include "../detail/memory_transport_read.hpp"
#include "../detail/peer_json.hpp"

#include <mcp/core/constants.hpp>
#include <mcp/detail/secure_random.hpp>
#include <mcp/transport/http_session_manager.hpp>
#include <mcp/transport/memory.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
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
    /// Names the stream that answers this POST in the session's event store.
    std::string stream;
};

struct PendingResult {
    std::optional<std::string> response_body;
    SseEventList stream_events;
};

struct SessionRuntime {
    std::string session_id;

    std::shared_ptr<MemoryTransport> client_transport;

    mutable std::mutex protocol_mutex;
    std::string negotiated_protocol_version{std::string(g_LATEST_PROTOCOL_VERSION)};

    mutable std::mutex response_mutex;
    EventStore event_store;

    std::unordered_map<std::string, PendingResponse> pending_responses;
    /// Messages sent while several POSTs wait, without an id yet: each is stored, and given its
    /// id, once the next answer names the stream it travels on.
    std::vector<std::string> unassigned_stream_events;
    /// Counts the POST streams of this session, to name each one.
    std::uint64_t streams{0};

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

inline bool is_discover_request(const nlohmann::json& request_json) {
    return request_json.is_object() && request_json.contains("method") &&
           request_json.at("method").is_string() &&
           request_json.at("method").get<std::string>() == "server/discover";
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
    // Answers sessionless server/discover requests. Built once on the first one, so a peer that
    // needs no session or credential cannot make the factory run per request.
    std::mutex discover_server_mutex_;
    std::shared_ptr<Server> discover_server_;

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
        auto server = factory(executor);

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

    HttpConnectionLimits current_connection_limits() const {
        std::lock_guard lock(configuration_mutex_);
        return connection_limits_;
    }

    Task<void> handle_connection(const std::shared_ptr<Connection>& connection,
                                 boost::asio::strand<boost::asio::any_io_executor> conn_strand) {
        namespace beast = boost::beast;

        auto& stream = *connection;
        beast::flat_buffer request_buffer;
        std::unique_ptr<Server> stateless_server;
        const detail::RefuseBeforeBody refuse =
            [this](const StringRequest& head) -> Task<std::optional<StringResponse>> {
            co_return co_await refuse_before_body(head);
        };

        for (;;) {
            const auto limits = current_connection_limits();
            detail::RequestParser parser;
            parser.body_limit(max_request_body_bytes_);
            detail::BudgetLease bytes(budget_.bytes, limits.max_request_bytes);
            if (!co_await detail::read_request_within_limits(stream, request_buffer, parser, limits,
                                                             max_request_body_bytes_, bytes, refuse)) {
                break;
            }
            StringRequest request = detail::take_request(parser);

            if (closed.load(std::memory_order_acquire)) {
                break;
            }

            detail::BudgetLease slot(budget_.requests, limits.max_requests);
            StringResponse response;
            if (slot.grow_to(1)) {
                response = co_await handle_request(request, conn_strand, stateless_server, bytes);
            } else {
                response = detail_session_mgr::make_error_response(
                    request, http::status::service_unavailable, "Too many requests");
            }
            if (closed.load(std::memory_order_acquire)) {
                break;
            }
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

    /// Origin and bearer checks: they need only the headers, so a request they refuse never has its
    /// body read.
    Task<std::optional<StringResponse>> refuse_before_body(const StringRequest& request) {
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

        co_return std::nullopt;
    }

    Task<StringResponse> handle_request(const StringRequest& request,
                                        boost::asio::strand<boost::asio::any_io_executor> conn_strand,
                                        std::unique_ptr<Server>& stateless_server,
                                        detail::BudgetLease& bytes) {
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
            co_return co_await handle_post(request, conn_strand, stateless_server, bytes);
        }

        if (stateless_json_mode_) {
            auto response = detail_session_mgr::make_error_response(
                request, http::status::method_not_allowed, "Method not allowed");
            response.set(http::field::allow, "POST");
            co_return response;
        }

        if (request.method() == http::verb::get) {
            co_return co_await handle_get(request, bytes);
        }

        if (request.method() == http::verb::delete_) {
            co_return co_await handle_delete(request);
        }

        auto response = detail_session_mgr::make_error_response(
            request, http::status::method_not_allowed, "Method not allowed");
        response.set(http::field::allow, "GET, POST, DELETE");
        co_return response;
    }

    bool has_valid_protocol_header(const StringRequest& request) const {
        const auto protocol_header_it = request.find("MCP-Protocol-Version");
        return protocol_header_it == request.end() ||
               is_supported_protocol_version(
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
        const bool is_discover = detail_session_mgr::is_discover_request(request_json);
        // server/discover advertises protocol versions (see g_DISCOVERABLE_PROTOCOL_VERSIONS)
        // outside g_SUPPORTED_PROTOCOL_VERSIONS, which alone backs has_valid_protocol_header;
        // exempt it from that check exactly like initialize is exempted, rather than widening
        // the check itself for every method.
        if (!is_initialize && !is_discover && !has_valid_protocol_header(request)) {
            co_return detail_session_mgr::make_error_response(request, http::status::bad_request,
                                                              "Invalid MCP-Protocol-Version header");
        }

        if (!request_json.contains("id") || !request_json.contains("method")) {
            co_return detail_session_mgr::make_empty_json_response(request, http::status::accepted);
        }

        const auto dispatch_executor = tool_executor_ ? tool_executor_ : executor;
        if (!stateless_server) {
            stateless_server = factory(dispatch_executor);
        }

        auto response_body = co_await boost::asio::co_spawn(
            dispatch_executor, dispatch_on_executor(stateless_server.get(), std::move(request_json)),
            boost::asio::use_awaitable);
        co_return detail_session_mgr::make_json_response(request, http::status::ok,
                                                         std::move(response_body));
    }

    /// server/discover is a pre-gate method: it MUST be reachable with zero prior state, so a
    /// sessionless discover request in stateful mode is dispatched directly against a
    /// throwaway Server instance instead of going through resolve_session_for_post — no
    /// session is created, registered, or otherwise touched, and no Mcp-Session-Id is issued.
    Task<StringResponse> handle_sessionless_discover(const StringRequest& request,
                                                     nlohmann::json request_json) {
        if (closed.load(std::memory_order_acquire)) {
            co_return detail_session_mgr::make_error_response(
                request, http::status::service_unavailable, "Transport closed");
        }

        const auto dispatch_executor = tool_executor_ ? tool_executor_ : executor;
        std::shared_ptr<Server> discover_server;
        {
            std::lock_guard lock(discover_server_mutex_);
            if (!discover_server_) {
                discover_server_ = factory(dispatch_executor);
            }
            discover_server = discover_server_;
        }

        auto response_body = co_await boost::asio::co_spawn(
            dispatch_executor, dispatch_on_executor(discover_server.get(), std::move(request_json)),
            boost::asio::use_awaitable);
        co_return detail_session_mgr::make_json_response(request, http::status::ok,
                                                         std::move(response_body));
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
            if (at_session_limit()) {
                return detail_session_mgr::make_error_response(
                    request, http::status::service_unavailable, "Too many sessions");
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

    Task<StringResponse> handle_post(const StringRequest& request,
                                     boost::asio::strand<boost::asio::any_io_executor> conn_strand,
                                     std::unique_ptr<Server>& stateless_server,
                                     detail::BudgetLease& bytes) {
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
        const bool is_initialize = detail_session_mgr::is_initialize_request(request_json);
        if (is_initialize && protocol_header_it != request.end() &&
            !is_supported_protocol_version(
                detail_session_mgr::protocol_header_value(protocol_header_it))) {
            co_return detail_session_mgr::make_error_response(request, http::status::bad_request,
                                                              "Invalid MCP-Protocol-Version header");
        }

        if (stateless_json_mode_) {
            co_return co_await handle_stateless_post(request, std::move(request_json),
                                                     stateless_server);
        }

        // The Server answers a request whose id is neither a string nor an integer within int64_t
        // with a null id, since it has none it may echo, so that answer could never be matched to
        // this request.
        if (request_json.contains("id") && request_json.contains("method") &&
            !detail::is_request_id(request_json.at("id"))) {
            co_return detail_session_mgr::make_jsonrpc_error_response(
                request, http::status::bad_request, g_INVALID_REQUEST,
                "JSON-RPC request id must be a string or an integer within int64_t");
        }

        if (detail_session_mgr::is_discover_request(request_json) &&
            request.find("Mcp-Session-Id") == request.end()) {
            co_return co_await handle_sessionless_discover(request, std::move(request_json));
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
                    .value_or(std::string(g_LATEST_PROTOCOL_VERSION));
            session->set_protocol_version(
                std::string(negotiate_protocol_version(requested_protocol_version)));
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

        auto response =
            co_await wait_for_response(session.get(), request, request_id_key, timer_signal);
        if (created_here && !session->established.load(std::memory_order_acquire)) {
            destroy_session(session->session_id);
        }
        co_return response;
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

    Task<StringResponse> handle_get(const StringRequest& request, detail::BudgetLease& bytes) {
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
            // Each replay holds its own copy of the events until the client has read it, so the
            // copies count against what requests may hold.
            std::size_t replay_bytes = 0;
            for (const auto& [id, data] : *missed_events) {
                replay_bytes += 2 * (id.size() + data.size());
            }
            if (!bytes.grow_to(replay_bytes)) {
                co_return detail_session_mgr::make_error_response(
                    request, http::status::service_unavailable, "Server busy");
            }
            // An empty stream is an answer here too: the client asked to resume one.
            co_return detail_session_mgr::make_sse_response(request, *missed_events);
        }

        // Answers travel on the response to each POST, so there is no stream to open.
        auto refusal = detail_session_mgr::make_error_response(
            request, http::status::method_not_allowed, "This server offers no SSE stream on GET");
        refusal.set(http::field::allow, "POST, DELETE");
        co_return refusal;
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

            // An event is stored under the stream it is sent on, so a resume of one POST's stream
            // replays nothing that another POST's stream carried.
            const auto send_on = [session](detail_session_mgr::PendingResponse& pending,
                                           std::string event) {
                auto event_id = session->event_store.append(event, pending.stream);
                pending.stream_events.emplace_back(std::move(event_id), std::move(event));
            };

            if (!is_response) {
                if (!json_only) {
                    if (session->pending_responses.size() == 1) {
                        send_on(session->pending_responses.begin()->second, std::move(message));
                    } else if (!session->pending_responses.empty()) {
                        session->unassigned_stream_events.push_back(std::move(message));
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
                for (auto& event : session->unassigned_stream_events) {
                    send_on(pending_it->second, std::move(event));
                }
                session->unassigned_stream_events.clear();
                send_on(pending_it->second, message);
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
                if (!session->established.load(std::memory_order_acquire)) {
                    const auto parsed = detail::parse_peer_json(*message);
                    session->established.store(parsed.is_object() && parsed.contains("result"),
                                               std::memory_order_release);
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
        auto conn_strand = boost::asio::make_strand(impl->executor);
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

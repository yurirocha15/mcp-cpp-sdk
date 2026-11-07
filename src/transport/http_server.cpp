#include "../detail/accept_retry.hpp"
#include "../detail/beast_string.hpp"
#include "../detail/http_limits.hpp"
#include "../detail/http_request_metadata.hpp"
#include "../detail/modern_methods.hpp"
#include "../detail/peer_json.hpp"
#include "../detail/principal.hpp"
#include "../detail/socket_probe.hpp"

#include <mcp/detail/secure_random.hpp>
#include <mcp/transport/http_server.hpp>
#include <mcp/transport/http_types.hpp>

#include <algorithm>
#include <atomic>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <queue>
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

struct HttpServerTransport::Impl {
    /// How many sessionless requests (those of revision 2026-07-28) may wait for the server at
    /// once. They share the incoming queue with the session's own requests.
    static constexpr std::size_t g_max_pending_sessionless_requests = 256;
    static constexpr auto g_disconnect_probe_interval = std::chrono::milliseconds(250);

    using Connection = beast::tcp_stream;

    struct SharedState {
        explicit SharedState(boost::asio::strand<boost::asio::any_io_executor>& execution_strand)
            : timer(execution_strand) {}

        boost::asio::steady_timer timer;
        std::queue<IncomingMessage> queue;
        std::atomic<bool> closed{false};
        bool read_active{false};
    };

    struct PendingResponse {
        std::shared_ptr<boost::asio::steady_timer> ready_timer;
        std::optional<std::string> response_body;
        std::optional<std::string> session_header;
        std::optional<std::string> event_id;
        /// Set only for a request carried under a sentinel id. Holds the id the caller actually
        /// sent, so run_write can put it back before the response leaves the transport.
        std::optional<nlohmann::json> client_request_id;
        bool response_ready{false};
        /// Set when a DELETE ended the session this request was sent in.
        bool session_ended{false};
        /// The connection writes the request's events as they come, on the stream named `stream`
        /// in the event store.
        bool streams{false};
        std::string stream;
        /// Events of that stream the connection has not taken yet.
        SseEventList stream_events;
        /// Which connection serves the stream: a resume takes it over from the one before.
        std::uint64_t generation{0};
        /// No connection serves the stream; its events wait in the event store for a resume.
        bool detached{false};
    };

    /// A stream of the legacy session that a connection writes as its events come: the answer to
    /// a POST, or the stream a GET opened.
    struct LegacyStream {
        /// The key of the POST's request, or empty for the GET stream.
        std::string request_key;
        /// The PendingResponse::generation or get_generation this connection serves.
        std::uint64_t generation{0};
        /// Woken when the stream has something new.
        std::shared_ptr<boost::asio::steady_timer> signal;
        /// Events to write before any other: those a resumed stream missed, or the priming event.
        SseEventList replay;
        /// Opens the stream at once rather than at its first event, as a GET does.
        bool open_at_once{false};
    };

    /// A request handled by handle_request: a complete response, or a stream serve_legacy_stream
    /// writes as its events come.
    using Reply = std::variant<StringResponse, std::shared_ptr<LegacyStream>>;

    /// Events a stream may have waiting to be written; more are dropped. Each is also in the
    /// event store, where a resume finds it.
    static constexpr std::size_t g_max_queued_events = 4096;
    /// How often an open stream carries an SSE comment, so idle intermediaries keep it open.
    static constexpr auto g_keepalive_interval = std::chrono::seconds(15);

    struct PendingResult {
        std::optional<std::string> response_body;
        std::optional<std::string> session_header;
        std::optional<std::string> event_id;
        bool session_ended{false};
    };

    struct SessionCheckResult {
        bool ok{false};
        std::string error_message;
        http::status status{http::status::bad_request};
    };

    /// A request naming a session this transport does not hold for its sender: one that ended, was
    /// taken over by another client's initialize, belongs to another principal, or never existed.
    /// Like a terminated session, it is not found, so a client starts a new one.
    static SessionCheckResult session_not_found() {
        return SessionCheckResult{false, "Session not found", http::status::not_found};
    }

    Impl(const boost::asio::any_io_executor& executor, std::string host, unsigned short port,
         std::size_t event_store_capacity)
        : host(std::move(host)),
          port(port),
          strand(boost::asio::make_strand(executor)),
          acceptor(strand),
          state(std::make_shared<SharedState>(strand)),
          event_store(event_store_capacity) {
        state->timer.expires_at(std::chrono::steady_clock::time_point::max());

        boost::system::error_code ec;
        const auto bind_address = boost::asio::ip::make_address(this->host, ec);
        if (ec) {
            throw std::runtime_error("Invalid bind address: " + this->host);
        }

        const auto endpoint = boost::asio::ip::tcp::endpoint(bind_address, this->port);
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

    static bool is_initialize_result_response(const nlohmann::json& response_json) {
        if (!response_json.is_object() || !response_json.contains("result")) {
            return false;
        }
        const auto& result_node = response_json.at("result");
        return result_node.is_object() && result_node.contains("protocolVersion");
    }

    static bool is_initialize_request(const nlohmann::json& request_json) {
        return request_json.is_object() && request_json.contains("method") &&
               request_json.at("method").is_string() &&
               request_json.at("method").get<std::string>() == "initialize";
    }

    static std::string_view header_value(const StringRequest::const_iterator& header_it) {
        return {header_it->value().data(), header_it->value().size()};
    }

    static std::string generate_session_id() { return detail::generate_secure_session_id(); }

    void ensure_configurable() const {
        if (listening_started || state->closed.load(std::memory_order_acquire)) {
            throw std::logic_error("HttpServerTransport configuration must be set before listen()");
        }
    }

    bool begin_listening() {
        std::lock_guard lock(configuration_mutex);
        if (state->closed.load(std::memory_order_acquire)) {
            return false;
        }
        if (listening_started) {
            throw std::logic_error("HttpServerTransport::listen() may only be called once");
        }
        listening_started = true;
        return true;
    }

    static void close_connection(const std::shared_ptr<Connection>& connection) {
        boost::system::error_code ignored;
        (void)connection->socket().cancel(ignored);
        (void)connection->socket().shutdown(boost::asio::ip::tcp::socket::shutdown_both, ignored);
        (void)connection->socket().close(ignored);
    }

    void close_active_connections() {
        for (const auto& connection : active_connections) {
            close_connection(connection);
        }
    }

    /// @brief Render the challenge a 401 will carry, borrowing the metadata URL when unset.
    std::string render_challenge(const BearerChallengeConfig& challenge) const {
        auto effective = challenge;
        if (effective.resource_metadata.empty() && protected_resource_metadata.has_value()) {
            effective.resource_metadata = protected_resource_metadata_url(*protected_resource_metadata);
        }
        return format_www_authenticate(effective);
    }

    bool is_unauthenticated_path(const StringRequest& request) const {
        if (unauthenticated_paths.empty()) {
            return false;
        }
        const auto path = http_request_path(detail::to_std_string_view(request.target()));
        return unauthenticated_paths.contains(std::string(path));
    }

    bool is_origin_allowed(std::string_view origin_value) const {
        if (allow_all_origins) {
            return true;
        }
        return allowed_origins.contains(std::string(origin_value));
    }

    /// Queues a message with the principal that sent it, so the server reads the two together.
    void enqueue_incoming_message(std::string message_payload, std::string principal) const {
        state->queue.push(IncomingMessage{std::move(message_payload), std::move(principal)});
        state->timer.cancel();
    }

    template <typename Body>
    static void set_common_headers(http::response<Body>& response, bool keep_alive) {
        response.set(http::field::server, "mcp-cpp-sdk");
        response.keep_alive(keep_alive);
    }

    static StringResponse make_json_response(const StringRequest& request, http::status status_code,
                                             std::string body) {
        StringResponse response{status_code, request.version()};
        set_common_headers(response, request.keep_alive());
        response.set(http::field::content_type, "application/json");
        response.body() = std::move(body);
        response.prepare_payload();
        return response;
    }

    static StringResponse make_error_response(const StringRequest& request, http::status status_code,
                                              std::string_view error_message) {
        nlohmann::json error_body = {{"error", std::string(error_message)}};
        return make_json_response(request, status_code, error_body.dump());
    }

    static StringResponse make_empty_json_response(const StringRequest& request,
                                                   http::status status_code) {
        auto response = make_json_response(request, status_code, "");
        response.content_length(0);
        return response;
    }

    static StringResponse make_sse_response(const StringRequest& request, const std::string& event_id,
                                            const std::string& data) {
        std::string sse_body = "id: " + event_id + "\ndata: " + data + "\n\n";
        StringResponse response{http::status::ok, request.version()};
        set_common_headers(response, request.keep_alive());
        response.set(http::field::content_type, "text/event-stream");
        response.set(http::field::cache_control, "no-cache");
        response.body() = std::move(sse_body);
        response.prepare_payload();
        return response;
    }

    static StringResponse make_sse_replay_response(const StringRequest& request,
                                                   const SseEventList& events) {
        std::string sse_body;
        for (const auto& [id, data] : events) {
            sse_body += "id: ";
            sse_body += id;
            sse_body += "\ndata: ";
            sse_body += data;
            sse_body += "\n\n";
        }
        StringResponse response{http::status::ok, request.version()};
        set_common_headers(response, request.keep_alive());
        response.set(http::field::content_type, "text/event-stream");
        response.set(http::field::cache_control, "no-cache");
        response.body() = std::move(sse_body);
        response.prepare_payload();
        return response;
    }

    /// A session belongs to the principal that created it, when a validator checks the tokens.
    /// Without one the Authorization header is not checked here, and a client may change it.
    [[nodiscard]] bool session_belongs_to(const std::string& principal) const {
        return !session_id.has_value() || (!bearer_token_validator && !async_bearer_token_validator) ||
               session_principal == principal;
    }

    /// Whether a client without the session's id may end it: nothing has used it for the idle
    /// timeout, and no request of it is waiting.
    bool session_left_idle(const StringRequest& request) const {
        std::chrono::milliseconds timeout{};
        {
            std::lock_guard lock(configuration_mutex);
            timeout = session_idle_timeout;
        }
        return session_active && timeout > std::chrono::milliseconds::zero() &&
               request.find("MCP-Session-Id") == request.end() && !session_request_waiting() &&
               std::chrono::steady_clock::now() - session_last_used >= timeout;
    }

    /// Whether a request of the session waits for its answer. Requests sent without a session, such
    /// as a 2026-07-28 client's, belong to no session and do not keep it in use.
    [[nodiscard]] bool session_request_waiting() const {
        return std::any_of(
            pending_responses.begin(), pending_responses.end(),
            [this](const auto& entry) { return !sessionless_request_ids.contains(entry.first); });
    }

    Task<SessionCheckResult> validate_post_session(const StringRequest& request) {
        const auto session_header_it = request.find("MCP-Session-Id");
        const bool session_header_present = session_header_it != request.end();

        if (session_id.has_value()) {
            if (!session_header_present) {
                co_return SessionCheckResult{false, "Session active"};
            }
            if (session_header_it->value() != *session_id) {
                co_return session_not_found();
            }
            co_return SessionCheckResult{true, {}};
        }

        if (session_active && !session_header_present) {
            co_return SessionCheckResult{false, "Session active"};
        }

        if (session_header_present) {
            co_return session_not_found();
        }

        co_return SessionCheckResult{true, {}};
    }

    Task<SessionCheckResult> validate_delete_session(const StringRequest& request) {
        if (!session_id.has_value()) {
            co_return SessionCheckResult{true, {}};
        }

        const auto session_header_it = request.find("MCP-Session-Id");
        if (session_header_it == request.end()) {
            co_return SessionCheckResult{false, "Missing MCP-Session-Id header"};
        }

        if (session_header_it->value() != *session_id) {
            co_return session_not_found();
        }

        co_return SessionCheckResult{true, {}};
    }

    Task<std::optional<std::shared_ptr<boost::asio::steady_timer>>> register_pending_request(
        const std::string& request_id_key,
        std::optional<nlohmann::json> client_request_id = std::nullopt) {
        if (pending_responses.contains(request_id_key)) {
            co_return std::nullopt;
        }

        auto timer_signal = std::make_shared<boost::asio::steady_timer>(strand);
        timer_signal->expires_at(std::chrono::steady_clock::time_point::max());

        pending_responses.emplace(
            request_id_key, PendingResponse{timer_signal, std::nullopt, std::nullopt, std::nullopt,
                                            std::move(client_request_id), false});
        co_return timer_signal;
    }

    /// A transport-private id for a request that arrives with no session credential. The random
    /// prefix is drawn once per transport from the same secure source as the session id, so a
    /// peer cannot construct an id that collides with a live sentinel, and the counter keeps
    /// concurrent sentinels distinct from each other.
    std::string next_sentinel_request_id() {
        if (sentinel_id_prefix.empty()) {
            sentinel_id_prefix = "mcp-pregate-" + generate_session_id() + "-";
        }
        return sentinel_id_prefix + std::to_string(++sentinel_id_counter);
    }

    /// Whether @p request_id_key is a sentinel. An abandoned request has left the set of waiting
    /// sessionless requests by the time its handler answers.
    bool is_sentinel_key(const std::string& request_id_key) const {
        return !sentinel_id_prefix.empty() &&
               request_id_key.compare(1, sentinel_id_prefix.size(), sentinel_id_prefix) == 0;
    }

    Task<bool> is_response_ready(const std::string& request_id_key) {
        const auto pending_it = pending_responses.find(request_id_key);
        if (pending_it == pending_responses.end()) {
            co_return true;
        }

        co_return pending_it->second.response_ready;
    }

    Task<PendingResult> consume_pending_response(const std::string& request_id_key) {
        const auto pending_it = pending_responses.find(request_id_key);
        if (pending_it == pending_responses.end()) {
            co_return PendingResult{};
        }

        PendingResult pending_result{
            std::move(pending_it->second.response_body), std::move(pending_it->second.session_header),
            std::move(pending_it->second.event_id), pending_it->second.session_ended};
        pending_responses.erase(pending_it);
        sessionless_request_ids.erase(request_id_key);
        co_return pending_result;
    }

    Task<void> terminate_session() {
        // Only a DELETE that ends an established session counts; one sent before the session
        // exists ends nothing.
        const bool ended = session_active;
        session_id.reset();
        session_active = false;
        if (ended) {
            terminated_sessions.fetch_add(1, std::memory_order_release);
            session_ended_awaiting_next = true;
            // Nothing of the ended session carries over to the next client: not its replay
            // history, and not the requests still waiting for an answer, which the Server no
            // longer sends once it sees the session has ended.
            event_store.clear();
            for (auto it = pending_responses.begin(); it != pending_responses.end();) {
                auto& [request_id_key, pending] = *it;
                if (sessionless_request_ids.contains(request_id_key)) {
                    ++it;
                    continue;  // a 2026-07-28 request belongs to no session
                }
                if (pending.detached) {
                    // Nobody waits for it, and nobody can resume its stream now.
                    it = pending_responses.erase(it);
                    continue;
                }
                pending.session_ended = true;
                pending.response_ready = true;
                pending.ready_timer->cancel();
                ++it;
            }
            end_get_stream();
        }
        co_return;
    }

    /// Ends the GET stream; the connection serving it sees it was replaced, and closes.
    void end_get_stream() {
        if (get_timer) {
            get_timer->cancel();
        }
        get_timer.reset();
        ++get_generation;
        get_stream.clear();
        get_events.clear();
    }

    /// Sends a message other than a response: on the stream of the request it relates to while
    /// that request waits on one, else on the GET stream, else on the stream of the only request
    /// waiting on one. With nowhere to go it is dropped, as it is when nothing streams.
    void route_event(std::string message, const std::string* related_request) {
        if (json_only_.load(std::memory_order_acquire) || !session_active ||
            session_ended_awaiting_next) {
            return;
        }
        PendingResponse* target = nullptr;
        if (related_request != nullptr) {
            const auto related = pending_responses.find(*related_request);
            if (related != pending_responses.end() && related->second.streams) {
                target = &related->second;
            }
        }
        if (target == nullptr && get_stream.empty()) {
            std::size_t streaming = 0;
            for (auto& [key, pending] : pending_responses) {
                if (pending.streams) {
                    ++streaming;
                    target = &pending;
                }
            }
            if (streaming != 1) {
                target = nullptr;
            }
        }
        if (target != nullptr) {
            auto event_id = event_store.append(message, target->stream);
            if (!target->detached && target->stream_events.size() < g_max_queued_events) {
                target->stream_events.emplace_back(std::move(event_id), std::move(message));
                target->ready_timer->cancel();
            }
            return;
        }
        if (!get_stream.empty()) {
            auto event_id = event_store.append(message, get_stream);
            if (get_timer && get_events.size() < g_max_queued_events) {
                get_events.emplace_back(std::move(event_id), std::move(message));
                get_timer->cancel();
            }
        }
    }

    /// Leaves the stream of @p legacy without a connection, unless a resume took it over: its
    /// events then wait in the event store.
    void detach_stream(const LegacyStream& legacy) {
        if (legacy.request_key.empty()) {
            if (get_generation == legacy.generation) {
                get_timer.reset();
                get_events.clear();
            }
            return;
        }
        const auto pending = pending_responses.find(legacy.request_key);
        if (pending != pending_responses.end() && pending->second.generation == legacy.generation) {
            pending->second.detached = true;
            pending->second.stream_events.clear();
        }
    }

    // 2026-07-28 has no initialize; the error names the versions served, as an initialize error
    // should.
    StringResponse initialize_not_served_response(const StringRequest& request,
                                                  const nlohmann::json& request_json) const {
        std::vector<std::string> served;
        {
            std::lock_guard lock(configuration_mutex);
            served = served_versions;
        }
        const auto id = request_json.value("id", nlohmann::json(nullptr));
        auto message = detail::initialize_not_served_message(served);
        nlohmann::json error_body = {{"jsonrpc", "2.0"},
                                     {"error",
                                      {{"code", g_METHOD_NOT_FOUND},
                                       {"message", std::move(message)},
                                       {"data", {{"supported", std::move(served)}}}}}};
        if (detail::is_request_id(id)) {
            error_body["id"] = id;
        }
        return make_json_response(request, http::status::not_found, error_body.dump());
    }

    /// Whether any version the Server serves is one that initialize negotiates.
    bool serves_legacy() const {
        std::lock_guard lock(configuration_mutex);
        return std::any_of(
            served_versions.begin(), served_versions.end(),
            [](const std::string& version) { return is_legacy_protocol_version(version); });
    }

    std::optional<StringResponse> check_protocol_version(const StringRequest& request,
                                                         const nlohmann::json& request_json) const {
        const auto protocol_header_it = request.find("MCP-Protocol-Version");
        const bool is_initialize = is_initialize_request(request_json);
        if (is_initialize) {
            if (protocol_header_it == request.end()) {
                return std::nullopt;
            }
            const auto version = header_value(protocol_header_it);
            if (is_legacy_protocol_version(version)) {
                return std::nullopt;
            }
            if (is_modern_protocol_version(version)) {
                return initialize_not_served_response(request, request_json);
            }
            return make_error_response(request, http::status::bad_request,
                                       "Invalid MCP-Protocol-Version header");
        }

        // Besides the version the session negotiated, any version initialize can negotiate is
        // accepted, as the TypeScript, Go and Rust SDKs do; the session keeps its own version.
        if (protocol_header_it == request.end() ||
            header_value(protocol_header_it) == negotiated_protocol_version ||
            is_legacy_protocol_version(header_value(protocol_header_it))) {
            return std::nullopt;
        }

        if (!is_initialize) {
            return make_error_response(request, http::status::bad_request,
                                       "Invalid MCP-Protocol-Version header");
        }
        return std::nullopt;
    }

    std::optional<StringResponse> check_origin(const StringRequest& request) const {
        const auto origin_header_it = request.find(http::field::origin);
        if (origin_header_it != request.end() &&
            !is_origin_allowed(detail::to_std_string_view(origin_header_it->value()))) {
            return make_error_response(request, http::status::forbidden, "Origin not allowed");
        }
        return std::nullopt;
    }

    /// @brief Answer a GET for the configured RFC 9728 document, which needs no bearer token.
    std::optional<StringResponse> serve_protected_resource_metadata(
        const StringRequest& request) const {
        if (!protected_resource_metadata.has_value() || request.method() != http::verb::get) {
            return std::nullopt;
        }
        if (http_request_path(detail::to_std_string_view(request.target())) !=
            protected_resource_metadata_path) {
            return std::nullopt;
        }
        if (auto error = check_origin(request)) {
            return error;
        }
        return make_json_response(request, http::status::ok, protected_resource_metadata_body);
    }

    StringResponse make_unauthorized_response(const StringRequest& request) const {
        auto response =
            make_error_response(request, http::status::unauthorized, "Invalid bearer token");
        response.set(http::field::www_authenticate, www_authenticate_value);
        return response;
    }

    /// A validator that throws has failed to decide. That is the server's failure, not the
    /// client's, so it is answered 500 rather than with a challenge that would send the client into
    /// re-authorization.
    static StringResponse make_validator_failure_response(const StringRequest& request) {
        return make_error_response(request, http::status::internal_server_error,
                                   "Bearer token validation failed");
    }

    static std::string_view request_authorization(const StringRequest& request) {
        const auto authorization_it = request.find(http::field::authorization);
        return authorization_it == request.end() ? std::string_view{} : header_value(authorization_it);
    }

    /// What the bearer check decided: the response refusing the request, or the principal the
    /// request is attributed to.
    struct Authorization {
        std::optional<StringResponse> refusal;
        std::string principal;
    };

    /// @brief Run the bearer check when it can be decided without suspending.
    /// @details Without a validator, the request is attributed to a digest of its Authorization
    ///          header. With an async one, the decision belongs to check_authorization_async and
    ///          this returns no refusal and no principal; keeping the two apart leaves the far
    ///          more common synchronous path free of a coroutine frame and of copying the token.
    Authorization check_authorization(const StringRequest& request) const {
        const auto authorization = request_authorization(request);
        if (async_bearer_token_validator) {
            return {};
        }
        if (!bearer_token_validator) {
            return {std::nullopt, detail::request_principal(std::nullopt, authorization)};
        }

        const auto token = http_bearer_token(authorization);
        if (token.empty()) {
            return {make_unauthorized_response(request), {}};
        }
        std::optional<TokenValidation> validation;
        try {
            validation = bearer_token_validator(token);
        } catch (...) {
            return {make_validator_failure_response(request), {}};
        }
        if (!validation->accepted) {
            return {make_unauthorized_response(request), {}};
        }
        return {std::nullopt, detail::request_principal(validation->principal_id, authorization)};
    }

    /// @brief Run the bearer check against an async validator. Only entered when one is installed.
    Task<Authorization> check_authorization_async(const StringRequest& request) const {
        const auto authorization = request_authorization(request);
        const auto token = http_bearer_token(authorization);
        if (token.empty()) {
            co_return Authorization{make_unauthorized_response(request), {}};
        }
        std::optional<TokenValidation> validation;
        try {
            validation = co_await async_bearer_token_validator(std::string(token));
        } catch (...) {
            co_return Authorization{make_validator_failure_response(request), {}};
        }
        if (!validation->accepted) {
            co_return Authorization{make_unauthorized_response(request), {}};
        }
        co_return Authorization{std::nullopt,
                                detail::request_principal(validation->principal_id, authorization)};
    }

    static StringResponse make_jsonrpc_error_response(const StringRequest& request,
                                                      http::status status_code, int code,
                                                      std::string message,
                                                      nlohmann::json id = nullptr) {
        nlohmann::json body = {{"jsonrpc", "2.0"},
                               {"error", {{"code", code}, {"message", std::move(message)}}}};
        // An error that names no request leaves out `id`, as MCP's schema has it.
        if (!id.is_null()) {
            body["id"] = std::move(id);
        }
        return make_json_response(request, status_code, body.dump());
    }

    static StringResponse make_invalid_id_response(const StringRequest& request) {
        return make_jsonrpc_error_response(
            request, http::status::bad_request, g_INVALID_REQUEST,
            "JSON-RPC request id must be a string or an integer within int64_t");
    }

    /// Waits until the server has answered @p request_id_key, or returns the response to send
    /// instead when the transport closes first.
    Task<std::optional<StringResponse>> wait_for_response(
        const StringRequest& request, const std::string& request_id_key,
        const std::shared_ptr<boost::asio::steady_timer>& timer_signal, Connection* watched = nullptr) {
        for (;;) {
            if (state->closed.load(std::memory_order_acquire)) {
                co_await consume_pending_response(request_id_key);
                co_return make_error_response(request, http::status::internal_server_error,
                                              "Transport closed while waiting response");
            }

            const bool ready = co_await is_response_ready(request_id_key);
            if (ready) {
                break;
            }

            if (watched != nullptr) {
                timer_signal->expires_after(g_disconnect_probe_interval);
            }
            bool probe_due = watched != nullptr;
            try {
                co_await timer_signal->async_wait(boost::asio::use_awaitable);
            } catch (const boost::system::system_error& err) {
                if (err.code() != boost::asio::error::operation_aborted) {
                    throw;
                }
                probe_due = false;
            }
            if (probe_due && detail::probe_peer(watched->socket()) != detail::PeerState::eConnected) {
                co_await consume_pending_response(request_id_key);
                auto gone =
                    make_error_response(request, http::status::service_unavailable, "The client left");
                gone.keep_alive(false);
                co_return gone;
            }
        }

        co_return std::nullopt;
    }

    /// Tells the Server that nobody waits for the request any more, as the session manager does
    /// when its stream closes. 2026-07-28 has no other way to say so over HTTP.
    void cancel_abandoned_request(const std::string& sentinel_id, std::string principal) const {
        nlohmann::json notification = {{"jsonrpc", "2.0"},
                                       {"method", "notifications/cancelled"},
                                       {"params", {{"requestId", sentinel_id}}}};
        enqueue_incoming_message(notification.dump(), std::move(principal));
    }

    /// Serves a request of protocol revision 2026-07-28. It needs no session: Mcp-Session-Id is
    /// neither read nor sent, the request is carried under a transport-private sentinel id so two
    /// clients may use the same id, and its answer is plain JSON with the status the specification
    /// gives its error, kept out of the session's replay store.
    Task<StringResponse> handle_modern_post(const StringRequest& request,
                                            const nlohmann::json& request_json, std::string principal,
                                            std::shared_ptr<Connection> connection) {
        // 2026-07-28 defines no notification a client sends over HTTP, so one is accepted and
        // dropped.
        if (!request_json.contains("id")) {
            co_return make_empty_json_response(request, http::status::accepted);
        }
        if (!detail::is_request_id(request_json.at("id"))) {
            co_return make_invalid_id_response(request);
        }
        if (auto mismatch = detail::modern_header_mismatch(request, request_json)) {
            header_mismatch_rejections.fetch_add(1, std::memory_order_relaxed);
            co_return make_jsonrpc_error_response(request, http::status::bad_request, g_HEADER_MISMATCH,
                                                  "Header mismatch: " + *mismatch,
                                                  request_json.at("id"));
        }
        if (auto refusal = detail::missing_modern_version(request_json)) {
            co_return make_jsonrpc_error_response(request, http::status::bad_request, g_INVALID_PARAMS,
                                                  std::move(*refusal), request_json.at("id"));
        }
        if (request_json.at("method") == "tools/call") {
            detail::ParamHeaderCheck check;
            {
                std::lock_guard lock(configuration_mutex);
                check = param_header_check;
            }
            if (check) {
                if (auto mismatch = check(request_json, detail::mcp_param_headers(request))) {
                    header_mismatch_rejections.fetch_add(1, std::memory_order_relaxed);
                    co_return make_jsonrpc_error_response(
                        request, http::status::bad_request, g_HEADER_MISMATCH,
                        "Header mismatch: " + *mismatch, request_json.at("id"));
                }
            }
        }

        // Sessionless requests wait in the session's incoming queue, so a stranger could otherwise
        // delay the session's own requests by sending many at once.
        if (sessionless_request_ids.size() >= g_max_pending_sessionless_requests) {
            co_return make_error_response(request, http::status::service_unavailable,
                                          "Too many pending requests");
        }

        // The caller chose the id without any session credential, so it is replaced by a
        // transport-private sentinel and restored in run_write. Kept raw, it would share a key
        // space with the session: a stranger could claim an id the session then needs, and two
        // sessionless clients could not both use id 1.
        auto sentinel_json = request_json;
        sentinel_json["id"] = next_sentinel_request_id();
        const auto request_id_key = sentinel_json.at("id").dump();
        std::optional<nlohmann::json> client_request_id(std::in_place, request_json.at("id"));
        const auto timer_signal =
            co_await register_pending_request(request_id_key, std::move(client_request_id));
        if (!timer_signal.has_value()) {
            co_return make_error_response(request, http::status::internal_server_error,
                                          "Request id already pending");
        }
        // Tracked so run_write keeps this response out of the session's replay store, and a DELETE
        // of the session leaves it waiting. Dropped again in consume_pending_response and close().
        sessionless_request_ids.insert(request_id_key);
        enqueue_incoming_message(sentinel_json.dump(), principal);

        auto closed_response =
            co_await wait_for_response(request, request_id_key, *timer_signal, connection.get());
        if (closed_response) {
            if (!state->closed.load(std::memory_order_acquire)) {
                cancel_abandoned_request(sentinel_json.at("id").get<std::string>(),
                                         std::move(principal));
            }
            co_return std::move(*closed_response);
        }
        auto pending_result = co_await consume_pending_response(request_id_key);
        if (!pending_result.response_body.has_value()) {
            co_return make_error_response(request, http::status::internal_server_error,
                                          "Missing response body for request");
        }
        const auto code = detail::response_error_code(*pending_result.response_body);
        const auto status = code ? detail::http_status_for_error(*code) : http::status::ok;
        co_return make_json_response(request, status, std::move(*pending_result.response_body));
    }

    /// @p principal is what refuse_before_body() attributed the request to.
    Task<Reply> handle_post(const StringRequest& request, std::shared_ptr<Connection> connection,
                            detail::BudgetLease& bytes, std::string principal) {
        const auto request_json = detail::parse_peer_json(request.body());
        if (request_json.is_discarded() || !request_json.is_object()) {
            co_return make_error_response(request, http::status::bad_request,
                                          "Invalid JSON-RPC payload");
        }
        // Held until the answer is written: the parsed message can be far larger than its body.
        if (!bytes.grow_to(request.body().size() + detail::json_memory_cost(request_json))) {
            co_return make_error_response(request, http::status::service_unavailable, "Server busy");
        }
        if (detail::is_modern_http_request(request_json,
                                           detail::http_header(request, "MCP-Protocol-Version"))) {
            co_return co_await handle_modern_post(request, request_json, std::move(principal),
                                                  std::move(connection));
        }
        if (is_initialize_request(request_json) && !serves_legacy()) {
            co_return initialize_not_served_response(request, request_json);
        }
        if (auto error = check_protocol_version(request, request_json)) {
            co_return std::move(*error);
        }

        if (is_initialize_request(request_json) && session_left_idle(request)) {
            co_await terminate_session();
        }
        const auto session_check = co_await validate_post_session(request);
        if (!session_check.ok) {
            co_return make_error_response(request, session_check.status, session_check.error_message);
        }
        if (!session_belongs_to(principal)) {
            const auto not_found = session_not_found();
            co_return make_error_response(request, not_found.status, not_found.error_message);
        }
        if (session_id.has_value()) {
            // Only a request the session serves counts as its use.
            session_last_used = std::chrono::steady_clock::now();
        }
        if (is_initialize_request(request_json)) {
            initializing_principal = principal;
        }

        const bool has_request_id = request_json.contains("id");
        const bool has_method = request_json.contains("method");
        // An answer to a request of the server belongs to the session that was asked. Without an
        // established one, the sender of this message is nobody the server asked anything.
        if (!has_method && !session_active) {
            co_return make_error_response(request, http::status::bad_request, "No active session");
        }
        if (!has_request_id || !has_method) {
            enqueue_incoming_message(request.body(), std::move(principal));
            co_return make_empty_json_response(request, http::status::accepted);
        }
        // The Server answers a request whose id is neither a string nor an integer within int64_t
        // without an id, since it has none it may echo, so that answer could never be matched to
        // this request.
        if (!detail::is_request_id(request_json.at("id"))) {
            co_return make_invalid_id_response(request);
        }

        // Session-gated requests keep the exact bytes the peer sent.
        const auto request_id_key = request_json.at("id").dump();
        const auto timer_signal = co_await register_pending_request(request_id_key);
        if (!timer_signal.has_value()) {
            co_return make_error_response(request, http::status::bad_request,
                                          "Request id already pending");
        }
        // A client that takes SSE over HTTP/1.1 gets the request's events as they come, so a
        // request of the server's own reaches it while the handler waits for the answer.
        const auto accept_it = request.find(http::field::accept);
        if (!is_initialize_request(request_json) && request.version() >= 11 &&
            !json_only_.load(std::memory_order_acquire) && accept_it != request.end() &&
            detail::to_std_string_view(accept_it->value()).find("text/event-stream") !=
                std::string_view::npos) {
            auto& pending = pending_responses.at(request_id_key);
            pending.streams = true;
            pending.stream = std::to_string(++response_streams);
            enqueue_incoming_message(request.body(), std::move(principal));
            auto legacy = std::make_shared<LegacyStream>();
            legacy->request_key = request_id_key;
            legacy->signal = *timer_signal;
            co_return legacy;
        }
        enqueue_incoming_message(request.body(), std::move(principal));

        auto closed_response = co_await wait_for_response(request, request_id_key, *timer_signal);
        if (closed_response) {
            co_return std::move(*closed_response);
        }
        const auto pending_result = co_await consume_pending_response(request_id_key);
        if (pending_result.session_ended) {
            // Not 404: the request was handed to the server, so a client must not send it again in
            // a new session.
            co_return make_error_response(request, http::status::internal_server_error,
                                          "Session terminated");
        }
        if (!pending_result.response_body.has_value()) {
            co_return make_error_response(request, http::status::internal_server_error,
                                          "Missing response body for request");
        }

        const bool client_accepts_sse =
            accept_it != request.end() &&
            detail::to_std_string_view(accept_it->value()).find("text/event-stream") !=
                std::string_view::npos;

        if (!json_only_.load(std::memory_order_acquire) && client_accepts_sse &&
            pending_result.event_id.has_value()) {
            auto response =
                make_sse_response(request, *pending_result.event_id, *pending_result.response_body);
            if (pending_result.session_header.has_value()) {
                response.set("MCP-Session-Id", *pending_result.session_header);
            }
            co_return response;
        }

        auto response = make_json_response(request, http::status::ok, *pending_result.response_body);
        if (pending_result.session_header.has_value()) {
            response.set("MCP-Session-Id", *pending_result.session_header);
        }
        co_return response;
    }

    /// A GET or DELETE naming 2026-07-28 without a session asks for something that revision does
    /// not have: it opens no stream this way and has no session to end.
    std::optional<StringResponse> refuse_sessionless_modern_stream(const StringRequest& request) const {
        const auto protocol_header = detail::http_header(request, "MCP-Protocol-Version");
        const bool asks_for_legacy = !protocol_header || is_legacy_protocol_version(*protocol_header) ||
                                     request.find("MCP-Session-Id") != request.end();
        if (asks_for_legacy && serves_legacy()) {
            return std::nullopt;
        }
        auto response =
            make_error_response(request, http::status::method_not_allowed, "Method not allowed");
        response.set(http::field::allow, "POST");
        return response;
    }

    Task<StringResponse> handle_delete(const StringRequest& request, const std::string& principal) {
        if (auto refusal = refuse_sessionless_modern_stream(request)) {
            co_return std::move(*refusal);
        }
        const nlohmann::json request_json = {"method", "delete"};
        if (auto error = check_protocol_version(request, request_json)) {
            co_return std::move(*error);
        }

        const auto session_check = co_await validate_delete_session(request);
        if (!session_check.ok) {
            co_return make_error_response(request, session_check.status, session_check.error_message);
        }
        if (!session_belongs_to(principal)) {
            const auto not_found = session_not_found();
            co_return make_error_response(request, not_found.status, not_found.error_message);
        }

        co_await terminate_session();
        co_return make_json_response(request, http::status::ok, "{}");
    }

    /// A GET opens the session's stream for messages that belong to no request, or with
    /// Last-Event-ID resumes a stream: one still going on is served live from there, and one that
    /// ended is replayed.
    Task<Reply> handle_get(const StringRequest& request, const std::string& principal,
                           detail::BudgetLease& bytes) {
        if (auto refusal = refuse_sessionless_modern_stream(request)) {
            co_return std::move(*refusal);
        }
        const nlohmann::json request_json = {"method", "get"};
        if (auto error = check_protocol_version(request, request_json)) {
            co_return std::move(*error);
        }

        const auto session_header_it = request.find("MCP-Session-Id");
        if (session_header_it == request.end()) {
            co_return make_error_response(
                request, http::status::bad_request,
                session_id.has_value() ? "Missing MCP-Session-Id header" : "No active session");
        }
        if (!session_id.has_value() || session_header_it->value() != *session_id) {
            const auto not_found = session_not_found();
            co_return make_error_response(request, not_found.status, not_found.error_message);
        }
        if (!session_belongs_to(principal)) {
            const auto not_found = session_not_found();
            co_return make_error_response(request, not_found.status, not_found.error_message);
        }

        auto signal = std::make_shared<boost::asio::steady_timer>(strand);
        signal->expires_at(std::chrono::steady_clock::time_point::max());
        auto legacy = std::make_shared<LegacyStream>();
        legacy->signal = signal;
        legacy->open_at_once = true;

        const auto last_event_id_it = request.find("Last-Event-ID");
        if (last_event_id_it != request.end()) {
            auto last_id = std::string(last_event_id_it->value());
            auto missed_events = event_store.events_after(last_id);
            if (!missed_events.has_value()) {
                co_return make_error_response(request, http::status::gone,
                                              "Event ID has been evicted from store");
            }
            // Each replay holds its own copy of the events until the client has read it, so the
            // copies count against what requests may hold.
            std::size_t replay_bytes = 0;
            for (const auto& [id, data] : *missed_events) {
                replay_bytes += 2 * (id.size() + data.size());
            }
            if (!bytes.grow_to(replay_bytes)) {
                co_return make_error_response(request, http::status::service_unavailable,
                                              "Server busy");
            }
            // A stream still going on is taken over from the connection that served it, which may
            // not have noticed yet that its client left.
            bool live = false;
            const auto stream_name = event_store.stream_of(last_id);
            if (stream_name.has_value() && !get_stream.empty() && *stream_name == get_stream) {
                live = true;
                legacy->generation = ++get_generation;
                // The connection that served it until now wakes to see it was replaced.
                if (get_timer) {
                    get_timer->cancel();
                }
                get_timer = signal;
                get_events.clear();
            } else if (stream_name.has_value()) {
                for (auto& [key, pending] : pending_responses) {
                    if (!pending.streams || pending.stream != *stream_name || pending.response_ready) {
                        continue;
                    }
                    live = true;
                    legacy->request_key = key;
                    legacy->generation = ++pending.generation;
                    pending.detached = false;
                    pending.ready_timer->cancel();
                    pending.ready_timer = signal;
                    pending.stream_events.clear();
                    break;
                }
            }
            if (!live) {
                // An empty stream is an answer here too: the client asked to resume one.
                co_return make_sse_replay_response(request, *missed_events);
            }
            legacy->replay = std::move(*missed_events);
            co_return legacy;
        }

        if (get_timer != nullptr) {
            // Each message goes on one stream, so the session has one GET stream at a time.
            co_return make_error_response(request, http::status::conflict,
                                          "This session already has a GET stream");
        }
        if (get_stream.empty()) {
            get_stream = std::to_string(++response_streams);
        }
        legacy->generation = ++get_generation;
        get_timer = signal;
        get_events.clear();
        // An event with an id and no data lets a 2025-11-25 client resume a stream that has carried
        // nothing yet.
        if (negotiated_protocol_version >= "2025-11-25") {
            auto id = event_store.append(std::string(), get_stream);
            legacy->replay.emplace_back(std::move(id), std::string());
        }
        co_return legacy;
    }

    /// Writes a stream of the legacy session as its events come: the events of a POST and finally
    /// its response, or the GET stream until the session ends. A POST answered before anything
    /// else was sent gets one complete response, as when nothing streams. A client that goes away
    /// cancels nothing, since the specification does not make a disconnect a cancellation: the
    /// stream's events wait in the event store, where a GET with Last-Event-ID resumes it.
    ///
    /// @return Whether the connection may carry another request.
    Task<bool> serve_legacy_stream(Connection& stream, const StringRequest& request,
                                   std::shared_ptr<LegacyStream> legacy) {
        const bool get = legacy->request_key.empty();
        SseEventList events = std::move(legacy->replay);
        bool stream_open = false;
        for (;;) {
            if (state->closed.load(std::memory_order_acquire)) {
                co_return false;
            }
            bool finished = false;
            bool session_ended = false;
            std::optional<std::string> response_body;
            std::optional<std::string> session_header;
            SseEventList* waiting = nullptr;
            if (get) {
                if (get_generation != legacy->generation) {
                    // A resume serves the stream now, or the session ended.
                    if (!stream_open) {
                        co_return false;
                    }
                    finished = true;
                } else {
                    waiting = &get_events;
                }
            } else {
                const auto pending = pending_responses.find(legacy->request_key);
                if (pending == pending_responses.end()) {
                    finished = true;
                } else if (pending->second.generation != legacy->generation) {
                    co_return false;
                } else {
                    waiting = &pending->second.stream_events;
                }
            }
            if (waiting != nullptr) {
                for (auto& event : *waiting) {
                    events.push_back(std::move(event));
                }
                waiting->clear();
            }
            if (!get) {
                const auto pending = pending_responses.find(legacy->request_key);
                if (pending != pending_responses.end() && pending->second.response_ready) {
                    finished = true;
                    session_ended = pending->second.session_ended;
                    response_body = std::move(pending->second.response_body);
                    session_header = std::move(pending->second.session_header);
                    pending_responses.erase(pending);
                }
            }

            if (finished && !stream_open && !get) {
                StringResponse response;
                if (session_ended) {
                    // Not 404: the request was handed to the server, so a client must not send it
                    // again in a new session.
                    response = make_error_response(request, http::status::internal_server_error,
                                                   "Session terminated");
                } else if (!response_body.has_value()) {
                    response = make_error_response(request, http::status::internal_server_error,
                                                   "Missing response body for request");
                } else if (!events.empty()) {
                    response = make_sse_replay_response(request, events);
                } else {
                    response = make_json_response(request, http::status::ok, std::move(*response_body));
                }
                if (session_header.has_value()) {
                    response.set("MCP-Session-Id", *session_header);
                }
                boost::system::error_code error;
                co_await http::async_write(
                    stream, response, boost::asio::redirect_error(boost::asio::use_awaitable, error));
                co_return !error && response.keep_alive();
            }

            if (!stream_open && (legacy->open_at_once || !events.empty())) {
                http::response<http::empty_body> header{http::status::ok, request.version()};
                set_common_headers(header, request.keep_alive());
                header.set(http::field::content_type, "text/event-stream");
                header.set(http::field::cache_control, "no-cache");
                header.set("X-Accel-Buffering", "no");
                if (session_id.has_value()) {
                    header.set("MCP-Session-Id", *session_id);
                }
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
                    text += "id: ";
                    text += id;
                    text += "\ndata: ";
                    text += data;
                    text += "\n\n";
                }
                events.clear();
                boost::system::error_code error;
                co_await boost::asio::async_write(
                    stream, http::make_chunk(boost::asio::buffer(text)),
                    boost::asio::redirect_error(boost::asio::use_awaitable, error));
                if (error) {
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
                legacy->signal->expires_after(g_keepalive_interval);
            } else {
                legacy->signal->expires_at(std::chrono::steady_clock::time_point::max());
            }
            boost::system::error_code wait_error;
            co_await legacy->signal->async_wait(
                boost::asio::redirect_error(boost::asio::use_awaitable, wait_error));
            if (!wait_error && stream_open) {
                static constexpr std::string_view g_keep_alive = ": keep-alive\n\n";
                boost::system::error_code error;
                co_await boost::asio::async_write(
                    stream, http::make_chunk(boost::asio::buffer(g_keep_alive)),
                    boost::asio::redirect_error(boost::asio::use_awaitable, error));
                if (error) {
                    detach_stream(*legacy);
                    co_return false;
                }
            }
        }
    }

    /// Origin and bearer checks for the MCP endpoint: they need only the headers, so a request they
    /// refuse never has its body read. Sets @p principal to what the request is attributed to. The
    /// metadata document and exempt paths need neither check.
    Task<std::optional<StringResponse>> refuse_before_body(const StringRequest& request,
                                                           std::string& principal) const {
        // A closed transport answers nothing more, so a request is not worth a token check.
        if (state->closed.load(std::memory_order_acquire)) {
            co_return std::nullopt;
        }
        const auto method = request.method();
        if (method != http::verb::post && method != http::verb::get && method != http::verb::delete_) {
            co_return std::nullopt;
        }
        if (is_unauthenticated_path(request) || serve_protected_resource_metadata(request)) {
            co_return std::nullopt;
        }
        if (auto error = check_origin(request)) {
            co_return error;
        }
        auto authorization = check_authorization(request);
        if (!authorization.refusal && async_bearer_token_validator) {
            authorization = co_await check_authorization_async(request);
        }
        principal = std::move(authorization.principal);
        co_return std::move(authorization.refusal);
    }

    Task<Reply> handle_request(const StringRequest& request, std::shared_ptr<Connection> connection,
                               detail::BudgetLease& bytes, std::string principal) {
        if (state->closed.load(std::memory_order_acquire)) {
            co_return make_error_response(request, http::status::service_unavailable,
                                          "Transport closed");
        }
        if (auto document = serve_protected_resource_metadata(request)) {
            co_return std::move(*document);
        }
        if (is_unauthenticated_path(request)) {
            // An exempt path is excluded from MCP dispatch, not merely excused from the bearer
            // check. Serving MCP here would answer it with no authentication at all, so a path the
            // metadata route did not claim has nothing left to answer it.
            co_return make_error_response(request, http::status::not_found, "Not found");
        }
        if (request.method() == http::verb::post) {
            co_return co_await handle_post(request, std::move(connection), bytes, std::move(principal));
        }
        if (request.method() == http::verb::get) {
            co_return co_await handle_get(request, principal, bytes);
        }
        if (request.method() == http::verb::delete_) {
            co_return co_await handle_delete(request, principal);
        }

        auto response =
            make_error_response(request, http::status::method_not_allowed, "Method not allowed");
        response.set(http::field::allow, "GET, POST, DELETE");
        co_return response;
    }

    Task<void> handle_connection(const std::shared_ptr<Connection>& connection) {
        auto& stream = *connection;
        beast::flat_buffer request_buffer;
        // What the request being served is attributed to.
        std::string principal;
        const detail::RefuseBeforeBody refuse =
            [this, &principal](const StringRequest& head) -> Task<std::optional<StringResponse>> {
            co_return co_await refuse_before_body(head, principal);
        };

        for (;;) {
            principal.clear();
            detail::RequestParser parser;
            parser.body_limit(max_request_body_bytes);
            detail::BudgetLease bytes(budget.bytes, connection_limits.max_request_bytes);
            if (!co_await detail::read_request_within_limits(stream, request_buffer, parser,
                                                             connection_limits, max_request_body_bytes,
                                                             bytes, refuse)) {
                break;
            }
            StringRequest request = detail::take_request(parser);

            if (state->closed.load(std::memory_order_acquire)) {
                break;
            }

            detail::BudgetLease slot(budget.requests, connection_limits.max_requests);
            Reply reply;
            if (slot.grow_to(1)) {
                reply = co_await handle_request(request, connection, bytes, std::move(principal));
            } else {
                reply = make_error_response(request, http::status::service_unavailable,
                                            "Too many requests");
            }
            if (state->closed.load(std::memory_order_acquire)) {
                break;
            }
            if (auto* legacy = std::get_if<1>(&reply)) {
                if (!co_await serve_legacy_stream(stream, request, *legacy)) {
                    break;
                }
                continue;
            }
            auto& response = std::get<0>(reply);
            const bool keep_connection_alive = response.keep_alive();
            if (!co_await detail::write_response_within_limits(stream, response, connection_limits) ||
                !keep_connection_alive) {
                break;
            }
        }

        boost::system::error_code shutdown_error;
        (void)stream.socket().shutdown(boost::asio::ip::tcp::socket::shutdown_send, shutdown_error);
    }

    static Task<IncomingMessage> run_read(std::shared_ptr<Impl> impl) {
        auto& state = *impl->state;
        if (state.read_active) {
            throw std::logic_error("HttpServerTransport supports one pending read");
        }
        state.read_active = true;

        try {
            for (;;) {
                if (!state.queue.empty()) {
                    auto incoming = std::move(state.queue.front());
                    state.queue.pop();
                    state.read_active = false;
                    co_return incoming;
                }

                if (state.closed.load(std::memory_order_acquire)) {
                    throw std::runtime_error("HttpServerTransport is closed");
                }

                state.timer.expires_at(std::chrono::steady_clock::time_point::max());
                try {
                    co_await state.timer.async_wait(boost::asio::use_awaitable);
                } catch (const boost::system::system_error& error) {
                    if (error.code() != boost::asio::error::operation_aborted) {
                        throw;
                    }
                }
            }
        } catch (...) {
            state.read_active = false;
            throw;
        }
    }

    static Task<void> run_write(std::shared_ptr<Impl> impl, std::string message,
                                std::shared_ptr<const std::string> related_request = nullptr) {
        if (impl->state->closed.load(std::memory_order_acquire)) {
            throw std::runtime_error("HttpServerTransport is closed");
        }

        const auto response_json = nlohmann::json::parse(message, nullptr, false);
        if (response_json.is_discarded() || !response_json.is_object()) {
            co_return;
        }

        // A request the Server sends to the client also carries an id, and it may equal the id of
        // a client request still waiting here; only a response answers that request.
        std::optional<std::string> response_id_key;
        if (response_json.contains("id") && !response_json.contains("method")) {
            response_id_key = response_json.at("id").dump();
        }

        // Responses to sessionless pre-gate requests must never reach the replay store. That
        // store is a bounded ring shared with the established session, so appending them would
        // evict the session's own replay history and turn its next Last-Event-ID resume into a
        // 410 Gone. Sessionless requests are unauthenticated by construction, so their traffic
        // must not be able to consume replay capacity that belongs to a real session.
        const bool skip_event_store =
            response_id_key.has_value() && (impl->sessionless_request_ids.contains(*response_id_key) ||
                                            impl->is_sentinel_key(*response_id_key));

        if (!response_id_key.has_value()) {
            impl->route_event(std::move(message), related_request.get());
            co_return;
        }

        const auto& request_id_key = *response_id_key;
        const auto pending_it = impl->pending_responses.find(request_id_key);

        // Between a DELETE and the next client's initialize result nobody holds a session, and
        // anything recorded then would become part of the next client's replay history. A
        // response ends the stream of its POST; one that did not stream is stored as a stream of
        // its own, so a resume never replays another.
        std::optional<std::string> event_id;
        if (!impl->json_only_.load(std::memory_order_acquire) && !skip_event_store &&
            (!impl->session_ended_awaiting_next || is_initialize_result_response(response_json))) {
            const bool streamed =
                pending_it != impl->pending_responses.end() && pending_it->second.streams;
            event_id =
                impl->event_store.append(message, streamed ? pending_it->second.stream
                                                           : std::to_string(++impl->response_streams));
        }

        if (pending_it == impl->pending_responses.end()) {
            co_return;
        }
        if (pending_it->second.streams) {
            if (pending_it->second.detached) {
                // Nobody waits for it: a resume finds the response in the event store.
                impl->pending_responses.erase(pending_it);
                co_return;
            }
            if (event_id.has_value()) {
                pending_it->second.stream_events.emplace_back(*event_id, message);
            }
        }

        // The request was carried under a sentinel id, so the server answered the sentinel. Put
        // the caller's own id back before the response leaves the transport; the peer must see
        // the id it sent, and must never see the sentinel.
        if (pending_it->second.client_request_id.has_value()) {
            auto restored_response = response_json;
            restored_response["id"] = *pending_it->second.client_request_id;
            message = restored_response.dump();
        }

        pending_it->second.response_body = std::move(message);
        pending_it->second.event_id = std::move(event_id);
        pending_it->second.response_ready = true;

        if (is_initialize_result_response(response_json)) {
            const auto& result = response_json.at("result");
            if (!impl->session_id.has_value()) {
                impl->session_id = generate_session_id();
                impl->session_principal = impl->initializing_principal;
            }
            if (result.contains("protocolVersion") && result.at("protocolVersion").is_string()) {
                impl->negotiated_protocol_version = result.at("protocolVersion").get<std::string>();
            }
            pending_it->second.session_header = impl->session_id;
            impl->session_active = true;
            impl->session_last_used = std::chrono::steady_clock::now();
            impl->session_ended_awaiting_next = false;
        }

        pending_it->second.ready_timer->cancel();
    }

    std::string host;
    unsigned short port;
    boost::asio::strand<boost::asio::any_io_executor> strand;
    boost::asio::ip::tcp::acceptor acceptor;
    std::shared_ptr<SharedState> state;
    std::unordered_set<std::shared_ptr<Connection>> active_connections;

    mutable std::mutex configuration_mutex;
    bool listening_started{false};
    /// Guarded by configuration_mutex: Server::run() installs it while the transport may be
    /// listening already.
    detail::ParamHeaderCheck param_header_check;
    /// The versions named when initialize arrives with a 2026-07-28 header; every version until
    /// Server::run() says which it serves.
    std::vector<std::string> served_versions{g_SUPPORTED_PROTOCOL_VERSIONS.begin(),
                                             g_SUPPORTED_PROTOCOL_VERSIONS.end()};
    std::atomic<std::uint64_t> header_mismatch_rejections{0};
    /// Names the stream of each stored response; see run_write.
    std::uint64_t response_streams{0};
    /// The stream a GET opened, for messages that belong to no request: its name in the event
    /// store (empty while there is none), the timer that wakes the connection serving it (null
    /// while none does), the events that connection has not taken yet, and which connection
    /// serves it.
    std::string get_stream;
    std::shared_ptr<boost::asio::steady_timer> get_timer;
    SseEventList get_events;
    std::uint64_t get_generation{0};

    std::unordered_map<std::string, PendingResponse> pending_responses;
    std::chrono::steady_clock::time_point session_last_used;
    /// Guarded by configuration_mutex.
    std::chrono::milliseconds session_idle_timeout{std::chrono::minutes(5)};
    std::unordered_set<std::string> sessionless_request_ids;
    std::string sentinel_id_prefix;
    std::uint64_t sentinel_id_counter{0};
    std::optional<std::string> session_id;
    std::string negotiated_protocol_version{std::string(g_LATEST_LEGACY_PROTOCOL_VERSION)};
    bool session_active{false};
    std::string session_principal;
    std::string initializing_principal;
    /// Set by a DELETE that ended a session, cleared by the next initialize result.
    bool session_ended_awaiting_next{false};
    std::atomic<std::uint64_t> terminated_sessions{0};

    bool allow_all_origins{false};
    std::unordered_set<std::string> allowed_origins;
    BearerTokenValidator bearer_token_validator;
    AsyncBearerTokenValidator async_bearer_token_validator;
    std::size_t max_request_body_bytes{constants::g_default_max_request_body_bytes};
    HttpConnectionLimits connection_limits;
    detail::HttpListenerBudget budget;
    BearerChallengeConfig bearer_challenge;
    /// Rendered once when the challenge or the metadata changes, so serving a 401 never formats.
    std::string www_authenticate_value{"Bearer"};
    std::optional<ProtectedResourceMetadataConfig> protected_resource_metadata;
    std::string protected_resource_metadata_body;
    /// The resolved serving path: `path` when set, else derived from `resource`.
    std::string protected_resource_metadata_path;
    std::unordered_set<std::string> unauthenticated_paths;

    EventStore event_store;
    std::atomic<bool> json_only_{false};
};

HttpServerTransport::HttpServerTransport(const boost::asio::any_io_executor& executor, std::string host,
                                         unsigned short port, std::size_t event_store_capacity)
    : impl_(std::make_shared<Impl>(executor, std::move(host), port, event_store_capacity)) {}

HttpServerTransport::~HttpServerTransport() {
    try {
        close();
    } catch (...) {
        // Ignore exceptions in destructor
        (void)0;
    }
}

const EventStore& HttpServerTransport::event_store() const { return impl_->event_store; }

unsigned short HttpServerTransport::port() const {
    boost::system::error_code ec;
    const auto endpoint = impl_->acceptor.local_endpoint(ec);
    if (ec) {
        throw std::runtime_error("Failed to query HTTP acceptor endpoint: " + ec.message());
    }
    return endpoint.port();
}

std::uint64_t HttpServerTransport::header_mismatch_rejections() const {
    return impl_->header_mismatch_rejections.load(std::memory_order_relaxed);
}

void HttpServerTransport::set_json_only(bool json_only) {
    impl_->json_only_.store(json_only, std::memory_order_release);
}

void HttpServerTransport::set_allowed_origins(std::vector<std::string> origins) {
    std::lock_guard lock(impl_->configuration_mutex);
    impl_->ensure_configurable();
    impl_->allowed_origins.clear();
    impl_->allowed_origins.reserve(origins.size());
    for (auto& origin : origins) {
        impl_->allowed_origins.insert(std::move(origin));
    }
    impl_->allow_all_origins = false;
}

void HttpServerTransport::set_allow_all_origins(bool allow_all) {
    std::lock_guard lock(impl_->configuration_mutex);
    impl_->ensure_configurable();
    impl_->allow_all_origins = allow_all;
}

void HttpServerTransport::set_bearer_token_validator(BearerTokenValidator validator) {
    std::lock_guard lock(impl_->configuration_mutex);
    impl_->ensure_configurable();
    if (validator && impl_->async_bearer_token_validator) {
        throw std::logic_error("HttpServerTransport accepts one bearer token validator");
    }
    impl_->bearer_token_validator = std::move(validator);
}

void HttpServerTransport::set_async_bearer_token_validator(AsyncBearerTokenValidator validator) {
    std::lock_guard lock(impl_->configuration_mutex);
    impl_->ensure_configurable();
    if (validator && impl_->bearer_token_validator) {
        throw std::logic_error("HttpServerTransport accepts one bearer token validator");
    }
    impl_->async_bearer_token_validator = std::move(validator);
}

void HttpServerTransport::set_session_idle_timeout(std::chrono::milliseconds timeout) {
    std::lock_guard lock(impl_->configuration_mutex);
    impl_->session_idle_timeout = timeout;
}

void HttpServerTransport::set_connection_limits(const HttpConnectionLimits& limits) {
    std::lock_guard lock(impl_->configuration_mutex);
    impl_->ensure_configurable();
    if (limits.max_requests == 0 || limits.max_request_bytes == 0) {
        throw std::invalid_argument("HTTP connection limits must allow a request");
    }
    impl_->connection_limits = limits;
}

void HttpServerTransport::set_max_request_body_bytes(std::size_t max_bytes) {
    std::lock_guard lock(impl_->configuration_mutex);
    impl_->ensure_configurable();
    if (max_bytes == 0) {
        throw std::invalid_argument("Maximum request body size must be greater than zero");
    }
    impl_->max_request_body_bytes = max_bytes;
}

void HttpServerTransport::set_bearer_challenge(BearerChallengeConfig challenge) {
    std::lock_guard lock(impl_->configuration_mutex);
    impl_->ensure_configurable();
    auto rendered = impl_->render_challenge(challenge);
    impl_->bearer_challenge = std::move(challenge);
    impl_->www_authenticate_value = std::move(rendered);
}

void HttpServerTransport::set_protected_resource_metadata(ProtectedResourceMetadataConfig metadata) {
    std::lock_guard lock(impl_->configuration_mutex);
    impl_->ensure_configurable();
    if (metadata.resource.empty()) {
        throw std::invalid_argument("Protected-resource metadata requires a resource URL");
    }

    auto metadata_url = protected_resource_metadata_url(metadata);
    auto document = format_protected_resource_metadata(metadata);
    auto challenge = impl_->bearer_challenge;
    if (challenge.resource_metadata.empty()) {
        challenge.resource_metadata = std::move(metadata_url);
    }
    auto rendered = format_www_authenticate(challenge);

    auto document_path = protected_resource_metadata_path(metadata);

    impl_->protected_resource_metadata = std::move(metadata);
    impl_->protected_resource_metadata_path = std::move(document_path);
    impl_->protected_resource_metadata_body = std::move(document);
    impl_->www_authenticate_value = std::move(rendered);
}

void HttpServerTransport::set_unauthenticated_paths(std::vector<std::string> paths) {
    std::lock_guard lock(impl_->configuration_mutex);
    impl_->ensure_configurable();
    impl_->unauthenticated_paths.clear();
    impl_->unauthenticated_paths.reserve(paths.size());
    for (auto& path : paths) {
        impl_->unauthenticated_paths.insert(std::move(path));
    }
}

namespace {

Task<std::string> message_of(Task<IncomingMessage> incoming) {
    auto message = co_await std::move(incoming);
    co_return std::move(message.message);
}

}  // namespace

// Not a coroutine: the read owns the implementation, so it may outlive this transport.
Task<std::string> HttpServerTransport::read_message() { return message_of(read_incoming()); }

Task<IncomingMessage> HttpServerTransport::read_incoming() {
    auto impl = impl_;
    return boost::asio::co_spawn(impl->strand, Impl::run_read(impl), boost::asio::use_awaitable);
}

Task<void> HttpServerTransport::write_message(std::string_view message) {
    auto impl = impl_;
    return boost::asio::co_spawn(impl->strand, Impl::run_write(impl, std::string(message)),
                                 boost::asio::use_awaitable);
}

Task<void> HttpServerTransport::write_related_message(std::string_view message,
                                                      std::string_view related_request) {
    auto impl = impl_;
    return boost::asio::co_spawn(impl->strand,
                                 Impl::run_write(impl, std::string(message),
                                                 std::make_shared<const std::string>(related_request)),
                                 boost::asio::use_awaitable);
}

void HttpServerTransport::close() {
    auto impl = impl_;
    {
        std::lock_guard lock(impl->configuration_mutex);
        if (impl->state->closed.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
    }

    boost::asio::post(impl->strand, [impl]() {
        boost::system::error_code ec;
        (void)impl->acceptor.cancel(ec);
        (void)impl->acceptor.close(ec);
        impl->close_active_connections();

        for (auto& pending_entry : impl->pending_responses) {
            pending_entry.second.ready_timer->cancel();
        }
        impl->pending_responses.clear();
        impl->end_get_stream();
        impl->sessionless_request_ids.clear();

        impl->session_id.reset();
        impl->negotiated_protocol_version = std::string(g_LATEST_LEGACY_PROTOCOL_VERSION);
        impl->session_active = false;
    });

    boost::asio::post(impl->state->timer.get_executor(),
                      [state = impl->state]() { state->timer.cancel(); });
}

Task<void> HttpServerTransport::listen() {
    auto impl = impl_;
    (void)impl->begin_listening();
    return boost::asio::co_spawn(impl->strand, listen_impl(impl), boost::asio::use_awaitable);
}

Task<void> HttpServerTransport::listen_impl(std::shared_ptr<Impl> impl) {
    for (;;) {
        if (impl->state->closed.load(std::memory_order_acquire)) {
            co_return;
        }

        boost::asio::ip::tcp::socket socket(impl->strand);
        bool retry = false;
        try {
            socket = co_await impl->acceptor.async_accept(boost::asio::use_awaitable);
        } catch (const boost::system::system_error& err) {
            if (impl->state->closed.load(std::memory_order_acquire) ||
                err.code() == boost::asio::error::operation_aborted) {
                co_return;
            }
            if (!detail::accept_failure_can_pass(err.code())) {
                throw;
            }
            retry = true;
        }
        if (retry) {
            boost::asio::steady_timer pause(impl->strand, detail::g_accept_retry_delay);
            boost::system::error_code ignored;
            co_await pause.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, ignored));
            continue;
        }

        if (impl->state->closed.load(std::memory_order_acquire)) {
            boost::system::error_code ignored;
            (void)socket.close(ignored);
            co_return;
        }

        if (impl->active_connections.size() >=
            detail::resolved_max_connections(impl->connection_limits.max_connections)) {
            // Closed at once, so the descriptor is free again for a connection that can be served.
            boost::system::error_code ignored;
            (void)socket.close(ignored);
            continue;
        }
        auto connection = std::make_shared<Impl::Connection>(std::move(socket));
        impl->active_connections.insert(connection);

        boost::asio::co_spawn(
            impl->strand,
            [impl, connection]() -> Task<void> {
                try {
                    co_await impl->handle_connection(connection);
                } catch (...) {
                    impl->active_connections.erase(connection);
                    throw;
                }
                impl->active_connections.erase(connection);
            },
            [](const std::exception_ptr&) {
                // Connection errors (EOF, client disconnect) are normal;
                // handled per-connection, not propagated to the accept loop.
            });
    }
}

namespace detail {

std::uint64_t terminated_session_count(const HttpServerTransport& transport) noexcept {
    return transport.impl_->terminated_sessions.load(std::memory_order_acquire);
}

void bind_server(HttpServerTransport& transport, HttpServerBinding binding) {
    std::lock_guard lock(transport.impl_->configuration_mutex);
    transport.impl_->param_header_check = std::move(binding.param_header_check);
    transport.impl_->served_versions = std::move(binding.served_versions);
}

}  // namespace detail

}  // namespace mcp

#include <mcp/detail/secure_random.hpp>
#include <mcp/transport/http_server.hpp>
#include <mcp/transport/http_types.hpp>

#include <atomic>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <chrono>
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
#include <vector>

namespace mcp {

namespace beast = boost::beast;
namespace http = boost::beast::http;

struct HttpServerTransport::Impl {
    using Connection = beast::tcp_stream;

    struct SharedState {
        explicit SharedState(boost::asio::strand<boost::asio::any_io_executor>& execution_strand)
            : timer(execution_strand) {}

        boost::asio::steady_timer timer;
        std::queue<std::string> queue;
        std::atomic<bool> closed{false};
        bool read_active{false};
    };

    struct PendingResponse {
        std::shared_ptr<boost::asio::steady_timer> ready_timer;
        std::optional<std::string> response_body;
        std::optional<std::string> session_header;
        std::optional<std::string> event_id;
        bool response_ready{false};
    };

    struct PendingResult {
        std::optional<std::string> response_body;
        std::optional<std::string> session_header;
        std::optional<std::string> event_id;
    };

    struct SessionCheckResult {
        bool ok{false};
        std::string error_message;
    };

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

    bool is_origin_allowed(std::string_view origin_value) const {
        if (allow_all_origins) {
            return true;
        }
        return allowed_origins.contains(std::string(origin_value));
    }

    void enqueue_incoming_message(std::string message_payload) const {
        state->queue.push(std::move(message_payload));
        state->timer.cancel();
    }

    static void set_common_headers(StringResponse& response, bool keep_alive) {
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

    Task<SessionCheckResult> validate_post_session(const StringRequest& request) {
        const auto session_header_it = request.find("MCP-Session-Id");
        const bool session_header_present = session_header_it != request.end();

        if (session_id.has_value()) {
            if (!session_header_present) {
                co_return SessionCheckResult{false, "Session active"};
            }
            if (session_header_it->value() != *session_id) {
                co_return SessionCheckResult{false, "Invalid MCP-Session-Id header"};
            }
            co_return SessionCheckResult{true, {}};
        }

        if (session_active && !session_header_present) {
            co_return SessionCheckResult{false, "Session active"};
        }

        if (session_header_present) {
            co_return SessionCheckResult{false, "Session not established"};
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
            co_return SessionCheckResult{false, "Invalid MCP-Session-Id header"};
        }

        co_return SessionCheckResult{true, {}};
    }

    Task<std::optional<std::shared_ptr<boost::asio::steady_timer>>> register_pending_request(
        const std::string& request_id_key) {
        if (pending_responses.contains(request_id_key)) {
            co_return std::nullopt;
        }

        auto timer_signal = std::make_shared<boost::asio::steady_timer>(strand);
        timer_signal->expires_at(std::chrono::steady_clock::time_point::max());

        pending_responses.emplace(request_id_key, PendingResponse{timer_signal, std::nullopt,
                                                                  std::nullopt, std::nullopt, false});
        co_return timer_signal;
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

        PendingResult pending_result{std::move(pending_it->second.response_body),
                                     std::move(pending_it->second.session_header),
                                     std::move(pending_it->second.event_id)};
        pending_responses.erase(pending_it);
        co_return pending_result;
    }

    Task<void> terminate_session() {
        session_id.reset();
        session_active = false;
        co_return;
    }

    std::optional<StringResponse> check_protocol_version(const StringRequest& request,
                                                         const nlohmann::json& request_json) const {
        const auto protocol_header_it = request.find("MCP-Protocol-Version");
        const bool is_initialize = is_initialize_request(request_json);
        if (is_initialize) {
            if (protocol_header_it == request.end()) {
                return std::nullopt;
            }
            if (is_supported_protocol_version(header_value(protocol_header_it))) {
                return std::nullopt;
            }
            return make_error_response(request, http::status::bad_request,
                                       "Invalid MCP-Protocol-Version header");
        }

        if (protocol_header_it == request.end() ||
            header_value(protocol_header_it) == negotiated_protocol_version) {
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
        if (origin_header_it != request.end() && !is_origin_allowed(origin_header_it->value())) {
            return make_error_response(request, http::status::forbidden, "Origin not allowed");
        }
        return std::nullopt;
    }

    std::optional<StringResponse> check_authorization(const StringRequest& request) const {
        if (!bearer_token_validator) {
            return std::nullopt;
        }

        const auto authorization_it = request.find(http::field::authorization);
        const auto token = authorization_it == request.end()
                               ? std::string_view{}
                               : http_bearer_token(header_value(authorization_it));
        if (token.empty() || !bearer_token_validator(token)) {
            auto response =
                make_error_response(request, http::status::unauthorized, "Invalid bearer token");
            response.set(http::field::www_authenticate, "Bearer");
            return response;
        }
        return std::nullopt;
    }

    Task<StringResponse> handle_post(const StringRequest& request) {
        const auto request_json = nlohmann::json::parse(request.body(), nullptr, false);
        if (request_json.is_discarded() || !request_json.is_object()) {
            co_return make_error_response(request, http::status::bad_request,
                                          "Invalid JSON-RPC payload");
        }

        if (auto error = check_protocol_version(request, request_json)) {
            co_return std::move(*error);
        }
        if (auto error = check_origin(request)) {
            co_return std::move(*error);
        }
        if (auto error = check_authorization(request)) {
            co_return std::move(*error);
        }

        const auto session_check = co_await validate_post_session(request);
        if (!session_check.ok) {
            co_return make_error_response(request, http::status::bad_request,
                                          session_check.error_message);
        }

        const bool has_request_id = request_json.contains("id");
        const bool has_method = request_json.contains("method");
        if (!has_request_id || !has_method) {
            enqueue_incoming_message(request.body());
            co_return make_empty_json_response(request, http::status::accepted);
        }

        const auto request_id_key = request_json.at("id").dump();
        const auto timer_signal = co_await register_pending_request(request_id_key);
        if (!timer_signal.has_value()) {
            co_return make_error_response(request, http::status::bad_request,
                                          "Request id already pending");
        }

        enqueue_incoming_message(request.body());

        for (;;) {
            if (state->closed.load(std::memory_order_acquire)) {
                co_await consume_pending_response(request_id_key);
                co_return make_error_response(request, http::status::internal_server_error,
                                              "Transport closed while waiting response");
            }

            if (co_await is_response_ready(request_id_key)) {
                break;
            }

            try {
                co_await (*timer_signal)->async_wait(boost::asio::use_awaitable);
            } catch (const boost::system::system_error& err) {
                if (err.code() != boost::asio::error::operation_aborted) {
                    throw;
                }
            }
        }

        const auto pending_result = co_await consume_pending_response(request_id_key);
        if (!pending_result.response_body.has_value()) {
            co_return make_error_response(request, http::status::internal_server_error,
                                          "Missing response body for request");
        }

        const auto accept_it = request.find(http::field::accept);
        const bool client_accepts_sse =
            accept_it != request.end() &&
            std::string_view(accept_it->value()).find("text/event-stream") != std::string_view::npos;

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

    Task<StringResponse> handle_delete(const StringRequest& request) {
        const nlohmann::json request_json = {"method", "delete"};
        if (auto error = check_protocol_version(request, request_json)) {
            co_return std::move(*error);
        }
        if (auto error = check_origin(request)) {
            co_return std::move(*error);
        }
        if (auto error = check_authorization(request)) {
            co_return std::move(*error);
        }

        const auto session_check = co_await validate_delete_session(request);
        if (!session_check.ok) {
            co_return make_error_response(request, http::status::bad_request,
                                          session_check.error_message);
        }

        co_await terminate_session();
        co_return make_json_response(request, http::status::ok, "{}");
    }

    Task<StringResponse> handle_get(const StringRequest& request) {
        const nlohmann::json request_json = {"method", "get"};
        if (auto error = check_protocol_version(request, request_json)) {
            co_return std::move(*error);
        }
        if (auto error = check_origin(request)) {
            co_return std::move(*error);
        }
        if (auto error = check_authorization(request)) {
            co_return std::move(*error);
        }

        if (!session_id.has_value()) {
            co_return make_error_response(request, http::status::bad_request, "No active session");
        }

        const auto session_header_it = request.find("MCP-Session-Id");
        if (session_header_it == request.end() || session_header_it->value() != *session_id) {
            co_return make_error_response(request, http::status::bad_request,
                                          "Invalid MCP-Session-Id header");
        }

        const auto last_event_id_it = request.find("Last-Event-ID");
        if (last_event_id_it != request.end()) {
            auto last_id = std::string(last_event_id_it->value());
            auto missed_events = event_store.events_after(last_id);
            if (!missed_events.has_value()) {
                co_return make_error_response(request, http::status::gone,
                                              "Event ID has been evicted from store");
            }
            if (!missed_events->empty()) {
                co_return make_sse_replay_response(request, *missed_events);
            }
        }

        co_return make_empty_json_response(request, http::status::ok);
    }

    Task<StringResponse> handle_request(const StringRequest& request) {
        if (state->closed.load(std::memory_order_acquire)) {
            co_return make_error_response(request, http::status::service_unavailable,
                                          "Transport closed");
        }
        if (request.method() == http::verb::post) {
            co_return co_await handle_post(request);
        }
        if (request.method() == http::verb::get) {
            co_return co_await handle_get(request);
        }
        if (request.method() == http::verb::delete_) {
            co_return co_await handle_delete(request);
        }

        auto response =
            make_error_response(request, http::status::method_not_allowed, "Method not allowed");
        response.set(http::field::allow, "GET, POST, DELETE");
        co_return response;
    }

    Task<void> handle_connection(const std::shared_ptr<Connection>& connection) {
        auto& stream = *connection;
        beast::flat_buffer request_buffer;

        for (;;) {
            StringRequest request;
            try {
                co_await http::async_read(stream, request_buffer, request, boost::asio::use_awaitable);
            } catch (const boost::system::system_error& err) {
                if (err.code() == boost::asio::error::eof ||
                    err.code() == boost::asio::error::connection_reset ||
                    err.code() == boost::asio::error::operation_aborted) {
                    break;
                }
                throw;
            }

            if (state->closed.load(std::memory_order_acquire)) {
                break;
            }

            auto response = co_await handle_request(request);
            if (state->closed.load(std::memory_order_acquire)) {
                break;
            }
            const bool keep_connection_alive = response.keep_alive();
            co_await http::async_write(stream, response, boost::asio::use_awaitable);

            if (!keep_connection_alive) {
                break;
            }
        }

        boost::system::error_code shutdown_error;
        (void)stream.socket().shutdown(boost::asio::ip::tcp::socket::shutdown_send, shutdown_error);
    }

    static Task<std::string> run_read(std::shared_ptr<Impl> impl) {
        auto& state = *impl->state;
        if (state.read_active) {
            throw std::logic_error("HttpServerTransport supports one pending read");
        }
        state.read_active = true;

        try {
            for (;;) {
                if (state.closed.load(std::memory_order_acquire)) {
                    throw std::runtime_error("HttpServerTransport is closed");
                }

                if (!state.queue.empty()) {
                    auto message_payload = std::move(state.queue.front());
                    state.queue.pop();
                    state.read_active = false;
                    co_return message_payload;
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

    static Task<void> run_write(std::shared_ptr<Impl> impl, std::string message) {
        if (impl->state->closed.load(std::memory_order_acquire)) {
            throw std::runtime_error("HttpServerTransport is closed");
        }

        const auto response_json = nlohmann::json::parse(message, nullptr, false);
        if (response_json.is_discarded() || !response_json.is_object()) {
            co_return;
        }

        std::optional<std::string> event_id;
        if (!impl->json_only_.load(std::memory_order_acquire)) {
            event_id = impl->event_store.append(message);
        }

        if (!response_json.contains("id")) {
            co_return;
        }

        const auto request_id_key = response_json.at("id").dump();
        const auto pending_it = impl->pending_responses.find(request_id_key);
        if (pending_it == impl->pending_responses.end()) {
            co_return;
        }

        pending_it->second.response_body = std::move(message);
        pending_it->second.event_id = std::move(event_id);
        pending_it->second.response_ready = true;

        if (is_initialize_result_response(response_json)) {
            const auto& result = response_json.at("result");
            if (!impl->session_id.has_value()) {
                impl->session_id = generate_session_id();
            }
            if (result.contains("protocolVersion") && result.at("protocolVersion").is_string()) {
                impl->negotiated_protocol_version = result.at("protocolVersion").get<std::string>();
            }
            pending_it->second.session_header = impl->session_id;
            impl->session_active = true;
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

    std::unordered_map<std::string, PendingResponse> pending_responses;
    std::optional<std::string> session_id;
    std::string negotiated_protocol_version{std::string(g_LATEST_PROTOCOL_VERSION)};
    bool session_active{false};

    bool allow_all_origins{false};
    std::unordered_set<std::string> allowed_origins;
    BearerTokenValidator bearer_token_validator;

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
    impl_->bearer_token_validator = std::move(validator);
}

Task<std::string> HttpServerTransport::read_message() {
    auto impl = impl_;
    return boost::asio::co_spawn(impl->strand, Impl::run_read(impl), boost::asio::use_awaitable);
}

Task<void> HttpServerTransport::write_message(std::string_view message) {
    auto impl = impl_;
    return boost::asio::co_spawn(impl->strand, Impl::run_write(impl, std::string(message)),
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

        impl->session_id.reset();
        impl->negotiated_protocol_version = std::string(g_LATEST_PROTOCOL_VERSION);
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
        try {
            socket = co_await impl->acceptor.async_accept(boost::asio::use_awaitable);
        } catch (const boost::system::system_error& err) {
            if (impl->state->closed.load(std::memory_order_acquire) ||
                err.code() == boost::asio::error::operation_aborted) {
                co_return;
            }
            throw;
        }

        if (impl->state->closed.load(std::memory_order_acquire)) {
            boost::system::error_code ignored;
            (void)socket.close(ignored);
            co_return;
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

}  // namespace mcp

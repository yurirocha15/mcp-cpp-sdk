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
#include <nlohmann/json.hpp>
#include <optional>
#include <queue>
#include <random>
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
    struct SharedState {
        explicit SharedState(boost::asio::strand<boost::asio::any_io_executor>& execution_strand)
            : timer(execution_strand) {}

        boost::asio::steady_timer timer;
        std::queue<std::string> queue;
        std::atomic<bool> closed{false};
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
        return std::string_view(header_it->value().data(), header_it->value().size());
    }

    static std::string generate_session_id() {
        std::random_device random_device;
        std::mt19937 generator(random_device());
        std::uniform_int_distribution<std::size_t> distribution(0, constants::g_hex_digits.size() - 1);

        std::string session_identifier;
        session_identifier.reserve(constants::g_session_id_length);
        for (std::size_t i = 0; i < constants::g_session_id_length; i++) {
            session_identifier.push_back(constants::g_hex_digits[distribution(generator)]);
        }
        return session_identifier;
    }

    bool is_origin_allowed(std::string_view origin_value) const {
        if (allow_all_origins) {
            return true;
        }
        return allowed_origins.contains(std::string(origin_value));
    }

    void enqueue_incoming_message(std::string message_payload) const {
        auto shared_state = state;
        boost::asio::post(shared_state->timer.get_executor(),
                          [shared_state, payload = std::move(message_payload)]() mutable {
                              shared_state->queue.push(std::move(payload));
                              shared_state->timer.cancel();
                          });
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
        co_await boost::asio::post(strand, boost::asio::use_awaitable);

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
        co_await boost::asio::post(strand, boost::asio::use_awaitable);

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
        co_await boost::asio::post(strand, boost::asio::use_awaitable);

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
        co_await boost::asio::post(strand, boost::asio::use_awaitable);

        const auto pending_it = pending_responses.find(request_id_key);
        if (pending_it == pending_responses.end()) {
            co_return true;
        }

        co_return pending_it->second.response_ready;
    }

    Task<PendingResult> consume_pending_response(const std::string& request_id_key) {
        co_await boost::asio::post(strand, boost::asio::use_awaitable);

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
        co_await boost::asio::post(strand, boost::asio::use_awaitable);
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

        if (client_accepts_sse && pending_result.event_id.has_value()) {
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

    Task<void> handle_connection(boost::asio::ip::tcp::socket socket) {
        beast::tcp_stream stream(std::move(socket));
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

            auto response = co_await handle_request(request);
            const bool keep_connection_alive = response.keep_alive();
            co_await http::async_write(stream, response, boost::asio::use_awaitable);

            if (!keep_connection_alive) {
                break;
            }
        }

        boost::system::error_code shutdown_error;
        (void)stream.socket().shutdown(boost::asio::ip::tcp::socket::shutdown_send, shutdown_error);
    }

    std::string host;
    unsigned short port;
    boost::asio::strand<boost::asio::any_io_executor> strand;
    boost::asio::ip::tcp::acceptor acceptor;
    std::shared_ptr<SharedState> state;

    std::unordered_map<std::string, PendingResponse> pending_responses;
    std::optional<std::string> session_id;
    std::string negotiated_protocol_version{std::string(g_LATEST_PROTOCOL_VERSION)};
    bool session_active{false};

    bool allow_all_origins{true};
    std::unordered_set<std::string> allowed_origins;

    EventStore event_store;
};

HttpServerTransport::HttpServerTransport(const boost::asio::any_io_executor& executor, std::string host,
                                         unsigned short port, std::size_t event_store_capacity)
    : impl_(std::make_unique<Impl>(executor, std::move(host), port, event_store_capacity)) {}

HttpServerTransport::~HttpServerTransport() {
    try {
        close();
    } catch (...) {
        // Ignore exceptions in destructor
        (void)0;
    }
}

const EventStore& HttpServerTransport::event_store() const { return impl_->event_store; }

Task<std::string> HttpServerTransport::read_message() {
    auto& state = *impl_->state;
    for (;;) {
        if (!state.queue.empty()) {
            auto message_payload = std::move(state.queue.front());
            state.queue.pop();
            co_return message_payload;
        }

        if (state.closed.load(std::memory_order_acquire)) {
            throw std::runtime_error("HttpServerTransport is closed");
        }

        state.timer.expires_at(std::chrono::steady_clock::time_point::max());
        try {
            co_await state.timer.async_wait(boost::asio::use_awaitable);
        } catch (const boost::system::system_error& err) {
            if (err.code() != boost::asio::error::operation_aborted) {
                throw;
            }
        }
    }
}

Task<void> HttpServerTransport::write_message(std::string_view message) {
    std::string msg(message);
    co_await boost::asio::post(impl_->strand, boost::asio::use_awaitable);

    if (impl_->state->closed.load(std::memory_order_acquire)) {
        throw std::runtime_error("HttpServerTransport is closed");
    }

    const auto response_json = nlohmann::json::parse(msg, nullptr, false);
    if (response_json.is_discarded() || !response_json.is_object()) {
        co_return;
    }

    auto event_id = impl_->event_store.append(msg);

    if (!response_json.contains("id")) {
        co_return;
    }

    const auto request_id_key = response_json.at("id").dump();
    const auto pending_it = impl_->pending_responses.find(request_id_key);
    if (pending_it == impl_->pending_responses.end()) {
        co_return;
    }

    pending_it->second.response_body = msg;
    pending_it->second.event_id = std::move(event_id);
    pending_it->second.response_ready = true;

    if (Impl::is_initialize_result_response(response_json)) {
        const auto& result = response_json.at("result");
        if (!impl_->session_id.has_value()) {
            impl_->session_id = Impl::generate_session_id();
        }
        if (result.contains("protocolVersion") && result.at("protocolVersion").is_string()) {
            impl_->negotiated_protocol_version = result.at("protocolVersion").get<std::string>();
        }
        pending_it->second.session_header = impl_->session_id;
        impl_->session_active = true;
    }

    pending_it->second.ready_timer->cancel();
    co_return;
}

void HttpServerTransport::close() {
    if (impl_->state->closed.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    boost::asio::post(impl_->strand, [this]() {
        boost::system::error_code ec;
        (void)impl_->acceptor.cancel(ec);
        (void)impl_->acceptor.close(ec);

        for (auto& pending_entry : impl_->pending_responses) {
            pending_entry.second.ready_timer->cancel();
        }
        impl_->pending_responses.clear();

        impl_->session_id.reset();
        impl_->negotiated_protocol_version = std::string(g_LATEST_PROTOCOL_VERSION);
        impl_->session_active = false;
    });

    boost::asio::post(impl_->state->timer.get_executor(),
                      [state = impl_->state]() { state->timer.cancel(); });
}

Task<void> HttpServerTransport::listen() {
    for (;;) {
        if (impl_->state->closed.load(std::memory_order_acquire)) {
            co_return;
        }

        boost::asio::ip::tcp::socket socket(impl_->strand);
        try {
            socket = co_await impl_->acceptor.async_accept(boost::asio::use_awaitable);
        } catch (const boost::system::system_error& err) {
            if (impl_->state->closed.load(std::memory_order_acquire) ||
                err.code() == boost::asio::error::operation_aborted) {
                co_return;
            }
            throw;
        }

        boost::asio::co_spawn(impl_->strand, impl_->handle_connection(std::move(socket)),
                              [](const std::exception_ptr&) {
                                  // Connection errors (EOF, client disconnect) are normal;
                                  // handled per-connection, not propagated to the accept loop.
                              });
    }
}

}  // namespace mcp

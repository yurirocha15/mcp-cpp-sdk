#pragma once

#include <mcp/protocol.hpp>
#include <mcp/transport.hpp>

#include <atomic>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <chrono>
#include <cstdint>
#include <deque>
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

/**
 * @brief A bounded in-memory event store for SSE resumability.
 *
 * @details Stores recent SSE events with monotonically increasing IDs. When the
 * store exceeds its capacity, the oldest events are evicted. Clients can replay
 * missed events by providing the last event ID they received.
 *
 * Thread safety: Not thread-safe. Caller must synchronize access (e.g. via strand).
 */
class EventStore {
   public:
    /**
     * @brief Construct an event store with a given capacity.
     *
     * @param capacity Maximum number of events to retain.
     */
    explicit EventStore(std::size_t capacity = 1024) : capacity_(capacity) {}

    /**
     * @brief Append an event to the store and return its assigned ID.
     *
     * @param data The serialized event data (JSON-RPC message).
     * @return The monotonically increasing event ID assigned to this event.
     */
    std::string append(std::string data) {
        auto id = std::to_string(++next_id_);
        events_.push_back(StoredEvent{id, std::move(data)});
        while (events_.size() > capacity_) {
            events_.pop_front();
        }
        return id;
    }

    /**
     * @brief Retrieve all events after a given event ID.
     *
     * @param last_event_id The ID of the last event the client received.
     * @return A vector of (id, data) pairs for events after the given ID,
     *         or std::nullopt if the last_event_id has been evicted.
     */
    std::optional<std::vector<std::pair<std::string, std::string>>> events_after(
        const std::string& last_event_id) const {
        if (events_.empty()) {
            return std::vector<std::pair<std::string, std::string>>{};
        }

        auto it = events_.begin();
        bool found = false;
        for (; it != events_.end(); ++it) {
            if (it->id == last_event_id) {
                found = true;
                ++it;
                break;
            }
        }

        if (!found) {
            return std::nullopt;
        }

        std::vector<std::pair<std::string, std::string>> result;
        for (; it != events_.end(); ++it) {
            result.emplace_back(it->id, it->data);
        }
        return result;
    }

    /**
     * @brief Get all events currently in the store.
     *
     * @return A vector of (id, data) pairs for all stored events.
     */
    std::vector<std::pair<std::string, std::string>> all_events() const {
        std::vector<std::pair<std::string, std::string>> result;
        result.reserve(events_.size());
        for (const auto& event : events_) {
            result.emplace_back(event.id, event.data);
        }
        return result;
    }

    /**
     * @brief Get the number of events currently stored.
     */
    [[nodiscard]] std::size_t size() const { return events_.size(); }

    /**
     * @brief Get the maximum capacity of the store.
     */
    [[nodiscard]] std::size_t capacity() const { return capacity_; }

    /**
     * @brief Clear all stored events.
     */
    void clear() {
        events_.clear();
        next_id_ = 0;
    }

   private:
    struct StoredEvent {
        std::string id;
        std::string data;
    };

    std::size_t capacity_;
    std::deque<StoredEvent> events_;
    std::uint64_t next_id_{0};
};

/**
 * @brief HTTP server transport for MCP Streamable HTTP.
 *
 * @details Accepts HTTP requests with Boost.Beast, enqueues inbound JSON-RPC
 * messages for MCP server processing, and maps outbound JSON-RPC responses back
 * to the matching HTTP requests.
 */
class HttpServerTransport final : public ITransport {
   public:
    /**
     * @brief Construct an HTTP server transport bound to a host and port.
     *
     * @param executor Executor used for asynchronous operations.
     * @param host Local bind address.
     * @param port Local bind port.
     */
    HttpServerTransport(const boost::asio::any_io_executor& executor, std::string host,
                        unsigned short port, std::size_t event_store_capacity = 1024)
        : host_(std::move(host)),
          port_(port),
          strand_(boost::asio::make_strand(executor)),
          acceptor_(strand_),
          state_(std::make_shared<SharedState>(strand_)),
          event_store_(event_store_capacity) {
        state_->timer.expires_at(std::chrono::steady_clock::time_point::max());

        boost::system::error_code ec;
        const auto bind_address = boost::asio::ip::make_address(host_, ec);
        if (ec) {
            throw std::runtime_error("Invalid bind address: " + host_);
        }

        const auto endpoint = boost::asio::ip::tcp::endpoint(bind_address, port_);
        acceptor_.open(endpoint.protocol(), ec);
        if (ec) {
            throw std::runtime_error("Failed to open HTTP acceptor: " + ec.message());
        }

        acceptor_.set_option(boost::asio::socket_base::reuse_address(true), ec);
        if (ec) {
            throw std::runtime_error("Failed to set reuse_address: " + ec.message());
        }

        acceptor_.bind(endpoint, ec);
        if (ec) {
            throw std::runtime_error("Failed to bind HTTP acceptor: " + ec.message());
        }

        acceptor_.listen(boost::asio::socket_base::max_listen_connections, ec);
        if (ec) {
            throw std::runtime_error("Failed to listen on HTTP acceptor: " + ec.message());
        }
    }

    ~HttpServerTransport() override { close(); }

    HttpServerTransport(const HttpServerTransport&) = delete;
    HttpServerTransport& operator=(const HttpServerTransport&) = delete;
    HttpServerTransport(HttpServerTransport&&) = delete;
    HttpServerTransport& operator=(HttpServerTransport&&) = delete;

    [[nodiscard]] const EventStore& event_store() const { return event_store_; }

    /**
     * @brief Read the next queued JSON-RPC message from HTTP POST bodies.
     *
     * @details Uses the same timer-backed queue suspension pattern as
     * StdioTransport. Suspends while the queue is empty.
     *
     * @return A task resolving to the next JSON-RPC payload.
     */
    Task<std::string> read_message() override {
        auto& state = *state_;
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

    /**
     * @brief Store a JSON-RPC response for the pending HTTP request.
     *
     * @details Matches the response by JSON-RPC id and signals the waiting HTTP
     * handler through a per-request timer.
     *
     * @param message Serialized JSON-RPC response.
     * @return A task that completes when the response is stored.
     */
    Task<void> write_message(std::string_view message) override {
        co_await boost::asio::post(strand_, boost::asio::use_awaitable);

        if (state_->closed.load(std::memory_order_acquire)) {
            throw std::runtime_error("HttpServerTransport is closed");
        }

        const auto response_json = nlohmann::json::parse(message, nullptr, false);
        if (response_json.is_discarded() || !response_json.is_object()) {
            co_return;
        }

        auto event_id = event_store_.append(std::string(message));

        if (!response_json.contains("id")) {
            for (auto& timer : sse_clients_) {
                timer->cancel();
            }
            co_return;
        }

        const auto request_id_key = response_json.at("id").dump();
        const auto pending_it = pending_responses_.find(request_id_key);
        if (pending_it == pending_responses_.end()) {
            co_return;
        }

        pending_it->second.response_body = std::string(message);
        pending_it->second.event_id = std::move(event_id);
        pending_it->second.response_ready = true;

        if (is_initialize_result_response(response_json)) {
            if (!session_id_.has_value()) {
                session_id_ = generate_session_id();
            }
            pending_it->second.session_header = session_id_;
            session_active_ = true;
        }

        pending_it->second.ready_timer->cancel();
        co_return;
    }

    /**
     * @brief Close the transport and stop accepting connections.
     *
     * @details Cancels pending reads and pending per-request response waiters.
     */
    void close() override {
        if (state_->closed.exchange(true, std::memory_order_acq_rel)) {
            return;
        }

        boost::asio::post(strand_, [this]() {
            boost::system::error_code ec;
            acceptor_.cancel(ec);
            acceptor_.close(ec);

            for (auto& pending_entry : pending_responses_) {
                pending_entry.second.ready_timer->cancel();
            }
            pending_responses_.clear();

            session_id_.reset();
            session_active_ = false;
        });

        boost::asio::post(state_->timer.get_executor(), [state = state_]() { state->timer.cancel(); });
    }

    /**
     * @brief Start accepting HTTP connections.
     *
     * @details Runs an accept loop and spawns one coroutine per TCP connection.
     *
     * @return A task that completes when the accept loop exits.
     */
    Task<void> listen() {
        for (;;) {
            if (state_->closed.load(std::memory_order_acquire)) {
                co_return;
            }

            boost::asio::ip::tcp::socket socket(strand_);
            try {
                socket = co_await acceptor_.async_accept(boost::asio::use_awaitable);
            } catch (const boost::system::system_error& err) {
                if (state_->closed.load(std::memory_order_acquire) ||
                    err.code() == boost::asio::error::operation_aborted) {
                    co_return;
                }
                throw;
            }

            boost::asio::co_spawn(strand_, handle_connection(std::move(socket)),
                                  [](std::exception_ptr) {});
        }
    }

   private:
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

    static bool is_initialize_result_response(const nlohmann::json& response_json) {
        if (!response_json.is_object() || !response_json.contains("result")) {
            return false;
        }

        const auto& result_node = response_json.at("result");
        return result_node.is_object() && result_node.contains("protocolVersion");
    }

    static std::string generate_session_id() {
        static constexpr char hex_digits[] = "0123456789abcdef";
        std::random_device random_device;
        std::mt19937 generator(random_device());
        std::uniform_int_distribution<int> distribution(0, 15);

        std::string session_identifier(32, '0');
        for (char& current_char : session_identifier) {
            current_char = hex_digits[distribution(generator)];
        }
        return session_identifier;
    }

    bool is_origin_allowed(std::string_view origin_value) const {
        if (allow_all_origins_) {
            return true;
        }

        return allowed_origins_.contains(std::string(origin_value));
    }

    void enqueue_incoming_message(std::string message_payload) {
        auto state = state_;
        boost::asio::post(state->timer.get_executor(),
                          [state, payload = std::move(message_payload)]() mutable {
                              state->queue.push(std::move(payload));
                              state->timer.cancel();
                          });
    }

    static http::response<http::string_body> make_json_response(
        const http::request<http::string_body>& request, http::status status_code, std::string body) {
        http::response<http::string_body> response{status_code, request.version()};
        response.set(http::field::server, "mcp-cpp-sdk");
        response.set(http::field::content_type, "application/json");
        response.keep_alive(request.keep_alive());
        response.body() = std::move(body);
        response.prepare_payload();
        return response;
    }

    static http::response<http::string_body> make_error_response(
        const http::request<http::string_body>& request, http::status status_code,
        std::string_view error_message) {
        nlohmann::json error_body = nlohmann::json::object();
        error_body["error"] = std::string(error_message);
        return make_json_response(request, status_code, error_body.dump());
    }

    static http::response<http::string_body> make_empty_json_response(
        const http::request<http::string_body>& request, http::status status_code) {
        auto response = make_json_response(request, status_code, "");
        response.content_length(0);
        return response;
    }

    static http::response<http::string_body> make_sse_response(
        const http::request<http::string_body>& request, const std::string& event_id,
        const std::string& data) {
        std::string sse_body = "id: " + event_id + "\ndata: " + data + "\n\n";
        http::response<http::string_body> response{http::status::ok, request.version()};
        response.set(http::field::server, "mcp-cpp-sdk");
        response.set(http::field::content_type, "text/event-stream");
        response.set(http::field::cache_control, "no-cache");
        response.keep_alive(request.keep_alive());
        response.body() = std::move(sse_body);
        response.prepare_payload();
        return response;
    }

    static http::response<http::string_body> make_sse_replay_response(
        const http::request<http::string_body>& request,
        const std::vector<std::pair<std::string, std::string>>& events) {
        std::string sse_body;
        for (const auto& [id, data] : events) {
            sse_body += "id: " + id + "\ndata: " + data + "\n\n";
        }
        http::response<http::string_body> response{http::status::ok, request.version()};
        response.set(http::field::server, "mcp-cpp-sdk");
        response.set(http::field::content_type, "text/event-stream");
        response.set(http::field::cache_control, "no-cache");
        response.keep_alive(request.keep_alive());
        response.body() = std::move(sse_body);
        response.prepare_payload();
        return response;
    }

    Task<SessionCheckResult> validate_post_session(const http::request<http::string_body>& request) {
        co_await boost::asio::post(strand_, boost::asio::use_awaitable);

        const auto session_header_it = request.find("MCP-Session-Id");
        const bool session_header_present = session_header_it != request.end();

        if (session_id_.has_value()) {
            if (!session_header_present) {
                co_return SessionCheckResult{false, "Session active"};
            }

            if (session_header_it->value() != *session_id_) {
                co_return SessionCheckResult{false, "Invalid MCP-Session-Id header"};
            }

            co_return SessionCheckResult{true, {}};
        }

        if (session_active_ && !session_header_present) {
            co_return SessionCheckResult{false, "Session active"};
        }

        if (session_header_present) {
            co_return SessionCheckResult{false, "Session not established"};
        }

        co_return SessionCheckResult{true, {}};
    }

    Task<SessionCheckResult> validate_delete_session(const http::request<http::string_body>& request) {
        co_await boost::asio::post(strand_, boost::asio::use_awaitable);

        if (!session_id_.has_value()) {
            co_return SessionCheckResult{true, {}};
        }

        const auto session_header_it = request.find("MCP-Session-Id");
        if (session_header_it == request.end()) {
            co_return SessionCheckResult{false, "Missing MCP-Session-Id header"};
        }

        if (session_header_it->value() != *session_id_) {
            co_return SessionCheckResult{false, "Invalid MCP-Session-Id header"};
        }

        co_return SessionCheckResult{true, {}};
    }

    Task<std::optional<std::shared_ptr<boost::asio::steady_timer>>> register_pending_request(
        const std::string& request_id_key) {
        co_await boost::asio::post(strand_, boost::asio::use_awaitable);

        if (pending_responses_.contains(request_id_key)) {
            co_return std::nullopt;
        }

        auto timer_signal = std::make_shared<boost::asio::steady_timer>(strand_);
        timer_signal->expires_at(std::chrono::steady_clock::time_point::max());

        pending_responses_.emplace(request_id_key, PendingResponse{timer_signal, std::nullopt,
                                                                   std::nullopt, std::nullopt, false});
        co_return timer_signal;
    }

    Task<bool> is_response_ready(const std::string& request_id_key) {
        co_await boost::asio::post(strand_, boost::asio::use_awaitable);

        const auto pending_it = pending_responses_.find(request_id_key);
        if (pending_it == pending_responses_.end()) {
            co_return true;
        }

        co_return pending_it->second.response_ready;
    }

    Task<PendingResult> consume_pending_response(const std::string& request_id_key) {
        co_await boost::asio::post(strand_, boost::asio::use_awaitable);

        const auto pending_it = pending_responses_.find(request_id_key);
        if (pending_it == pending_responses_.end()) {
            co_return PendingResult{};
        }

        PendingResult pending_result{std::move(pending_it->second.response_body),
                                     std::move(pending_it->second.session_header),
                                     std::move(pending_it->second.event_id)};
        pending_responses_.erase(pending_it);
        co_return pending_result;
    }

    Task<void> terminate_session() {
        co_await boost::asio::post(strand_, boost::asio::use_awaitable);

        session_id_.reset();
        session_active_ = false;
        co_return;
    }

    Task<http::response<http::string_body>> handle_post(
        const http::request<http::string_body>& request) {
        const auto protocol_header_it = request.find("MCP-Protocol-Version");
        if (protocol_header_it == request.end() ||
            protocol_header_it->value() != LATEST_PROTOCOL_VERSION) {
            co_return make_error_response(request, http::status::bad_request,
                                          "Invalid MCP-Protocol-Version header");
        }

        const auto origin_header_it = request.find(http::field::origin);
        if (origin_header_it != request.end() && !is_origin_allowed(origin_header_it->value())) {
            co_return make_error_response(request, http::status::forbidden, "Origin not allowed");
        }

        const auto session_check = co_await validate_post_session(request);
        if (!session_check.ok) {
            co_return make_error_response(request, http::status::bad_request,
                                          session_check.error_message);
        }

        const auto request_json = nlohmann::json::parse(request.body(), nullptr, false);
        if (request_json.is_discarded() || !request_json.is_object()) {
            co_return make_error_response(request, http::status::bad_request,
                                          "Invalid JSON-RPC payload");
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
            if (state_->closed.load(std::memory_order_acquire)) {
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

        auto response =
            make_json_response(request, http::status::ok, std::move(*pending_result.response_body));
        if (pending_result.session_header.has_value()) {
            response.set("MCP-Session-Id", *pending_result.session_header);
        }
        co_return response;
    }

    Task<http::response<http::string_body>> handle_delete(
        const http::request<http::string_body>& request) {
        const auto protocol_header_it = request.find("MCP-Protocol-Version");
        if (protocol_header_it == request.end() ||
            protocol_header_it->value() != LATEST_PROTOCOL_VERSION) {
            co_return make_error_response(request, http::status::bad_request,
                                          "Invalid MCP-Protocol-Version header");
        }

        const auto origin_header_it = request.find(http::field::origin);
        if (origin_header_it != request.end() && !is_origin_allowed(origin_header_it->value())) {
            co_return make_error_response(request, http::status::forbidden, "Origin not allowed");
        }

        const auto session_check = co_await validate_delete_session(request);
        if (!session_check.ok) {
            co_return make_error_response(request, http::status::bad_request,
                                          session_check.error_message);
        }

        co_await terminate_session();
        co_return make_json_response(request, http::status::ok, "{}");
    }

    Task<http::response<http::string_body>> handle_get(
        const http::request<http::string_body>& request) {
        const auto protocol_header_it = request.find("MCP-Protocol-Version");
        if (protocol_header_it == request.end() ||
            protocol_header_it->value() != LATEST_PROTOCOL_VERSION) {
            co_return make_error_response(request, http::status::bad_request,
                                          "Invalid MCP-Protocol-Version header");
        }

        if (!session_id_.has_value()) {
            co_return make_error_response(request, http::status::bad_request, "No active session");
        }

        const auto session_header_it = request.find("MCP-Session-Id");
        if (session_header_it == request.end() || session_header_it->value() != *session_id_) {
            co_return make_error_response(request, http::status::bad_request,
                                          "Invalid MCP-Session-Id header");
        }

        const auto last_event_id_it = request.find("Last-Event-ID");
        if (last_event_id_it != request.end()) {
            auto last_id = std::string(last_event_id_it->value());
            auto missed_events = event_store_.events_after(last_id);
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

    Task<http::response<http::string_body>> handle_request(
        const http::request<http::string_body>& request) {
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
            http::request<http::string_body> request;
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
        stream.socket().shutdown(boost::asio::ip::tcp::socket::shutdown_send, shutdown_error);
    }

    std::string host_;
    unsigned short port_;
    boost::asio::strand<boost::asio::any_io_executor> strand_;
    boost::asio::ip::tcp::acceptor acceptor_;
    std::shared_ptr<SharedState> state_;

    std::unordered_map<std::string, PendingResponse> pending_responses_;
    std::optional<std::string> session_id_;
    bool session_active_{false};

    bool allow_all_origins_{true};
    std::unordered_set<std::string> allowed_origins_;

    EventStore event_store_;
    std::vector<std::shared_ptr<boost::asio::steady_timer>> sse_clients_;
};

}  // namespace mcp

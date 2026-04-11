#pragma once

#include <mcp/protocol.hpp>
#include <mcp/server.hpp>
#include <mcp/transport.hpp>
#include <mcp/transport/http_server.hpp>
#include <mcp/transport/memory.hpp>

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
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace mcp {

namespace detail_session_mgr {

namespace beast = boost::beast;
namespace http = boost::beast::http;

// ============================================================================
// Per-session runtime state
// ============================================================================

struct PendingResponse {
    std::shared_ptr<boost::asio::steady_timer> ready_timer;
    std::optional<std::string> response_body;
    std::optional<std::string> event_id;
    bool response_ready{false};
};

struct PendingResult {
    std::optional<std::string> response_body;
    std::optional<std::string> event_id;
};

struct SessionRuntime {
    std::string session_id;

    /// Client-side of the MemoryTransport pair (manager writes requests here,
    /// reads responses from here). The server-side is owned by Server::run().
    std::unique_ptr<ITransport> client_transport;
    MemoryTransport* client_transport_ptr{nullptr};

    /// Per-session event store for SSE resumability.
    EventStore event_store;

    /// Pending request→response map, keyed by JSON-RPC id (dump of id field).
    std::unordered_map<std::string, PendingResponse> pending_responses;

    explicit SessionRuntime(std::string id, std::size_t event_store_capacity = 1024)
        : session_id(std::move(id)), event_store(event_store_capacity) {}
};

// ============================================================================
// Helpers
// ============================================================================

inline std::string generate_session_id() {
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

inline bool is_initialize_request(const nlohmann::json& request_json) {
    return request_json.is_object() && request_json.contains("method") &&
           request_json.at("method").get<std::string>() == "initialize";
}

inline http::response<http::string_body> make_json_response(
    const http::request<http::string_body>& request, http::status status_code, std::string body) {
    http::response<http::string_body> response{status_code, request.version()};
    response.set(http::field::server, "mcp-cpp-sdk");
    response.set(http::field::content_type, "application/json");
    response.keep_alive(request.keep_alive());
    response.body() = std::move(body);
    response.prepare_payload();
    return response;
}

inline http::response<http::string_body> make_error_response(
    const http::request<http::string_body>& request, http::status status_code,
    std::string_view error_message) {
    nlohmann::json error_body = nlohmann::json::object();
    error_body["error"] = std::string(error_message);
    return make_json_response(request, status_code, error_body.dump());
}

inline http::response<http::string_body> make_jsonrpc_error_response(
    const http::request<http::string_body>& request, http::status status_code, int error_code,
    std::string_view error_message) {
    nlohmann::json error_body = {{"jsonrpc", "2.0"},
                                 {"error", {{"code", error_code}, {"message", error_message}}},
                                 {"id", nullptr}};
    return make_json_response(request, status_code, error_body.dump());
}

inline http::response<http::string_body> make_empty_json_response(
    const http::request<http::string_body>& request, http::status status_code) {
    auto response = make_json_response(request, status_code, "");
    response.content_length(0);
    return response;
}

inline http::response<http::string_body> make_sse_response(
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

inline http::response<http::string_body> make_sse_replay_response(
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

}  // namespace detail_session_mgr

/**
 * @brief Multi-session HTTP endpoint for MCP Streamable HTTP.
 *
 * @details Manages multiple concurrent MCP sessions over a single HTTP port.
 * Each session is backed by its own Server + MemoryTransport pair. Requests
 * are routed by the `Mcp-Session-Id` header. New sessions are created on
 * `initialize` requests that arrive without a session ID.
 *
 * This matches the behavior of Python's StreamableHTTPSessionManager and
 * Go's StreamableHTTPServer: session creation on initialize, 404 on unknown
 * sessions, DELETE for session teardown.
 *
 * This class is NOT an ITransport. It owns the HTTP listener and manages
 * the full lifecycle of per-session Server instances.
 *
 * Usage:
 * @code
 * asio::io_context io_ctx;
 *
 * auto server_factory = [](const asio::any_io_executor&) {
 *     ServerCapabilities caps;
 *     caps.tools = ServerCapabilities::ToolsCapability{};
 *     Server server({"my-server", "1.0"}, std::move(caps));
 *     server.add_tool<json, json>("echo", "Echo tool", schema, handler);
 *     return server;
 * };
 *
 * StreamableHttpSessionManager manager(io_ctx.get_executor(), "0.0.0.0", 8080,
 *                                       std::move(server_factory));
 * asio::co_spawn(io_ctx, manager.listen(), asio::detached);
 * io_ctx.run();
 * @endcode
 */
class StreamableHttpSessionManager {
   public:
    /// Callback that creates a configured Server instance for a new session.
    using ServerFactory = std::function<std::unique_ptr<Server>(const boost::asio::any_io_executor&)>;

    /// Optional callback for handling non-MCP HTTP requests (e.g., health checks).
    /// Return std::nullopt to let the session manager handle the request normally.
    using CustomRequestHandler =
        std::function<std::optional<boost::beast::http::response<boost::beast::http::string_body>>(
            const boost::beast::http::request<boost::beast::http::string_body>&)>;

    /**
     * @brief Construct a multi-session HTTP endpoint.
     *
     * @param executor Executor for async operations.
     * @param host Local bind address.
     * @param port Local bind port.
     * @param factory Callback that creates a configured Server for each new session.
     * @param event_store_capacity Per-session event store capacity.
     */
    StreamableHttpSessionManager(const boost::asio::any_io_executor& executor, std::string host,
                                 unsigned short port, ServerFactory factory,
                                 std::size_t event_store_capacity = 1024)
        : host_(std::move(host)),
          port_(port),
          executor_(executor),
          strand_(boost::asio::make_strand(executor)),
          acceptor_(strand_),
          factory_(std::move(factory)),
          event_store_capacity_(event_store_capacity) {
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

    ~StreamableHttpSessionManager() { close(); }

    StreamableHttpSessionManager(const StreamableHttpSessionManager&) = delete;
    StreamableHttpSessionManager& operator=(const StreamableHttpSessionManager&) = delete;
    StreamableHttpSessionManager(StreamableHttpSessionManager&&) = delete;
    StreamableHttpSessionManager& operator=(StreamableHttpSessionManager&&) = delete;

    /**
     * @brief Set a custom request handler for non-MCP routes (e.g., health checks).
     *
     * @param handler A function that receives the HTTP request and optionally returns
     *                a response. If it returns std::nullopt, the request is handled
     *                as MCP protocol.
     */
    void set_custom_request_handler(CustomRequestHandler handler) {
        custom_handler_ = std::move(handler);
    }

    /**
     * @brief Get the number of active sessions.
     */
    [[nodiscard]] std::size_t session_count() const { return sessions_.size(); }

    /**
     * @brief Close the manager and all sessions.
     */
    void close() {
        if (closed_.exchange(true, std::memory_order_acq_rel)) {
            return;
        }

        boost::asio::post(strand_, [this]() {
            boost::system::error_code ec;
            acceptor_.cancel(ec);
            acceptor_.close(ec);

            for (auto& [id, session] : sessions_) {
                for (auto& [key, pending] : session->pending_responses) {
                    pending.ready_timer->cancel();
                }
                if (session->client_transport) {
                    session->client_transport->close();
                }
            }
            sessions_.clear();
        });
    }

    /**
     * @brief Start accepting HTTP connections.
     *
     * @return A task that completes when the accept loop exits.
     */
    Task<void> listen() {
        namespace beast = boost::beast;
        namespace http = boost::beast::http;

        for (;;) {
            if (closed_.load(std::memory_order_acquire)) {
                co_return;
            }

            boost::asio::ip::tcp::socket socket(strand_);
            try {
                socket = co_await acceptor_.async_accept(boost::asio::use_awaitable);
            } catch (const boost::system::system_error& err) {
                if (closed_.load(std::memory_order_acquire) ||
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
    using SessionMap =
        std::unordered_map<std::string, std::unique_ptr<detail_session_mgr::SessionRuntime>>;

    // ========================================================================
    // Session lifecycle
    // ========================================================================

    detail_session_mgr::SessionRuntime* create_session() {
        auto session_id = detail_session_mgr::generate_session_id();

        auto client_mem = std::make_unique<MemoryTransport>(executor_);
        auto server_mem = std::make_unique<MemoryTransport>(executor_);
        client_mem->set_peer(server_mem.get());
        server_mem->set_peer(client_mem.get());

        auto* client_mem_ptr = client_mem.get();
        auto server = factory_(executor_);
        std::unique_ptr<ITransport> server_transport(server_mem.release());

        // Spawn Server::run() — it owns the server-side transport.
        // When the client transport closes, the server's read loop exits.
        boost::asio::co_spawn(
            executor_,
            [srv = std::move(server), transport = std::move(server_transport),
             exec = executor_]() mutable -> Task<void> {
                co_await srv->run(std::move(transport), exec);
            },
            boost::asio::detached);

        auto runtime =
            std::make_unique<detail_session_mgr::SessionRuntime>(session_id, event_store_capacity_);
        runtime->client_transport.reset(client_mem.release());
        runtime->client_transport_ptr = client_mem_ptr;

        auto* runtime_ptr = runtime.get();
        sessions_.emplace(session_id, std::move(runtime));

        return runtime_ptr;
    }

    detail_session_mgr::SessionRuntime* find_session(const std::string& session_id) {
        auto it = sessions_.find(session_id);
        if (it == sessions_.end()) {
            return nullptr;
        }
        return it->second.get();
    }

    void destroy_session(const std::string& session_id) {
        auto it = sessions_.find(session_id);
        if (it == sessions_.end()) {
            return;
        }
        for (auto& [key, pending] : it->second->pending_responses) {
            pending.ready_timer->cancel();
        }
        if (it->second->client_transport) {
            it->second->client_transport->close();
        }
        sessions_.erase(it);
    }

    // ========================================================================
    // HTTP connection handling
    // ========================================================================

    Task<void> handle_connection(boost::asio::ip::tcp::socket socket) {
        namespace beast = boost::beast;
        namespace http = boost::beast::http;

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

    Task<boost::beast::http::response<boost::beast::http::string_body>> handle_request(
        const boost::beast::http::request<boost::beast::http::string_body>& request) {
        namespace http = boost::beast::http;

        // Custom handler (health checks, etc.) takes priority
        if (custom_handler_) {
            auto custom_response = custom_handler_(request);
            if (custom_response.has_value()) {
                co_return std::move(*custom_response);
            }
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

        auto response = detail_session_mgr::make_error_response(
            request, http::status::method_not_allowed, "Method not allowed");
        response.set(http::field::allow, "GET, POST, DELETE");
        co_return response;
    }

    // ========================================================================
    // POST handler — route to session or create new session
    // ========================================================================

    Task<boost::beast::http::response<boost::beast::http::string_body>> handle_post(
        const boost::beast::http::request<boost::beast::http::string_body>& request) {
        namespace http = boost::beast::http;
        // Protocol version check
        const auto protocol_header_it = request.find("MCP-Protocol-Version");
        if (protocol_header_it == request.end() ||
            protocol_header_it->value() != LATEST_PROTOCOL_VERSION) {
            co_return detail_session_mgr::make_error_response(request, http::status::bad_request,
                                                              "Invalid MCP-Protocol-Version header");
        }

        // Parse JSON-RPC payload
        const auto request_json = nlohmann::json::parse(request.body(), nullptr, false);
        if (request_json.is_discarded() || !request_json.is_object()) {
            co_return detail_session_mgr::make_error_response(request, http::status::bad_request,
                                                              "Invalid JSON-RPC payload");
        }

        // Extract session header
        const auto session_header_it = request.find("Mcp-Session-Id");
        const bool has_session_header = session_header_it != request.end();
        std::string session_id_value;
        if (has_session_header) {
            session_id_value = std::string(session_header_it->value());
        }

        detail_session_mgr::SessionRuntime* session = nullptr;

        if (!has_session_header) {
            // No session header — must be an initialize request to create a new session
            if (!detail_session_mgr::is_initialize_request(request_json)) {
                co_return detail_session_mgr::make_jsonrpc_error_response(
                    request, http::status::bad_request, -32600,
                    "Missing Mcp-Session-Id header; only initialize is allowed without a session");
            }
            session = create_session();
        } else {
            // Session header present — look up existing session
            session = find_session(session_id_value);
            if (!session) {
                co_return detail_session_mgr::make_jsonrpc_error_response(
                    request, http::status::not_found, -32600, "Session not found");
            }
        }

        const bool has_request_id = request_json.contains("id");
        const bool has_method = request_json.contains("method");

        // Notification (no id): fire-and-forget
        if (!has_request_id || !has_method) {
            co_await session->client_transport_ptr->write_message(request.body());
            auto response =
                detail_session_mgr::make_empty_json_response(request, http::status::accepted);
            response.set("Mcp-Session-Id", session->session_id);
            co_return response;
        }

        // Request with id: register pending, send to server, await response
        const auto request_id_key = request_json.at("id").dump();

        // Check for duplicate id
        if (session->pending_responses.contains(request_id_key)) {
            co_return detail_session_mgr::make_error_response(request, http::status::bad_request,
                                                              "Request id already pending");
        }

        auto timer_signal = std::make_shared<boost::asio::steady_timer>(strand_);
        timer_signal->expires_at(std::chrono::steady_clock::time_point::max());

        session->pending_responses.emplace(
            request_id_key,
            detail_session_mgr::PendingResponse{timer_signal, std::nullopt, std::nullopt, false});

        // Send the request to the Server via MemoryTransport
        co_await session->client_transport_ptr->write_message(request.body());

        // Spawn a reader coroutine that consumes the response from the client transport
        // and matches it to pending requests.
        //
        // We need to read the response from the MemoryTransport. The response will
        // come back through the client_transport_ptr.
        // Wait for the server to produce a response by reading from client transport.
        auto* session_ptr = session;
        boost::asio::co_spawn(
            strand_,
            [this, session_ptr]() -> Task<void> {
                try {
                    auto response_str = co_await session_ptr->client_transport_ptr->read_message();
                    co_await dispatch_response_to_pending(session_ptr, std::move(response_str));
                } catch (...) {
                    // Transport closed or error — cancel all pending
                    for (auto& [key, pending] : session_ptr->pending_responses) {
                        pending.ready_timer->cancel();
                    }
                }
            },
            boost::asio::detached);

        // Wait for our response to be ready
        for (;;) {
            if (closed_.load(std::memory_order_acquire)) {
                consume_pending_response(session, request_id_key);
                co_return detail_session_mgr::make_error_response(
                    request, http::status::internal_server_error,
                    "Transport closed while waiting response");
            }

            auto pending_it = session->pending_responses.find(request_id_key);
            if (pending_it != session->pending_responses.end() && pending_it->second.response_ready) {
                break;
            }
            if (pending_it == session->pending_responses.end()) {
                // Already consumed elsewhere — shouldn't happen but handle gracefully
                co_return detail_session_mgr::make_error_response(
                    request, http::status::internal_server_error, "Response lost");
            }

            try {
                co_await timer_signal->async_wait(boost::asio::use_awaitable);
            } catch (const boost::system::system_error& err) {
                if (err.code() != boost::asio::error::operation_aborted) {
                    throw;
                }
            }
        }

        auto result = consume_pending_response(session, request_id_key);
        if (!result.response_body.has_value()) {
            co_return detail_session_mgr::make_error_response(
                request, http::status::internal_server_error, "Missing response body for request");
        }

        // Check if client accepts SSE
        const auto accept_it = request.find(http::field::accept);
        const bool client_accepts_sse =
            accept_it != request.end() &&
            std::string_view(accept_it->value()).find("text/event-stream") != std::string_view::npos;

        if (client_accepts_sse && result.event_id.has_value()) {
            auto response =
                detail_session_mgr::make_sse_response(request, *result.event_id, *result.response_body);
            response.set("Mcp-Session-Id", session_ptr->session_id);
            co_return response;
        }

        auto response = detail_session_mgr::make_json_response(request, http::status::ok,
                                                               std::move(*result.response_body));
        response.set("Mcp-Session-Id", session_ptr->session_id);
        co_return response;
    }

    // ========================================================================
    // DELETE handler — destroy session
    // ========================================================================

    Task<boost::beast::http::response<boost::beast::http::string_body>> handle_delete(
        const boost::beast::http::request<boost::beast::http::string_body>& request) {
        namespace http = boost::beast::http;
        const auto protocol_header_it = request.find("MCP-Protocol-Version");
        if (protocol_header_it == request.end() ||
            protocol_header_it->value() != LATEST_PROTOCOL_VERSION) {
            co_return detail_session_mgr::make_error_response(request, http::status::bad_request,
                                                              "Invalid MCP-Protocol-Version header");
        }

        const auto session_header_it = request.find("Mcp-Session-Id");
        if (session_header_it == request.end()) {
            co_return detail_session_mgr::make_error_response(request, http::status::bad_request,
                                                              "Missing Mcp-Session-Id header");
        }

        std::string session_id_value(session_header_it->value());
        auto* session = find_session(session_id_value);
        if (!session) {
            co_return detail_session_mgr::make_jsonrpc_error_response(request, http::status::not_found,
                                                                      -32600, "Session not found");
        }

        destroy_session(session_id_value);
        co_return detail_session_mgr::make_json_response(request, http::status::ok, "{}");
    }

    // ========================================================================
    // GET handler — SSE replay from per-session EventStore
    // ========================================================================

    Task<boost::beast::http::response<boost::beast::http::string_body>> handle_get(
        const boost::beast::http::request<boost::beast::http::string_body>& request) {
        namespace http = boost::beast::http;
        const auto protocol_header_it = request.find("MCP-Protocol-Version");
        if (protocol_header_it == request.end() ||
            protocol_header_it->value() != LATEST_PROTOCOL_VERSION) {
            co_return detail_session_mgr::make_error_response(request, http::status::bad_request,
                                                              "Invalid MCP-Protocol-Version header");
        }

        const auto session_header_it = request.find("Mcp-Session-Id");
        if (session_header_it == request.end()) {
            co_return detail_session_mgr::make_error_response(request, http::status::bad_request,
                                                              "Missing Mcp-Session-Id header");
        }

        std::string session_id_value(session_header_it->value());
        auto* session = find_session(session_id_value);
        if (!session) {
            co_return detail_session_mgr::make_jsonrpc_error_response(request, http::status::not_found,
                                                                      -32600, "Session not found");
        }

        const auto last_event_id_it = request.find("Last-Event-ID");
        if (last_event_id_it != request.end()) {
            auto last_id = std::string(last_event_id_it->value());
            auto missed_events = session->event_store.events_after(last_id);
            if (!missed_events.has_value()) {
                co_return detail_session_mgr::make_error_response(
                    request, http::status::gone, "Event ID has been evicted from store");
            }
            if (!missed_events->empty()) {
                co_return detail_session_mgr::make_sse_replay_response(request, *missed_events);
            }
        }

        co_return detail_session_mgr::make_empty_json_response(request, http::status::ok);
    }

    // ========================================================================
    // Response dispatch — match server response to pending HTTP request
    // ========================================================================

    Task<void> dispatch_response_to_pending(detail_session_mgr::SessionRuntime* session,
                                            std::string response_str) {
        const auto response_json = nlohmann::json::parse(response_str, nullptr, false);
        if (response_json.is_discarded() || !response_json.is_object()) {
            co_return;
        }

        // Store in event store
        auto event_id = session->event_store.append(response_str);

        if (!response_json.contains("id")) {
            // Notification from server — no pending request to match
            co_return;
        }

        const auto request_id_key = response_json.at("id").dump();
        auto pending_it = session->pending_responses.find(request_id_key);
        if (pending_it == session->pending_responses.end()) {
            co_return;
        }

        pending_it->second.response_body = std::move(response_str);
        pending_it->second.event_id = std::move(event_id);
        pending_it->second.response_ready = true;
        pending_it->second.ready_timer->cancel();
    }

    detail_session_mgr::PendingResult consume_pending_response(
        detail_session_mgr::SessionRuntime* session, const std::string& request_id_key) {
        auto pending_it = session->pending_responses.find(request_id_key);
        if (pending_it == session->pending_responses.end()) {
            return {};
        }
        detail_session_mgr::PendingResult result{std::move(pending_it->second.response_body),
                                                 std::move(pending_it->second.event_id)};
        session->pending_responses.erase(pending_it);
        return result;
    }

    // ========================================================================
    // Data members
    // ========================================================================

    std::string host_;
    unsigned short port_;
    boost::asio::any_io_executor executor_;
    boost::asio::strand<boost::asio::any_io_executor> strand_;
    boost::asio::ip::tcp::acceptor acceptor_;

    ServerFactory factory_;
    CustomRequestHandler custom_handler_;
    std::size_t event_store_capacity_;

    std::atomic<bool> closed_{false};
    SessionMap sessions_;
};

}  // namespace mcp

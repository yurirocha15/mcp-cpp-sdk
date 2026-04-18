#include <mcp/transport/http_session_manager.hpp>
#include <mcp/transport/memory.hpp>

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

#include <atomic>
#include <chrono>
#include <cstddef>
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
#include <vector>

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

    std::shared_ptr<ITransport> client_transport;
    MemoryTransport* client_transport_ptr{nullptr};

    EventStore event_store;

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

// ============================================================================
// Impl
// ============================================================================

struct StreamableHttpSessionManager::Impl {
    using SessionMap =
        std::unordered_map<std::string, std::unique_ptr<detail_session_mgr::SessionRuntime>>;

    std::string host;
    unsigned short port;
    boost::asio::any_io_executor executor;
    boost::asio::strand<boost::asio::any_io_executor> strand;
    boost::asio::ip::tcp::acceptor acceptor;

    StreamableHttpSessionManager::ServerFactory factory;
    StreamableHttpSessionManager::CustomRequestHandler custom_handler;
    std::size_t event_store_capacity;

    std::atomic<bool> closed{false};
    SessionMap sessions;

    Impl(const boost::asio::any_io_executor& exec, std::string host_arg, unsigned short port_arg,
         StreamableHttpSessionManager::ServerFactory factory_arg, std::size_t capacity)
        : host(std::move(host_arg)),
          port(port_arg),
          executor(exec),
          strand(boost::asio::make_strand(exec)),
          acceptor(strand),
          factory(std::move(factory_arg)),
          event_store_capacity(capacity) {
        boost::system::error_code ec;
        const auto bind_address = boost::asio::ip::make_address(host, ec);
        if (ec) {
            throw std::runtime_error("Invalid bind address: " + host);
        }

        const auto endpoint = boost::asio::ip::tcp::endpoint(bind_address, port);
        acceptor.open(endpoint.protocol(), ec);
        if (ec) {
            throw std::runtime_error("Failed to open HTTP acceptor: " + ec.message());
        }

        acceptor.set_option(boost::asio::socket_base::reuse_address(true), ec);
        if (ec) {
            throw std::runtime_error("Failed to set reuse_address: " + ec.message());
        }

        acceptor.bind(endpoint, ec);
        if (ec) {
            throw std::runtime_error("Failed to bind HTTP acceptor: " + ec.message());
        }

        acceptor.listen(boost::asio::socket_base::max_listen_connections, ec);
        if (ec) {
            throw std::runtime_error("Failed to listen on HTTP acceptor: " + ec.message());
        }
    }

    // Session lifecycle

    detail_session_mgr::SessionRuntime* create_session() {
        auto session_id = detail_session_mgr::generate_session_id();

        auto client_mem = std::make_shared<MemoryTransport>(executor);
        auto server_mem = std::make_shared<MemoryTransport>(executor);
        client_mem->set_peer(server_mem);
        server_mem->set_peer(client_mem);

        auto* client_mem_ptr = client_mem.get();
        auto server = factory(executor);
        std::shared_ptr<ITransport> server_transport = server_mem;

        boost::asio::co_spawn(
            executor,
            [srv = std::move(server), transport = std::move(server_transport),
             exec = executor]() mutable -> Task<void> {
                co_await srv->run(std::move(transport), exec);
            },
            boost::asio::detached);

        auto runtime =
            std::make_unique<detail_session_mgr::SessionRuntime>(session_id, event_store_capacity);
        runtime->client_transport = client_mem;
        runtime->client_transport_ptr = client_mem_ptr;

        auto* runtime_ptr = runtime.get();
        sessions.emplace(session_id, std::move(runtime));

        return runtime_ptr;
    }

    detail_session_mgr::SessionRuntime* find_session(const std::string& session_id) {
        auto it = sessions.find(session_id);
        if (it == sessions.end()) {
            return nullptr;
        }
        return it->second.get();
    }

    void destroy_session(const std::string& session_id) {
        auto it = sessions.find(session_id);
        if (it == sessions.end()) {
            return;
        }
        for (auto& [key, pending] : it->second->pending_responses) {
            pending.ready_timer->cancel();
        }
        if (it->second->client_transport) {
            it->second->client_transport->close();
        }
        sessions.erase(it);
    }

    // HTTP connection / request handling

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

        if (custom_handler) {
            auto custom_response = custom_handler(request);
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

    Task<boost::beast::http::response<boost::beast::http::string_body>> handle_post(
        const boost::beast::http::request<boost::beast::http::string_body>& request) {
        namespace http = boost::beast::http;

        const auto protocol_header_it = request.find("MCP-Protocol-Version");
        if (protocol_header_it == request.end() ||
            protocol_header_it->value() != LATEST_PROTOCOL_VERSION) {
            co_return detail_session_mgr::make_error_response(request, http::status::bad_request,
                                                              "Invalid MCP-Protocol-Version header");
        }

        const auto request_json = nlohmann::json::parse(request.body(), nullptr, false);
        if (request_json.is_discarded() || !request_json.is_object()) {
            co_return detail_session_mgr::make_error_response(request, http::status::bad_request,
                                                              "Invalid JSON-RPC payload");
        }

        const auto session_header_it = request.find("Mcp-Session-Id");
        const bool has_session_header = session_header_it != request.end();
        std::string session_id_value;
        if (has_session_header) {
            session_id_value = std::string(session_header_it->value());
        }

        detail_session_mgr::SessionRuntime* session = nullptr;

        if (!has_session_header) {
            if (!detail_session_mgr::is_initialize_request(request_json)) {
                co_return detail_session_mgr::make_jsonrpc_error_response(
                    request, http::status::bad_request, -32600,
                    "Missing Mcp-Session-Id header; only initialize is allowed without a session");
            }
            session = create_session();
        } else {
            session = find_session(session_id_value);
            if (!session) {
                co_return detail_session_mgr::make_jsonrpc_error_response(
                    request, http::status::not_found, -32600, "Session not found");
            }
        }

        const bool has_request_id = request_json.contains("id");
        const bool has_method = request_json.contains("method");

        if (!has_request_id || !has_method) {
            co_await session->client_transport_ptr->write_message(request.body());
            auto response =
                detail_session_mgr::make_empty_json_response(request, http::status::accepted);
            response.set("Mcp-Session-Id", session->session_id);
            co_return response;
        }

        const auto request_id_key = request_json.at("id").dump();

        if (session->pending_responses.contains(request_id_key)) {
            co_return detail_session_mgr::make_error_response(request, http::status::bad_request,
                                                              "Request id already pending");
        }

        auto timer_signal = std::make_shared<boost::asio::steady_timer>(strand);
        timer_signal->expires_at(std::chrono::steady_clock::time_point::max());

        session->pending_responses.emplace(
            request_id_key,
            detail_session_mgr::PendingResponse{timer_signal, std::nullopt, std::nullopt, false});

        co_await session->client_transport_ptr->write_message(request.body());

        auto* session_ptr = session;
        boost::asio::co_spawn(
            strand,
            [this, session_ptr]() -> Task<void> {
                try {
                    auto response_str = co_await session_ptr->client_transport_ptr->read_message();
                    co_await dispatch_response_to_pending(session_ptr, std::move(response_str));
                } catch (...) {
                    for (auto& [key, pending] : session_ptr->pending_responses) {
                        pending.ready_timer->cancel();
                    }
                }
            },
            boost::asio::detached);

        for (;;) {
            if (closed.load(std::memory_order_acquire)) {
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

    Task<void> dispatch_response_to_pending(detail_session_mgr::SessionRuntime* session,
                                            std::string response_str) {
        const auto response_json = nlohmann::json::parse(response_str, nullptr, false);
        if (response_json.is_discarded() || !response_json.is_object()) {
            co_return;
        }

        auto event_id = session->event_store.append(response_str);

        if (!response_json.contains("id")) {
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
};

// ============================================================================
// StreamableHttpSessionManager public methods
// ============================================================================

StreamableHttpSessionManager::StreamableHttpSessionManager(const boost::asio::any_io_executor& executor,
                                                           std::string host, unsigned short port,
                                                           ServerFactory factory,
                                                           std::size_t event_store_capacity)
    : impl_(std::make_unique<Impl>(executor, std::move(host), port, std::move(factory),
                                   event_store_capacity)) {}

StreamableHttpSessionManager::~StreamableHttpSessionManager() { close(); }

void StreamableHttpSessionManager::set_custom_request_handler(CustomRequestHandler handler) {
    impl_->custom_handler = std::move(handler);
}

std::size_t StreamableHttpSessionManager::session_count() const { return impl_->sessions.size(); }

void StreamableHttpSessionManager::close() {
    if (impl_->closed.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    boost::asio::post(impl_->strand, [this]() {
        boost::system::error_code ec;
        impl_->acceptor.cancel(ec);
        impl_->acceptor.close(ec);

        for (auto& [id, session] : impl_->sessions) {
            for (auto& [key, pending] : session->pending_responses) {
                pending.ready_timer->cancel();
            }
            if (session->client_transport) {
                session->client_transport->close();
            }
        }
        impl_->sessions.clear();
    });
}

Task<void> StreamableHttpSessionManager::listen() {
    for (;;) {
        if (impl_->closed.load(std::memory_order_acquire)) {
            co_return;
        }

        boost::asio::ip::tcp::socket socket(impl_->strand);
        try {
            socket = co_await impl_->acceptor.async_accept(boost::asio::use_awaitable);
        } catch (const boost::system::system_error& err) {
            if (impl_->closed.load(std::memory_order_acquire) ||
                err.code() == boost::asio::error::operation_aborted) {
                co_return;
            }
            throw;
        }

        boost::asio::co_spawn(impl_->strand, impl_->handle_connection(std::move(socket)),
                              [](std::exception_ptr) {
                                  // Connection errors (EOF, client disconnect) are normal;
                                  // handled per-connection, not propagated to the accept loop.
                              });
    }
}

}  // namespace mcp

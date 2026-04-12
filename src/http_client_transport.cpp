#include <mcp/transport/http_client.hpp>

#include <atomic>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <chrono>
#include <memory>
#include <optional>
#include <queue>
#include <stdexcept>
#include <string>
#include <string_view>

namespace mcp {

namespace beast = boost::beast;
namespace http = boost::beast::http;
namespace net = boost::asio;

struct HttpClientTransport::Impl {
    struct ParsedUrl {
        std::string host;
        std::string port;
        std::string path;
    };

    struct SharedState {
        explicit SharedState(net::strand<net::any_io_executor>& strand)
            : timer(strand), resolver(strand) {}

        net::steady_timer timer;
        net::ip::tcp::resolver resolver;
        std::optional<beast::tcp_stream> stream;
        std::queue<std::string> queue;
        std::atomic<bool> closed{false};
        std::string session_id;
        std::string last_event_id;
    };

    struct ParsedSseEvent {
        std::string data;
        std::string id;
    };

    Impl(const net::any_io_executor& executor, std::string url)
        : strand(net::make_strand(executor)), state(std::make_shared<SharedState>(strand)) {
        auto parsed = parse_url(std::move(url));
        host = std::move(parsed.host);
        port = std::move(parsed.port);
        path = std::move(parsed.path);
        state->timer.expires_at(std::chrono::steady_clock::time_point::max());
    }

    static ParsedUrl parse_url(std::string url) {
        constexpr std::string_view scheme = "http://";
        if (url.rfind(std::string(scheme), 0) != 0) {
            throw std::invalid_argument("HttpClientTransport URL must start with http://");
        }

        const std::string authority_and_path = url.substr(scheme.size());
        if (authority_and_path.empty()) {
            throw std::invalid_argument("HttpClientTransport URL is missing host");
        }

        const std::size_t path_separator = authority_and_path.find('/');
        std::string authority = authority_and_path.substr(0, path_separator);
        std::string path =
            path_separator == std::string::npos ? "/" : authority_and_path.substr(path_separator);

        if (authority.empty()) {
            throw std::invalid_argument("HttpClientTransport URL is missing host");
        }

        std::string host;
        std::string port = "80";

        const std::size_t colon_position = authority.find(':');
        if (colon_position == std::string::npos) {
            host = std::move(authority);
        } else {
            host = authority.substr(0, colon_position);
            port = authority.substr(colon_position + 1);
            if (port.empty()) {
                throw std::invalid_argument("HttpClientTransport URL contains empty port");
            }
        }

        if (host.empty()) {
            throw std::invalid_argument("HttpClientTransport URL is missing host");
        }

        return ParsedUrl{std::move(host), std::move(port), std::move(path)};
    }

    Task<void> ensure_connected() {
        if (state->stream.has_value()) {
            co_return;
        }

        auto resolved_endpoints =
            co_await state->resolver.async_resolve(host, port, net::use_awaitable);
        state->stream.emplace(strand);
        state->stream->expires_after(std::chrono::seconds(30));
        co_await state->stream->async_connect(resolved_endpoints, net::use_awaitable);
    }

    static bool starts_with(std::string_view value, std::string_view prefix) {
        return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
    }

    static std::string strip_carriage_return(std::string_view line) {
        if (!line.empty() && line.back() == '\r') {
            return std::string(line.substr(0, line.size() - 1));
        }
        return std::string(line);
    }

    static std::queue<ParsedSseEvent> parse_sse_messages(std::string_view sse_body) {
        std::queue<ParsedSseEvent> parsed_messages;
        std::string current_event_payload;
        std::string current_event_id;

        std::size_t offset = 0;
        while (offset <= sse_body.size()) {
            const std::size_t line_end = sse_body.find('\n', offset);
            const std::size_t extracted_length =
                line_end == std::string_view::npos ? sse_body.size() - offset : line_end - offset;
            const std::string normalized_line =
                strip_carriage_return(sse_body.substr(offset, extracted_length));

            if (normalized_line.empty()) {
                if (!current_event_payload.empty()) {
                    parsed_messages.push(
                        ParsedSseEvent{std::move(current_event_payload), current_event_id});
                    current_event_payload.clear();
                    current_event_id.clear();
                }
            } else if (starts_with(normalized_line, "data:")) {
                std::string_view data_value =
                    std::string_view(normalized_line).substr(std::string_view("data:").size());
                if (!data_value.empty() && data_value.front() == ' ') {
                    data_value.remove_prefix(1);
                }
                if (!current_event_payload.empty()) {
                    current_event_payload.push_back('\n');
                }
                current_event_payload.append(data_value.data(), data_value.size());
            } else if (starts_with(normalized_line, "id:")) {
                std::string_view id_value =
                    std::string_view(normalized_line).substr(std::string_view("id:").size());
                if (!id_value.empty() && id_value.front() == ' ') {
                    id_value.remove_prefix(1);
                }
                current_event_id = std::string(id_value);
            }

            if (line_end == std::string_view::npos) {
                break;
            }
            offset = line_end + 1;
        }

        if (!current_event_payload.empty()) {
            parsed_messages.push(ParsedSseEvent{std::move(current_event_payload), current_event_id});
        }

        return parsed_messages;
    }

    void enqueue_message(std::string message_text) {
        auto shared_state = state;
        net::post(shared_state->timer.get_executor(),
                  [shared_state, queued_message = std::move(message_text)]() mutable {
                      shared_state->queue.push(std::move(queued_message));
                      shared_state->timer.cancel();
                  });
    }

    void capture_session_id(const http::response<http::string_body>& response) {
        auto header_iter = response.find("MCP-Session-Id");
        if (header_iter != response.end()) {
            state->session_id = std::string(header_iter->value());
        }
    }

    void process_response(const http::response<http::string_body>& response) {
        if (response.result() == http::status::accepted) {
            return;
        }

        if (response.result_int() >= 400) {
            throw std::runtime_error("HTTP request failed with status " +
                                     std::to_string(response.result_int()));
        }

        if (response.find(http::field::content_type) == response.end()) {
            return;
        }

        const std::string content_type_header = std::string(response[http::field::content_type]);

        if (starts_with(content_type_header, "application/json")) {
            if (!response.body().empty()) {
                enqueue_message(response.body());
            }
            return;
        }

        if (starts_with(content_type_header, "text/event-stream")) {
            auto parsed_messages = parse_sse_messages(response.body());
            while (!parsed_messages.empty()) {
                auto& event = parsed_messages.front();
                if (!event.id.empty()) {
                    state->last_event_id = event.id;
                }
                enqueue_message(std::move(event.data));
                parsed_messages.pop();
            }
        }
    }

    void reset_connection() {
        if (!state->stream.has_value()) {
            return;
        }

        beast::error_code operation_error;
        state->stream->socket().shutdown(net::ip::tcp::socket::shutdown_both, operation_error);
        state->stream->socket().close(operation_error);
        state->stream.reset();
    }

    net::strand<net::any_io_executor> strand;
    std::shared_ptr<SharedState> state;
    std::string host;
    std::string port;
    std::string path;
};

HttpClientTransport::HttpClientTransport(const net::any_io_executor& executor, std::string url)
    : impl_(std::make_unique<Impl>(executor, std::move(url))) {}

HttpClientTransport::~HttpClientTransport() { close(); }

const std::string& HttpClientTransport::session_id() const { return impl_->state->session_id; }

const std::string& HttpClientTransport::last_event_id() const { return impl_->state->last_event_id; }

Task<std::string> HttpClientTransport::read_message() {
    auto& state = *impl_->state;
    for (;;) {
        if (!state.queue.empty()) {
            auto message_text = std::move(state.queue.front());
            state.queue.pop();
            co_return message_text;
        }

        if (state.closed.load(std::memory_order_acquire)) {
            throw std::runtime_error("HttpClientTransport is closed");
        }

        state.timer.expires_at(std::chrono::steady_clock::time_point::max());
        try {
            co_await state.timer.async_wait(net::use_awaitable);
        } catch (const boost::system::system_error& error) {
            if (error.code() != net::error::operation_aborted) {
                throw;
            }
        }
    }
}

Task<void> HttpClientTransport::write_message(std::string_view message) {
    if (impl_->state->closed.load(std::memory_order_acquire)) {
        throw std::runtime_error("HttpClientTransport is closed");
    }

    co_await net::post(impl_->strand, net::use_awaitable);

    if (impl_->state->closed.load(std::memory_order_acquire)) {
        throw std::runtime_error("HttpClientTransport is closed");
    }

    try {
        co_await impl_->ensure_connected();

        http::request<http::string_body> request{http::verb::post, impl_->path, 11};
        request.set(http::field::host, impl_->host);
        request.set(http::field::content_type, "application/json");
        request.set(http::field::accept, "application/json, text/event-stream");
        request.set("MCP-Protocol-Version", std::string(LATEST_PROTOCOL_VERSION));
        if (!impl_->state->session_id.empty()) {
            request.set("MCP-Session-Id", impl_->state->session_id);
        }
        if (!impl_->state->last_event_id.empty()) {
            request.set("Last-Event-ID", impl_->state->last_event_id);
        }
        request.body() = std::string(message);
        request.prepare_payload();

        impl_->state->stream->expires_after(std::chrono::seconds(30));
        co_await http::async_write(*impl_->state->stream, request, net::use_awaitable);

        beast::flat_buffer response_buffer;
        http::response<http::string_body> response;
        co_await http::async_read(*impl_->state->stream, response_buffer, response, net::use_awaitable);

        impl_->capture_session_id(response);
        impl_->process_response(response);
    } catch (...) {
        impl_->reset_connection();
        throw;
    }
}

void HttpClientTransport::close() {
    if (impl_->state->closed.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    auto shared_state = impl_->state;
    net::post(shared_state->timer.get_executor(), [shared_state]() { shared_state->timer.cancel(); });

    auto active_session_id = shared_state->session_id;
    net::co_spawn(
        impl_->strand,
        [shared_state, active_session_id, host = impl_->host, port = impl_->port,
         path = impl_->path]() -> Task<void> {
            if (!active_session_id.empty()) {
                try {
                    if (!shared_state->stream.has_value()) {
                        auto resolved_endpoints = co_await shared_state->resolver.async_resolve(
                            host, port, net::use_awaitable);

                        shared_state->stream.emplace(shared_state->timer.get_executor());
                        shared_state->stream->expires_after(std::chrono::seconds(30));
                        co_await shared_state->stream->async_connect(resolved_endpoints,
                                                                     net::use_awaitable);
                    }

                    http::request<http::empty_body> delete_request{http::verb::delete_, path, 11};
                    delete_request.set(http::field::host, host);
                    delete_request.set("MCP-Session-Id", active_session_id);
                    delete_request.set("MCP-Protocol-Version", std::string(LATEST_PROTOCOL_VERSION));

                    shared_state->stream->expires_after(std::chrono::seconds(30));
                    co_await http::async_write(*shared_state->stream, delete_request,
                                               net::use_awaitable);

                    beast::flat_buffer delete_response_buffer;
                    http::response<http::string_body> delete_response;
                    co_await http::async_read(*shared_state->stream, delete_response_buffer,
                                              delete_response, net::use_awaitable);
                } catch (...) {
                    // Best-effort DELETE on close; ignore errors if the server is unreachable.
                }
            }

            if (shared_state->stream.has_value()) {
                beast::error_code operation_error;
                shared_state->stream->socket().shutdown(net::ip::tcp::socket::shutdown_both,
                                                        operation_error);
                shared_state->stream->socket().close(operation_error);
                shared_state->stream.reset();
            }
            shared_state->session_id.clear();
            co_return;
        },
        net::detached);
}

}  // namespace mcp

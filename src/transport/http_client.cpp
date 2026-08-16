#include <mcp/core/constants.hpp>
#include <mcp/transport/http_client.hpp>
#include <mcp/transport/http_types.hpp>

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
#include <mutex>
#include <nlohmann/json.hpp>
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
            : timer(strand), operation_timer(strand), resolver(strand) {
            operation_timer.expires_at(std::chrono::steady_clock::time_point::max());
        }

        net::steady_timer timer;
        net::steady_timer operation_timer;
        net::ip::tcp::resolver resolver;
        std::optional<beast::tcp_stream> stream;
        std::queue<std::string> queue;
        std::atomic<bool> closed{false};
        mutable std::mutex metadata_mutex;
        std::string session_id;
        std::string last_event_id;
        std::string protocol_version{std::string(g_LATEST_PROTOCOL_VERSION)};
        std::optional<std::string> initialize_request_key;
        bool operation_active{false};
        bool read_active{false};
    };

    struct ParsedSseEvent {
        std::string data;
        std::string id;
    };

    struct CloseTarget {
        std::string host;
        std::string port;
        std::string path;
        std::function<std::string()> bearer_token_provider;
    };

    Impl(const net::any_io_executor& executor, const std::string& url)
        : strand(net::make_strand(executor)), state(std::make_shared<SharedState>(strand)) {
        auto parsed = parse_url(url);
        host = std::move(parsed.host);
        port = std::move(parsed.port);
        path = std::move(parsed.path);
        state->timer.expires_at(std::chrono::steady_clock::time_point::max());
    }

    static ParsedUrl parse_url(const std::string& url) {
        constexpr std::string_view scheme = "http://";
        if (!url.starts_with(std::string(scheme))) {
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
        if (!state->stream) {
            state->stream.emplace(strand);
        }
        auto& stream = *state->stream;
        stream.expires_after(std::chrono::seconds(constants::g_http_timeout_seconds));
        co_await stream.async_connect(resolved_endpoints, net::use_awaitable);
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

    void enqueue_message(std::string message_text) const {
        state->queue.push(std::move(message_text));
        state->timer.cancel();
    }

    void capture_session_id(const StringResponse& response) {
        auto header_iter = response.find("MCP-Session-Id");
        if (header_iter != response.end()) {
            std::lock_guard lock(state->metadata_mutex);
            state->session_id = std::string(header_iter->value());
        }
    }

    bool capture_initialize_request(std::string_view message) {
        try {
            const auto request = nlohmann::json::parse(message);
            if (request.is_object() && request.value("method", "") == "initialize" &&
                request.contains("id") &&
                (request.at("id").is_string() || request.at("id").is_number_integer())) {
                state->initialize_request_key = request.at("id").get<RequestId>().correlation_key();
                return true;
            }
        } catch (const std::exception&) {
            // The protocol layer reports malformed JSON; the transport only
            // tracks valid initialize envelopes for header negotiation.
        }
        return false;
    }

    void capture_negotiated_protocol_version(std::string_view message) {
        if (!state->initialize_request_key) {
            return;
        }

        try {
            const auto response = nlohmann::json::parse(message);
            if (!response.is_object() || !response.contains("id") ||
                (!response.at("id").is_string() && !response.at("id").is_number_integer()) ||
                response.at("id").get<RequestId>().correlation_key() !=
                    *state->initialize_request_key) {
                return;
            }

            state->initialize_request_key.reset();
            if (!response.contains("result") || !response.at("result").is_object()) {
                return;
            }
            const auto& result = response.at("result");
            if (!result.contains("protocolVersion") || !result.at("protocolVersion").is_string()) {
                return;
            }

            const auto protocol_version = result.at("protocolVersion").get<std::string>();
            if (is_supported_protocol_version(protocol_version)) {
                state->protocol_version = protocol_version;
            }
        } catch (const std::exception&) {
            // The client protocol layer owns response validation.
        }
    }

    void process_response(const StringResponse& response) {
        if (response.result() == http::status::accepted) {
            return;
        }

        if (response.result_int() >= constants::g_http_bad_request) {
            std::string challenge;
            const auto challenge_it = response.find(http::field::www_authenticate);
            if (challenge_it != response.end()) {
                challenge = std::string(challenge_it->value());
            }
            throw HttpStatusError(
                response.result_int(),
                "HTTP request failed with status " + std::to_string(response.result_int()),
                std::move(challenge));
        }

        if (response.find(http::field::content_type) == response.end()) {
            return;
        }

        const std::string content_type_header = std::string(response[http::field::content_type]);

        if (starts_with(content_type_header, "application/json")) {
            if (!response.body().empty()) {
                capture_negotiated_protocol_version(response.body());
                enqueue_message(response.body());
            }
            return;
        }

        if (starts_with(content_type_header, "text/event-stream")) {
            auto parsed_messages = parse_sse_messages(response.body());
            while (!parsed_messages.empty()) {
                auto& event = parsed_messages.front();
                if (!event.id.empty()) {
                    std::lock_guard lock(state->metadata_mutex);
                    state->last_event_id = event.id;
                }
                capture_negotiated_protocol_version(event.data);
                enqueue_message(std::move(event.data));
                parsed_messages.pop();
            }
        }
    }

    void reset_connection() {
        if (!state->stream.has_value()) {
            return;
        }

        if (state->stream) {
            beast::error_code operation_error;
            (void)state->stream->socket().shutdown(net::ip::tcp::socket::shutdown_both,
                                                   operation_error);
            (void)state->stream->socket().close(operation_error);
        }
        state->stream.reset();
    }

    static void complete_operation(const std::shared_ptr<SharedState>& shared_state) {
        shared_state->operation_active = false;
        boost::system::error_code ignored;
        shared_state->operation_timer.cancel(ignored);
    }

    static Task<std::string> run_read(std::shared_ptr<Impl> impl) {
        auto& state = *impl->state;
        if (state.read_active) {
            throw std::logic_error("HttpClientTransport supports one pending read");
        }
        state.read_active = true;
        try {
            for (;;) {
                if (state.closed.load(std::memory_order_acquire)) {
                    throw std::runtime_error("HttpClientTransport is closed");
                }

                if (!state.queue.empty()) {
                    auto message_text = std::move(state.queue.front());
                    state.queue.pop();
                    state.read_active = false;
                    co_return message_text;
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
        } catch (...) {
            state.read_active = false;
            throw;
        }
    }

    static Task<void> run_write(std::shared_ptr<Impl> impl, std::string message) {
        auto& state = *impl->state;
        if (state.closed.load(std::memory_order_acquire)) {
            throw std::runtime_error("HttpClientTransport is closed");
        }

        while (state.operation_active) {
            try {
                co_await state.operation_timer.async_wait(net::use_awaitable);
            } catch (const boost::system::system_error& error) {
                if (error.code() != net::error::operation_aborted) {
                    throw;
                }
            }
            if (state.closed.load(std::memory_order_acquire)) {
                throw std::runtime_error("HttpClientTransport is closed");
            }
        }

        state.operation_timer.expires_at(std::chrono::steady_clock::time_point::max());
        state.operation_active = true;
        const bool initialize_operation = impl->capture_initialize_request(message);

        try {
            co_await impl->ensure_connected();

            StringRequest request{http::verb::post, impl->path, constants::g_http_version_11};
            request.set(http::field::host, impl->host);
            request.set(http::field::content_type, "application/json");
            request.set(http::field::accept, "application/json, text/event-stream");
            request.set("MCP-Protocol-Version", state.protocol_version);
            if (impl->bearer_token_provider) {
                const auto token = impl->bearer_token_provider();
                if (!token.empty()) {
                    request.set(http::field::authorization, "Bearer " + token);
                }
            }
            if (!state.session_id.empty()) {
                request.set("MCP-Session-Id", state.session_id);
            }
            if (!state.last_event_id.empty()) {
                request.set("Last-Event-ID", state.last_event_id);
            }
            request.body() = std::move(message);
            request.prepare_payload();

            if (!state.stream) {
                throw std::runtime_error("HttpClientTransport stream is not initialized");
            }
            auto& stream = *state.stream;
            stream.expires_after(std::chrono::seconds(constants::g_http_timeout_seconds));
            co_await http::async_write(stream, request, net::use_awaitable);

            beast::flat_buffer response_buffer;
            StringResponse response;
            co_await http::async_read(stream, response_buffer, response, net::use_awaitable);

            impl->capture_session_id(response);
            impl->process_response(response);
            if (initialize_operation) {
                state.initialize_request_key.reset();
            }
            complete_operation(impl->state);
        } catch (...) {
            if (initialize_operation) {
                state.initialize_request_key.reset();
            }
            impl->reset_connection();
            complete_operation(impl->state);
            throw;
        }
    }

    net::strand<net::any_io_executor> strand;
    std::shared_ptr<SharedState> state;
    std::string host;
    std::string port;
    std::string path;
    std::function<std::string()> bearer_token_provider;
};

HttpClientTransport::HttpClientTransport(const net::any_io_executor& executor, const std::string& url)
    : impl_(std::make_shared<Impl>(executor, url)) {}

HttpClientTransport::~HttpClientTransport() {
    try {
        close();
    } catch (const std::exception& e) {
        // Destructor must not throw; swallow any exception from close().
        (void)e;
    }
}

std::string HttpClientTransport::session_id() const {
    std::lock_guard lock(impl_->state->metadata_mutex);
    return impl_->state->session_id;
}

std::string HttpClientTransport::last_event_id() const {
    std::lock_guard lock(impl_->state->metadata_mutex);
    return impl_->state->last_event_id;
}

void HttpClientTransport::set_bearer_token_provider(std::function<std::string()> provider) {
    impl_->bearer_token_provider = std::move(provider);
}

Task<std::string> HttpClientTransport::read_message() {
    auto impl = impl_;
    return net::co_spawn(impl->strand, Impl::run_read(impl), net::use_awaitable);
}

Task<void> HttpClientTransport::write_message(std::string_view message) {
    auto impl = impl_;
    return net::co_spawn(impl->strand, Impl::run_write(impl, std::string(message)), net::use_awaitable);
}

void HttpClientTransport::close() {
    if (impl_->state->closed.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    auto shared_state = impl_->state;
    net::post(shared_state->timer.get_executor(), [shared_state]() { shared_state->timer.cancel(); });

    auto close_target = std::make_shared<Impl::CloseTarget>(
        Impl::CloseTarget{impl_->host, impl_->port, impl_->path, impl_->bearer_token_provider});
    net::co_spawn(
        impl_->strand,
        [shared_state, close_target = std::move(close_target)]() -> Task<void> {
            if (shared_state->operation_active) {
                shared_state->resolver.cancel();
                if (shared_state->stream) {
                    beast::error_code ignored;
                    shared_state->stream->socket().cancel(ignored);
                }
                while (shared_state->operation_active) {
                    try {
                        co_await shared_state->operation_timer.async_wait(net::use_awaitable);
                    } catch (const boost::system::system_error& error) {
                        if (error.code() != net::error::operation_aborted) {
                            co_return;
                        }
                    }
                }
            }

            auto active_session_id = std::make_shared<const std::string>(shared_state->session_id);
            auto active_protocol_version =
                std::make_shared<const std::string>(shared_state->protocol_version);
            if (!active_session_id->empty()) {
                try {
                    if (!shared_state->stream.has_value()) {
                        auto resolved_endpoints = co_await shared_state->resolver.async_resolve(
                            close_target->host, close_target->port, net::use_awaitable);

                        shared_state->stream.emplace(shared_state->timer.get_executor());
                        shared_state->stream->expires_after(
                            std::chrono::seconds(constants::g_http_timeout_seconds));
                        co_await shared_state->stream->async_connect(resolved_endpoints,
                                                                     net::use_awaitable);
                    }

                    http::request<http::empty_body> delete_request{
                        http::verb::delete_, close_target->path, constants::g_http_version_11};
                    delete_request.set(http::field::host, close_target->host);
                    delete_request.set("MCP-Session-Id", *active_session_id);
                    delete_request.set("MCP-Protocol-Version", *active_protocol_version);
                    if (close_target->bearer_token_provider) {
                        const auto token = close_target->bearer_token_provider();
                        if (!token.empty()) {
                            delete_request.set(http::field::authorization, "Bearer " + token);
                        }
                    }

                    shared_state->stream->expires_after(
                        std::chrono::seconds(constants::g_http_timeout_seconds));
                    co_await http::async_write(*shared_state->stream, delete_request,
                                               net::use_awaitable);

                    beast::flat_buffer delete_response_buffer;
                    StringResponse delete_response;
                    co_await http::async_read(*shared_state->stream, delete_response_buffer,
                                              delete_response, net::use_awaitable);
                } catch (const std::exception& e) {
                    // Best-effort DELETE on close; ignore errors if the server is unreachable.
                    (void)e;
                }
            }

            if (shared_state->stream.has_value()) {
                beast::error_code operation_error;
                shared_state->stream->socket().shutdown(net::ip::tcp::socket::shutdown_both,
                                                        operation_error);
                shared_state->stream->socket().close(operation_error);
                shared_state->stream.reset();
            }
            {
                std::lock_guard lock(shared_state->metadata_mutex);
                shared_state->session_id.clear();
            }
            co_return;
        },
        net::detached);
}

}  // namespace mcp

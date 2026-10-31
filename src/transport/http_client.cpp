#include "../detail/header_value.hpp"
#include "../detail/http_request_metadata.hpp"
#include "../detail/param_headers.hpp"
#include "../detail/peer_json.hpp"
#include "../detail/request_meta.hpp"
#include "../detail/socket_deadline.hpp"

#include <mcp/core/constants.hpp>
#include <mcp/transport/http_client.hpp>
#include <mcp/transport/http_types.hpp>

#include <array>
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
#include <cstdint>
#include <exception>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <queue>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mcp {

namespace beast = boost::beast;
namespace http = boost::beast::http;
namespace net = boost::asio;

namespace {

/// The most a single response body or SSE line may hold before the exchange is abandoned.
constexpr std::size_t g_max_modern_message_bytes = 16U * 1024U * 1024U;
/// The most pages of tools/list one schema refresh follows.
constexpr int g_max_refresh_pages = 64;

/// Reads a text/event-stream body in the pieces the network delivers it and returns the data of each
/// event as soon as its terminating blank line arrives. Comment lines and fields other than `data`
/// carry nothing the client needs and are skipped.
class SseDecoder {
   public:
    std::vector<std::string> feed(std::string_view chunk) {
        std::vector<std::string> events;
        for (const char symbol : chunk) {
            if (symbol != '\n') {
                line_.push_back(symbol);
                if (line_.size() > g_max_modern_message_bytes) {
                    throw std::runtime_error("An SSE line from the server is too long");
                }
                continue;
            }
            consume_line(events);
        }
        return events;
    }

    /// The event a stream that ends without a final blank line still carries.
    std::vector<std::string> finish() {
        std::vector<std::string> events;
        if (!line_.empty()) {
            consume_line(events);
        }
        if (!payload_.empty()) {
            events.push_back(std::move(payload_));
            payload_.clear();
        }
        return events;
    }

   private:
    void consume_line(std::vector<std::string>& events) {
        if (!line_.empty() && line_.back() == '\r') {
            line_.pop_back();
        }
        if (line_.empty()) {
            if (!payload_.empty()) {
                events.push_back(std::move(payload_));
                payload_.clear();
            }
        } else if (line_.compare(0, 5, "data:") == 0) {
            std::string_view value = std::string_view(line_).substr(5);
            if (!value.empty() && value.front() == ' ') {
                value.remove_prefix(1);
            }
            if (!payload_.empty()) {
                payload_.push_back('\n');
            }
            payload_.append(value.data(), value.size());
        }
        line_.clear();
    }

    std::string line_;
    std::string payload_;
};

/// One request or notification sent the way protocol revision 2026-07-28 sends it: its own HTTP
/// POST on its own connection, with the headers that mirror the body.
struct ModernCall {
    std::string body;
    nlohmann::json message;
    std::string method;
    std::string version;
    /// Correlation key of the request; empty for a notification.
    std::string id_key;
    /// For a call made on behalf of another (a schema refresh): that call's key.
    std::string owner_key;
    nlohmann::json id;
    std::function<std::string()> bearer_token_provider;
    /// Whether notifications and the final response are handed to the reader. A schema refresh
    /// keeps its answers to itself.
    bool forward{true};
    bool expects_response{true};
    bool cancelled{false};
    std::uint64_t serial{0};

    // The current attempt.
    std::shared_ptr<beast::tcp_stream> stream;
    std::optional<detail::SocketDeadline> deadline;
    std::optional<std::string> final_message;
    std::string content_type;
    std::string challenge;
    // Held here, not in the coroutine's frame: GCC 11 mishandles a std::string that lives across
    // a suspension point there.
    SseDecoder decoder;
    std::string response_body;
    beast::flat_buffer buffer;
    std::unique_ptr<http::response_parser<http::buffer_body>> parser;
    unsigned int status{0};
    bool event_stream{false};
};

}  // namespace

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
        std::string protocol_version{std::string(g_LATEST_LEGACY_PROTOCOL_VERSION)};
        std::optional<std::string> initialize_request_key;
        bool operation_active{false};
        bool read_active{false};
        /// The protocol version of the 2026-07-28 requests this transport carries, once one was
        /// answered; empty for a legacy session. Touched on the strand only.
        std::string modern_version;
        /// Requests being exchanged without a session, by serial. Touched on the strand only.
        std::map<std::uint64_t, std::shared_ptr<ModernCall>> modern_calls;
        std::uint64_t next_modern_serial{0};
        /// The `x-mcp-header` annotations of the tools tools/list returned, by tool name.
        std::map<std::string, std::vector<detail::ParamHeader>> tool_params;

        /// Ends the exchange of the call that @p key names, and of the calls made for it.
        void cancel_modern_call(const std::string& key) {
            for (auto& entry : modern_calls) {
                auto& call = *entry.second;
                if (call.id_key == key || call.owner_key == key) {
                    call.cancelled = true;
                    close_call_socket(call);
                }
            }
        }

        static void close_call_socket(ModernCall& call) {
            if (call.stream) {
                beast::error_code ignored;
                (void)call.stream->socket().close(ignored);
            }
        }
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
        // A close() that ran while the resolve could no longer be cancelled found no socket to
        // cancel either. From here to the socket opening inside async_connect() nothing suspends,
        // so a later close() runs on the strand after it and closes that socket.
        if (state->closed.load(std::memory_order_acquire)) {
            throw std::runtime_error("HttpClientTransport is closed");
        }
        if (!state->stream) {
            state->stream.emplace(strand);
        }
        auto& stream = *state->stream;
        const detail::SocketDeadline deadline(stream.get_executor(), stream.socket(),
                                              std::chrono::seconds(constants::g_http_timeout_seconds));
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
                // The headers of the initialize POST and of what follows it name the version the
                // client asks for, until the answer names the one the server chose.
                const auto params = request.find("params");
                if (params != request.end() && params->is_object() &&
                    params->contains("protocolVersion") && params->at("protocolVersion").is_string()) {
                    const auto requested = params->at("protocolVersion").get<std::string>();
                    if (is_legacy_protocol_version(requested)) {
                        state->protocol_version = requested;
                    }
                }
                state->modern_version.clear();
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
            const auto response = detail::parse_peer_json(message);
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
            if (is_legacy_protocol_version(protocol_version)) {
                state->protocol_version = protocol_version;
            }
        } catch (const std::exception&) {
            // The client protocol layer owns response validation.
        }
    }

    /// The JSON-RPC error a 400 or 404 answer carries for a request @p sent outside any session,
    /// with the request's id filled in when the server left it out, or nullopt when the answer is
    /// anything else. Such a request is an initialize, so the error explains why there is no
    /// session.
    static std::optional<std::string> sessionless_error_body(const StringResponse& response,
                                                             const std::string& sent) {
        const auto status = response.result();
        const auto content_type = response.find(http::field::content_type);
        if ((status != http::status::bad_request && status != http::status::not_found) ||
            content_type == response.end() ||
            !starts_with(std::string(content_type->value()), "application/json")) {
            return std::nullopt;
        }
        auto parsed = detail::parse_peer_json(response.body());
        const auto request = detail::parse_peer_json(sent);
        if (!parsed.is_object() || !request.is_object() || !request.contains("id")) {
            return std::nullopt;
        }
        const auto error = parsed.find("error");
        if (error == parsed.end() || !error->is_object() || !error->contains("code") ||
            !error->at("code").is_number_integer()) {
            return std::nullopt;
        }
        if (!parsed.contains("id") || parsed.at("id").is_null()) {
            parsed["id"] = request.at("id");
        }
        if (parsed.at("id") != request.at("id")) {
            return std::nullopt;
        }
        parsed["jsonrpc"] = "2.0";
        return parsed.dump();
    }

    void process_response(const StringResponse& response, const std::string& sent, bool in_session) {
        if (response.result() == http::status::accepted) {
            return;
        }

        if (response.result_int() >= constants::g_http_bad_request) {
            if (!in_session) {
                if (auto error_body = sessionless_error_body(response, sent)) {
                    enqueue_message(std::move(*error_body));
                    return;
                }
            }
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
        shared_state->operation_timer.cancel();
    }

    static Task<std::string> run_read(std::shared_ptr<Impl> impl) {
        auto& state = *impl->state;
        if (state.read_active) {
            throw std::logic_error("HttpClientTransport supports one pending read");
        }
        state.read_active = true;
        try {
            for (;;) {
                if (!state.queue.empty()) {
                    auto message_text = std::move(state.queue.front());
                    state.queue.pop();
                    state.read_active = false;
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
        } catch (...) {
            state.read_active = false;
            throw;
        }
    }

    /// Read the provider once, whole, under the mutex that guards it. Callers pin it at the call
    /// that starts a request -- write_message() and close(), both on the application thread -- so
    /// no code path on the strand reads the member itself afterwards.
    std::function<std::string()> pin_bearer_token_provider() const {
        std::lock_guard lock(provider_mutex);
        return bearer_token_provider;
    }

    static Task<void> run_write(std::shared_ptr<Impl> impl, std::string message,
                                std::function<std::string()> bearer_token_provider) {
        auto& state = *impl->state;
        if (state.closed.load(std::memory_order_acquire)) {
            throw std::runtime_error("HttpClientTransport is closed");
        }

        bool handled = false;
        auto modern_call = impl->route_modern(message, bearer_token_provider, handled);
        if (handled) {
            co_return;
        }
        if (modern_call) {
            co_await run_modern(impl, std::move(modern_call));
            co_return;
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

        std::optional<detail::SocketDeadline> deadline;
        try {
            co_await impl->ensure_connected();

            StringRequest request{http::verb::post, impl->path, constants::g_http_version_11};
            request.set(http::field::host, impl->host);
            request.set(http::field::content_type, "application/json");
            request.set(http::field::accept, "application/json, text/event-stream");
            request.set("MCP-Protocol-Version", state.protocol_version);
            if (bearer_token_provider) {
                const auto token = bearer_token_provider();
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
            deadline.emplace(stream.get_executor(), stream.socket(),
                             std::chrono::seconds(constants::g_http_timeout_seconds));
            co_await http::async_write(stream, request, net::use_awaitable);

            beast::flat_buffer response_buffer;
            StringResponse response;
            co_await http::async_read(stream, response_buffer, response, net::use_awaitable);
            deadline.reset();
            if (!response.keep_alive()) {
                // The server closes the connection after this response (RFC 9112 section 9.6), so
                // the next request has to open a new one.
                impl->reset_connection();
            }

            impl->capture_session_id(response);
            impl->process_response(response, request.body(), request.count("MCP-Session-Id") != 0);
            if (initialize_operation) {
                state.initialize_request_key.reset();
            }
            complete_operation(impl->state);
        } catch (...) {
            const bool timed_out = deadline && deadline->expired();
            deadline.reset();
            if (initialize_operation) {
                state.initialize_request_key.reset();
            }
            impl->reset_connection();
            complete_operation(impl->state);
            // close() closes the socket under the write, and what the socket layer then reports
            // depends on the platform and on which operation the close landed in.
            if (state.closed.load(std::memory_order_acquire)) {
                throw boost::system::system_error(net::error::operation_aborted);
            }
            if (timed_out) {
                throw boost::system::system_error(beast::error::timeout);
            }
            throw;
        }
    }

    void warn(const std::string& text) const {
        std::function<void(const std::string&)> handler;
        {
            std::lock_guard lock(provider_mutex);
            handler = warning_handler;
        }
        if (handler) {
            try {
                handler(text);
            } catch (const std::exception&) {
                // A warning that cannot be reported is not worth failing a request over.
            }
        }
    }

    /// Decides how @p message is sent. A request whose `_meta` names a protocol version is built
    /// into the call that is returned. A notification travels as one as well once a 2026-07-28
    /// request has been answered, except notifications/cancelled, which ends the response stream
    /// of the request it names instead (@p handled is then set).
    std::shared_ptr<ModernCall> route_modern(const std::string& message,
                                             const std::function<std::string()>& bearer_provider,
                                             bool& handled) {
        auto parsed = nlohmann::json::parse(message, nullptr, false);
        if (!parsed.is_object()) {
            return nullptr;
        }
        const auto method_it = parsed.find("method");
        if (method_it == parsed.end() || !method_it->is_string()) {
            return nullptr;
        }
        const auto& method = method_it->get_ref<const std::string&>();
        const auto id_it = parsed.find("id");
        const bool is_request = id_it != parsed.end();

        auto call = std::make_shared<ModernCall>();
        call->method = method;
        call->bearer_token_provider = bearer_provider;
        if (is_request) {
            if (!detail::is_modern_request(parsed, method)) {
                return nullptr;
            }
            const auto* version = detail::requested_protocol_version(parsed);
            if (version == nullptr || (!id_it->is_string() && !id_it->is_number_integer())) {
                return nullptr;
            }
            call->version = *version;
            call->id = *id_it;
            call->id_key = id_it->get<RequestId>().correlation_key();
        } else {
            if (state->modern_version.empty()) {
                return nullptr;
            }
            if (method == "notifications/cancelled") {
                // Closing the response stream is the cancellation; no notification is sent.
                handled = true;
                try {
                    const auto params = parsed.find("params");
                    if (params != parsed.end() && params->is_object() &&
                        params->contains("requestId")) {
                        state->cancel_modern_call(
                            params->at("requestId").get<RequestId>().correlation_key());
                    }
                } catch (const std::exception&) {
                    // A cancellation that names no usable id cancels nothing.
                }
                return nullptr;
            }
            call->version = state->modern_version;
            call->expects_response = false;
        }
        call->body = message;
        call->message = std::move(parsed);
        return call;
    }

    /// The text an `Mcp-Param-*` header carries for @p value, or nullopt when the value is not one a
    /// header can carry.
    static std::optional<std::string> param_header_text(const nlohmann::json& value) {
        if (value.is_string()) {
            return value.get<std::string>();
        }
        if (value.is_boolean()) {
            return value.get<bool>() ? "true" : "false";
        }
        if (value.is_number_unsigned()) {
            const auto number = value.get<std::uint64_t>();
            if (number > static_cast<std::uint64_t>(detail::g_MAX_SAFE_HEADER_INTEGER)) {
                return std::nullopt;
            }
            return std::to_string(number);
        }
        if (value.is_number_integer()) {
            const auto number = value.get<std::int64_t>();
            if (number < -detail::g_MAX_SAFE_HEADER_INTEGER) {
                return std::nullopt;
            }
            return std::to_string(number);
        }
        return std::nullopt;
    }

    /// The `Mcp-Name` source of @p call: params.name for tools/call and prompts/get, params.uri for
    /// resources/read.
    static const nlohmann::json* mirrored_name(const ModernCall& call) {
        const char* member = nullptr;
        if (call.method == "tools/call" || call.method == "prompts/get") {
            member = "name";
        } else if (call.method == "resources/read") {
            member = "uri";
        } else {
            return nullptr;
        }
        const auto params = call.message.find("params");
        if (params == call.message.end() || !params->is_object()) {
            return nullptr;
        }
        const auto value = params->find(member);
        return value != params->end() && value->is_string() ? &*value : nullptr;
    }

    void apply_modern_headers(StringRequest& request, const ModernCall& call) const {
        request.set("MCP-Protocol-Version", call.version);
        request.set("Mcp-Method", call.method);
        const auto* name = mirrored_name(call);
        if (name == nullptr) {
            return;
        }
        request.set("Mcp-Name", detail::encode_header_value(name->get_ref<const std::string&>()));
        if (call.method != "tools/call") {
            return;
        }
        const auto known = state->tool_params.find(name->get_ref<const std::string&>());
        const auto& params = call.message.at("params");
        const auto arguments = params.find("arguments");
        if (known == state->tool_params.end() || arguments == params.end()) {
            return;
        }
        for (const auto& param : known->second) {
            const auto* value = detail::param_headers::argument_at(*arguments, param.path);
            if (value == nullptr) {
                continue;
            }
            const auto text = param_header_text(*value);
            if (text) {
                request.set("Mcp-Param-" + param.name, detail::encode_header_value(*text));
            }
        }
    }

    /// Drops the tools whose `x-mcp-header` annotations the specification forbids from a tools/list
    /// answer and remembers the annotations of the rest. A page without a cursor starts the cache
    /// over. Returns @p text, or the text without the excluded tools.
    std::string track_tools(const ModernCall& call, std::string text) {
        auto answer = nlohmann::json::parse(text, nullptr, false);
        if (!answer.is_object()) {
            return text;
        }
        const auto result = answer.find("result");
        if (result == answer.end() || !result->is_object()) {
            return text;
        }
        const auto tools = result->find("tools");
        if (tools == result->end() || !tools->is_array()) {
            return text;
        }
        const auto params = call.message.find("params");
        const bool first_page =
            params == call.message.end() || !params->is_object() || !params->contains("cursor");
        if (first_page) {
            state->tool_params.clear();
        }

        auto kept = nlohmann::json::array();
        bool excluded = false;
        for (const auto& tool : *tools) {
            const auto name = tool.is_object() ? tool.find("name") : tool.end();
            if (!tool.is_object() || name == tool.end() || !name->is_string()) {
                kept.push_back(tool);
                continue;
            }
            const auto& tool_name = name->get_ref<const std::string&>();
            try {
                const auto schema = tool.find("inputSchema");
                state->tool_params[tool_name] = schema != tool.end() && schema->is_object()
                                                    ? detail::collect_param_headers(*schema)
                                                    : std::vector<detail::ParamHeader>{};
                kept.push_back(tool);
            } catch (const std::invalid_argument& error) {
                state->tool_params.erase(tool_name);
                excluded = true;
                warn("Excluded tool \"" + tool_name + "\" from tools/list: " + error.what());
            }
        }
        if (!excluded) {
            return text;
        }
        (*result)["tools"] = std::move(kept);
        return answer.dump();
    }

    static bool starts_with_ignoring_case(std::string_view value, std::string_view prefix) {
        return value.size() >= prefix.size() &&
               detail::equals_ignoring_case(value.substr(0, prefix.size()), prefix);
    }

    /// Whether a message of the stream is the response to the request, rather than a notification.
    static bool is_response(const std::string& text) {
        const auto parsed = nlohmann::json::parse(text, nullptr, false);
        return parsed.is_object() && parsed.contains("id") && !parsed.contains("method");
    }

    /// Takes one message off the stream: a response is kept as the answer, anything else goes to the
    /// reader at once.
    void accept_stream_message(ModernCall& call, std::string text) const {
        if (call.expects_response && !call.final_message && is_response(text)) {
            call.final_message = std::move(text);
        } else if (call.forward) {
            enqueue_message(std::move(text));
        }
    }

    /// A JSON-RPC error body an HTTP error status carries, with the request's id filled in when the
    /// server left it out, or nullopt when the body is anything else.
    static std::optional<std::string> jsonrpc_error_body(const ModernCall& call,
                                                         const std::string& body) {
        auto parsed = nlohmann::json::parse(body, nullptr, false);
        if (!parsed.is_object()) {
            return std::nullopt;
        }
        const auto error = parsed.find("error");
        if (error == parsed.end() || !error->is_object()) {
            return std::nullopt;
        }
        const auto code = error->find("code");
        if (code == error->end() || !code->is_number_integer()) {
            return std::nullopt;
        }
        const auto id = parsed.find("id");
        if (id == parsed.end() || id->is_null()) {
            parsed["id"] = call.id;
        }
        if (!parsed.contains("jsonrpc")) {
            parsed["jsonrpc"] = "2.0";
        }
        return parsed.dump();
    }

    static void throw_if_unavailable(const SharedState& shared, const ModernCall& call) {
        if (shared.closed.load(std::memory_order_acquire) || call.cancelled) {
            throw boost::system::system_error(net::error::operation_aborted);
        }
    }

    /// Sends @p call and reads the head of the answer: the status and headers.
    static Task<void> modern_open(std::shared_ptr<Impl> impl, std::shared_ptr<ModernCall> call) {
        auto& shared = *impl->state;
        call->stream.reset();
        call->final_message.reset();
        call->decoder = SseDecoder();
        call->content_type.clear();
        call->challenge.clear();
        call->response_body.clear();
        throw_if_unavailable(shared, *call);

        net::ip::tcp::resolver resolver(impl->strand);
        auto endpoints = co_await resolver.async_resolve(impl->host, impl->port, net::use_awaitable);
        throw_if_unavailable(shared, *call);
        call->stream = std::make_shared<beast::tcp_stream>(impl->strand);
        const auto stream = call->stream;

        call->deadline.emplace(stream->get_executor(), stream->socket(),
                               std::chrono::seconds(constants::g_http_timeout_seconds));
        co_await stream->async_connect(endpoints, net::use_awaitable);

        StringRequest request{http::verb::post, impl->path, constants::g_http_version_11};
        request.set(http::field::host, impl->host);
        request.set(http::field::content_type, "application/json");
        request.set(http::field::accept, "application/json, text/event-stream");
        if (call->bearer_token_provider) {
            const auto token = call->bearer_token_provider();
            if (!token.empty()) {
                request.set(http::field::authorization, "Bearer " + token);
            }
        }
        impl->apply_modern_headers(request, *call);
        request.body() = call->body;
        request.prepare_payload();
        co_await http::async_write(*stream, request, net::use_awaitable);

        call->buffer.clear();
        call->parser = std::make_unique<http::response_parser<http::buffer_body>>();
        // No limit: an event stream is as long as the request runs. Not boost::none, which Boost
        // 1.74 compares as smaller than any Content-Length and so refuses every sized body.
        call->parser->body_limit(std::numeric_limits<std::uint64_t>::max());
        co_await http::async_read_header(*stream, call->buffer, *call->parser, net::use_awaitable);
        call->deadline.reset();
        auto& head = call->parser->get();
        call->status = head.result_int();
        const auto content_type = head.find(http::field::content_type);
        if (content_type != head.end()) {
            call->content_type = std::string(content_type->value());
        }
        const auto challenge = head.find(http::field::www_authenticate);
        if (challenge != head.end()) {
            call->challenge = std::string(challenge->value());
        }
        call->event_stream = call->status < constants::g_http_bad_request &&
                             starts_with(call->content_type, "text/event-stream");
    }

    /// Reads the rest of the answer @p call opened. Notifications of an event stream are handed to
    /// the reader as they arrive; the response is left in `call.final_message`.
    static Task<void> modern_read(std::shared_ptr<Impl> impl, std::shared_ptr<ModernCall> call) {
        const auto stream = call->stream;
        auto& parser = *call->parser;
        auto& decoder = call->decoder;
        auto& body = call->response_body;
        const bool event_stream = call->event_stream;
        const unsigned int status = call->status;
        std::array<char, 8192> chunk{};
        while (!parser.is_done() && !(event_stream && call->final_message)) {
            call->deadline.reset();
            call->deadline.emplace(stream->get_executor(), stream->socket(),
                                   std::chrono::seconds(constants::g_http_timeout_seconds));
            parser.get().body().data = chunk.data();
            parser.get().body().size = chunk.size();
            try {
                co_await http::async_read_some(*stream, call->buffer, parser, net::use_awaitable);
            } catch (const boost::system::system_error& error) {
                if (error.code() != http::error::need_buffer) {
                    throw;
                }
            }
            const std::size_t received = chunk.size() - parser.get().body().size;
            if (received == 0) {
                continue;
            }
            const std::string_view piece(chunk.data(), received);
            if (event_stream) {
                for (auto& text : decoder.feed(piece)) {
                    impl->accept_stream_message(*call, std::move(text));
                }
            } else {
                body.append(piece);
                if (body.size() > g_max_modern_message_bytes) {
                    throw std::runtime_error("The server's response is too large");
                }
            }
        }
        call->deadline.reset();

        if (event_stream) {
            if (!call->final_message) {
                for (auto& text : decoder.finish()) {
                    impl->accept_stream_message(*call, std::move(text));
                }
            }
            if (call->expects_response && !call->final_message) {
                throw std::runtime_error("The server's event stream ended without a response");
            }
            co_return;
        }

        if (status >= constants::g_http_bad_request) {
            const bool may_carry_error =
                call->expects_response && (status == 400 || status == 404 || status == 405);
            if (may_carry_error) {
                auto error_body = jsonrpc_error_body(*call, body);
                if (error_body) {
                    call->final_message = std::move(error_body);
                    co_return;
                }
            }
            throw HttpStatusError(status, "HTTP request failed with status " + std::to_string(status),
                                  call->challenge);
        }
        if (status == static_cast<unsigned int>(http::status::accepted) || body.empty()) {
            if (call->expects_response) {
                throw std::runtime_error("The server answered a request without a response");
            }
            co_return;
        }
        if (!starts_with(call->content_type, "application/json")) {
            throw std::runtime_error("The server answered with an unexpected content type");
        }
        call->final_message = std::move(body);
    }

    static bool is_header_mismatch(const ModernCall& call) {
        if (!call.final_message) {
            return false;
        }
        const auto code = detail::response_error_code(*call.final_message);
        return code && *code == g_HEADER_MISMATCH;
    }

    /// Asks the server for its tools again so the `x-mcp-header` annotations the next attempt mirrors
    /// are current.
    static Task<void> refresh_tool_schemas(std::shared_ptr<Impl> impl,
                                           std::shared_ptr<ModernCall> origin) {
        const auto cursor = std::make_shared<std::optional<std::string>>();
        for (int page = 0; page < g_max_refresh_pages; ++page) {
            auto refresh = std::make_shared<ModernCall>();
            refresh->method = "tools/list";
            refresh->version = origin->version;
            refresh->owner_key = origin->id_key;
            refresh->forward = false;
            refresh->bearer_token_provider = origin->bearer_token_provider;
            refresh->id = "mcp-tools-refresh-" + std::to_string(page);
            nlohmann::json params = nlohmann::json::object();
            const auto& origin_params = origin->message.at("params");
            const auto meta = origin_params.find("_meta");
            if (meta != origin_params.end()) {
                params["_meta"] = *meta;
            }
            if (*cursor) {
                params["cursor"] = **cursor;
            }
            refresh->message = nlohmann::json{
                {"jsonrpc", "2.0"}, {"id", refresh->id}, {"method", "tools/list"}, {"params", params}};
            refresh->body = refresh->message.dump();
            co_await run_exchange(impl, refresh, false);
            if (!refresh->final_message) {
                co_return;
            }
            const auto answer = nlohmann::json::parse(*refresh->final_message, nullptr, false);
            impl->track_tools(*refresh, *refresh->final_message);
            if (!answer.is_object() || !answer.contains("result") || !answer.at("result").is_object()) {
                co_return;
            }
            const auto next = answer.at("result").find("nextCursor");
            if (next == answer.at("result").end() || !next->is_string() ||
                next->get_ref<const std::string&>().empty()) {
                co_return;
            }
            *cursor = next->get<std::string>();
        }
    }

    /// Ends an exchange: nothing of it stays open or registered.
    static void end_call(SharedState& shared, ModernCall& call) {
        call.deadline.reset();
        SharedState::close_call_socket(call);
        call.stream.reset();
        call.parser.reset();
        shared.modern_calls.erase(call.serial);
    }

    /// Hands the reader what @p call was answered with.
    void deliver_final(const ModernCall& call) {
        if (!call.final_message) {
            return;
        }
        auto text = *call.final_message;
        if (call.method == "tools/list") {
            text = track_tools(call, std::move(text));
        }
        enqueue_message(std::move(text));
    }

    static std::string describe(const std::exception_ptr& failure) {
        try {
            std::rethrow_exception(failure);
        } catch (const std::exception& error) {
            return error.what();
        } catch (...) {
            return "unknown error";
        }
    }

    static std::string error_response(const ModernCall& call, int code, const std::string& message) {
        return nlohmann::json{
            {"jsonrpc", "2.0"}, {"id", call.id}, {"error", {{"code", code}, {"message", message}}}}
            .dump();
    }

    /// Reads an event stream to its end after the request that opened it has been sent. Whatever
    /// goes wrong is reported to the reader as a JSON-RPC error for the request, because nobody is
    /// waiting on the write any more.
    static Task<void> stream_to_end(std::shared_ptr<Impl> impl, std::shared_ptr<ModernCall> call) {
        auto& shared = *impl->state;
        std::exception_ptr failure;
        try {
            co_await modern_read(impl, call);
        } catch (...) {
            failure = std::current_exception();
        }
        const bool timed_out = call->deadline && call->deadline->expired();
        end_call(shared, *call);
        if (shared.closed.load(std::memory_order_acquire) || !call->forward) {
            co_return;
        }
        if (call->cancelled) {
            impl->enqueue_message(error_response(*call, g_REQUEST_CANCELLED, "Request cancelled"));
        } else if (failure) {
            impl->enqueue_message(
                error_response(*call, g_CONNECTION_CLOSED,
                               timed_out ? "The response stream timed out" : describe(failure)));
        } else {
            impl->deliver_final(*call);
        }
    }

    /// Sends @p call and reads the answer: all of it, or when it is an event stream (and
    /// @p may_stream) only up to the start of the stream, which is then read in the background.
    /// Returns whether a background read took over.
    static Task<bool> run_exchange(std::shared_ptr<Impl> impl, std::shared_ptr<ModernCall> call,
                                   bool may_stream) {
        auto& shared = *impl->state;
        call->serial = ++shared.next_modern_serial;
        shared.modern_calls.emplace(call->serial, call);
        std::exception_ptr failure;
        bool streaming = false;
        try {
            co_await modern_open(impl, call);
            streaming = call->event_stream && may_stream;
            if (!streaming) {
                co_await modern_read(impl, call);
            }
        } catch (...) {
            failure = std::current_exception();
        }
        if (!failure && streaming) {
            shared.modern_version = call->version;
            net::co_spawn(impl->strand, stream_to_end(impl, call), net::detached);
            co_return true;
        }
        const bool timed_out = call->deadline && call->deadline->expired();
        end_call(shared, *call);
        if (!failure) {
            co_return false;
        }
        if (shared.closed.load(std::memory_order_acquire) || call->cancelled) {
            throw boost::system::system_error(net::error::operation_aborted);
        }
        if (timed_out) {
            throw boost::system::system_error(beast::error::timeout);
        }
        std::rethrow_exception(failure);
    }

    /// Sends @p call and delivers the answer to the reader. A tools/call refused with HeaderMismatch
    /// is sent once more after the tool schemas are refreshed.
    static Task<void> run_modern(std::shared_ptr<Impl> impl, std::shared_ptr<ModernCall> call) {
        auto& shared = *impl->state;
        std::exception_ptr failure;
        bool streaming = false;
        try {
            streaming = co_await run_exchange(impl, call, true);
            if (!streaming && call->method == "tools/call" && is_header_mismatch(*call)) {
                co_await refresh_tool_schemas(impl, call);
                streaming = co_await run_exchange(impl, call, true);
            }
        } catch (...) {
            failure = std::current_exception();
        }

        if (failure) {
            if (call->cancelled && !shared.closed.load(std::memory_order_acquire)) {
                // The response stream was closed on purpose; the request is over.
                if (call->expects_response && call->forward) {
                    impl->enqueue_message(
                        error_response(*call, g_REQUEST_CANCELLED, "Request cancelled"));
                }
                co_return;
            }
            std::rethrow_exception(failure);
        }
        if (streaming) {
            co_return;
        }
        shared.modern_version = call->version;
        impl->deliver_final(*call);
    }

    net::strand<net::any_io_executor> strand;
    std::shared_ptr<SharedState> state;
    std::string host;
    std::string port;
    std::string path;
    mutable std::mutex provider_mutex;
    /// Guarded by provider_mutex: written by set_bearer_token_provider(), read once per request in
    /// pin_bearer_token_provider().
    std::function<std::string()> bearer_token_provider;
    /// Guarded by provider_mutex.
    std::function<void(const std::string&)> warning_handler;
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

/// Safe to call at any time, including mid-flight: a request already started keeps the provider it
/// pinned, and the next one picks up the new value.
void HttpClientTransport::set_bearer_token_provider(std::function<std::string()> provider) {
    std::lock_guard lock(impl_->provider_mutex);
    impl_->bearer_token_provider = std::move(provider);
}

void HttpClientTransport::set_warning_handler(std::function<void(const std::string&)> handler) {
    std::lock_guard lock(impl_->provider_mutex);
    impl_->warning_handler = std::move(handler);
}

Task<std::string> HttpClientTransport::read_message() {
    auto impl = impl_;
    return net::co_spawn(impl->strand, Impl::run_read(impl), net::use_awaitable);
}

Task<void> HttpClientTransport::write_message(std::string_view message) {
    auto impl = impl_;
    // Pinned here, on the caller's thread, before the coroutine is created: this call is where the
    // request starts, so this is the value it means. Nothing on the strand reads the member later.
    auto bearer_token_provider = impl->pin_bearer_token_provider();
    return net::co_spawn(impl->strand,
                         Impl::run_write(impl, std::string(message), std::move(bearer_token_provider)),
                         net::use_awaitable);
}

void HttpClientTransport::close() {
    if (impl_->state->closed.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    auto shared_state = impl_->state;
    net::post(shared_state->timer.get_executor(), [shared_state]() { shared_state->timer.cancel(); });

    auto close_target = std::make_shared<Impl::CloseTarget>(
        Impl::CloseTarget{impl_->host, impl_->port, impl_->path, impl_->pin_bearer_token_provider()});
    net::co_spawn(
        impl_->strand,
        [shared_state, close_target = std::move(close_target)]() -> Task<void> {
            for (auto& entry : shared_state->modern_calls) {
                Impl::SharedState::close_call_socket(*entry.second);
            }
            if (shared_state->operation_active) {
                shared_state->resolver.cancel();
                if (shared_state->stream) {
                    // Closed, not cancelled: a cancel reaches only an operation that is pending
                    // right now, and the write may be between two of them or have one already
                    // completed and waiting to resume. A closed socket fails the next one too.
                    beast::error_code ignored;
                    shared_state->stream->socket().close(ignored);
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
                // A write that had already finished keeps its stream, now with a closed socket.
                if (shared_state->stream && !shared_state->stream->socket().is_open()) {
                    shared_state->stream.reset();
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
                        const detail::SocketDeadline connect_deadline(
                            shared_state->stream->get_executor(), shared_state->stream->socket(),
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

                    const detail::SocketDeadline deadline(
                        shared_state->stream->get_executor(), shared_state->stream->socket(),
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

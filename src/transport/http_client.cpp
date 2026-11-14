#include "../detail/header_value.hpp"
#include "../detail/http_request_metadata.hpp"
#include "../detail/param_headers.hpp"
#include "../detail/peer_json.hpp"
#include "../detail/request_meta.hpp"
#include "../detail/socket_deadline.hpp"

#include <mcp/core/constants.hpp>
#include <mcp/transport/http_client.hpp>
#include <mcp/transport/http_types.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
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
/// How many times one request's event stream is resumed after it ends without the response.
constexpr int g_max_stream_resumes = 3;
/// The pause before a stream is opened again when the server named none.
constexpr std::chrono::milliseconds g_default_reconnect_delay{1000};
/// The shortest pause before a stream is opened again, whatever the server asked for.
constexpr std::chrono::milliseconds g_min_reconnect_delay{100};
/// The longest pause before a stream is opened again, whatever the server asked for.
constexpr std::chrono::milliseconds g_max_reconnect_delay{30000};
/// Longest `Mcp-Param-*` value sent. Longer ones do not get through the servers that limit a header.
constexpr std::size_t g_max_param_header_bytes = 4096;
/// The most pages of tools/list one schema refresh follows.
constexpr int g_max_refresh_pages = 64;

/// Reads a text/event-stream body in the pieces the network delivers it and returns the data of each
/// event as soon as its terminating blank line arrives. It keeps the last event id and the
/// reconnection time the server asked for, which a resume needs; comment lines and other fields are
/// skipped.
class SseDecoder {
   public:
    SseDecoder() = default;
    /// A decoder for a stream resumed after the event @p last_id.
    explicit SseDecoder(std::string last_id) : last_id_(std::move(last_id)) {}

    /// The id of the last event that named one, which a resume sends as Last-Event-ID.
    [[nodiscard]] const std::string& last_id() const { return last_id_; }

    /// The reconnection time a `retry` field asked for, if one did.
    [[nodiscard]] std::optional<std::chrono::milliseconds> retry() const { return retry_; }

    /// Whether the stream carried an event or an event id.
    [[nodiscard]] bool carried_anything() const { return carried_anything_; }

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
            carried_anything_ = true;
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
                carried_anything_ = true;
            }
        } else if (line_.compare(0, 3, "id:") == 0) {
            std::string_view value = std::string_view(line_).substr(3);
            if (!value.empty() && value.front() == ' ') {
                value.remove_prefix(1);
            }
            // An id holding NUL is ignored, as the SSE standard has it.
            if (value.find('\0') == std::string_view::npos) {
                last_id_.assign(value.data(), value.size());
                carried_anything_ = true;
            }
        } else if (line_.compare(0, 6, "retry:") == 0) {
            std::string_view value = std::string_view(line_).substr(6);
            if (!value.empty() && value.front() == ' ') {
                value.remove_prefix(1);
            }
            if (!value.empty() && value.size() <= g_max_retry_digits &&
                value.find_first_not_of("0123456789") == std::string_view::npos) {
                retry_ = std::chrono::milliseconds(std::stoll(std::string(value)));
            }
        } else if (line_.compare(0, 5, "data:") == 0) {
            std::string_view value = std::string_view(line_).substr(5);
            if (!value.empty() && value.front() == ' ') {
                value.remove_prefix(1);
            }
            if (payload_.size() + value.size() + 1 > g_max_modern_message_bytes) {
                throw std::runtime_error("An SSE event from the server is too long");
            }
            if (!payload_.empty()) {
                payload_.push_back('\n');
            }
            payload_.append(value.data(), value.size());
        }
        line_.clear();
    }

    /// Longest `retry` value read, in digits: past this the field is ignored.
    static constexpr std::size_t g_max_retry_digits = 9;

    std::string line_;
    std::string payload_;
    std::string last_id_;
    bool carried_anything_ = false;
    std::optional<std::chrono::milliseconds> retry_;
};

/// The call cannot be sent as it stands, which is its caller's error rather than a failed write.
class UnsendableCall : public std::runtime_error {
   public:
    using std::runtime_error::runtime_error;
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
    /// Sent the way a session of protocol revision 2025-11-25 or earlier sends it: with the
    /// session's headers rather than the mirrored ones.
    bool legacy{false};
    /// A GET: the session's stream for messages of no request, or the resume of a stream after
    /// `last_event_id`.
    bool get{false};
    std::string last_event_id;
    /// Sent inside a session, where an error status keeps its HTTP meaning: a 404 says the session
    /// is gone.
    bool in_session{false};

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
            : timer(strand), operation_timer(strand), listen_timer(strand), resolver(strand) {
            operation_timer.expires_at(std::chrono::steady_clock::time_point::max());
        }

        net::steady_timer timer;
        net::steady_timer operation_timer;
        /// The pause before the session's GET stream is opened again.
        net::steady_timer listen_timer;
        net::ip::tcp::resolver resolver;
        std::optional<beast::tcp_stream> stream;
        std::queue<std::string> queue;
        std::atomic<bool> closed{false};
        mutable std::mutex metadata_mutex;
        std::string session_id;
        std::string last_event_id;
        std::string protocol_version{std::string(g_LATEST_LEGACY_PROTOCOL_VERSION)};
        std::optional<std::string> initialize_request_key;
        /// The initialize request that started the session, repeated to start a new one when the
        /// server has forgotten it. Touched on the strand only.
        std::string initialize_request;
        bool reinitialize_on_expired_session{true};
        /// Set while the session the server forgot has not been replaced: a new one is started
        /// before the next request. Touched on the strand only.
        bool session_lost{false};
        /// The protocol version and capabilities the server answered initialize with. A session
        /// started in place of a forgotten one must answer with the same, since the Client knows
        /// the server by them. Touched on the strand only.
        nlohmann::json session_terms;
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
        /// The session whose GET stream is read, or was refused. Touched on the strand only.
        std::string listening_session;

        /// Ends the exchange of the call that @p key names, and of the calls made for it. Returns
        /// whether there was one.
        bool cancel_modern_call(const std::string& key) {
            bool found = false;
            for (auto& entry : modern_calls) {
                auto& call = *entry.second;
                // A legacy request is cancelled by the notification itself, which is sent.
                if (call.legacy) {
                    continue;
                }
                if (call.id_key == key || call.owner_key == key) {
                    call.cancelled = true;
                    close_call_socket(call);
                    found = true;
                }
            }
            return found;
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

    /// Whether the peer has closed @p stream, or sent something no request asked for. A server
    /// closes a connection that sits idle between requests; a request written into it is never
    /// read, so the connection is not used again.
    static bool closed_by_peer(beast::tcp_stream& stream) {
        auto& socket = stream.socket();
        if (!socket.is_open()) {
            return true;
        }
        beast::error_code error;
        const bool was_non_blocking = socket.non_blocking();
        (void)socket.non_blocking(true, error);
        if (error) {
            return false;
        }
        char byte = 0;
        const auto read = socket.read_some(net::buffer(&byte, 1), error);
        beast::error_code restore_error;
        (void)socket.non_blocking(was_non_blocking, restore_error);
        return read != 0 || error != net::error::would_block;
    }

    Task<void> ensure_connected() {
        if (state->stream.has_value()) {
            if (!closed_by_peer(*state->stream)) {
                co_return;
            }
            reset_connection();
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
        // The wait for a response has a long limit, so this is what finds a peer that vanished.
        beast::error_code keep_alive_error;
        (void)stream.socket().set_option(net::socket_base::keep_alive(true), keep_alive_error);
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
                state->initialize_request = std::string(message);
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
            state->session_terms = terms_of(result);
        } catch (const std::exception&) {
            // The client protocol layer owns response validation.
        }
    }

    /// What of an initialize result the Client relies on: the protocol version and capabilities.
    static nlohmann::json terms_of(const nlohmann::json& result) {
        return nlohmann::json{{"protocolVersion", result.value("protocolVersion", nlohmann::json())},
                              {"capabilities", result.value("capabilities", nlohmann::json())}};
    }

    /// The terms of the initialize result @p response carries for the initialize @p sent, as
    /// JSON or in an event, or nullopt when it carries none.
    static std::optional<nlohmann::json> initialize_terms(const StringResponse& response,
                                                          const std::string& sent) {
        const auto request = detail::parse_peer_json(sent);
        if (!request.is_object() || !request.contains("id")) {
            return std::nullopt;
        }
        const auto answers = [&request](const std::string& text) -> std::optional<nlohmann::json> {
            const auto message = detail::parse_peer_json(text);
            if (!message.is_object() || message.value("id", nlohmann::json()) != request.at("id") ||
                !message.contains("result") || !message.at("result").is_object()) {
                return std::nullopt;
            }
            return terms_of(message.at("result"));
        };
        const auto content_type = response.find(http::field::content_type);
        if (content_type == response.end()) {
            return std::nullopt;
        }
        if (starts_with(std::string(content_type->value()), "text/event-stream")) {
            auto events = parse_sse_messages(response.body());
            for (; !events.empty(); events.pop()) {
                if (auto terms = answers(events.front().data)) {
                    return terms;
                }
            }
            return std::nullopt;
        }
        return answers(response.body());
    }

    /// Whether the initialize result @p response carries for @p sent names another protocol
    /// version or other capabilities than the session it replaces.
    [[nodiscard]] bool changes_session_terms(const StringResponse& response,
                                             const std::string& sent) const {
        if (state->session_terms.is_null()) {
            return false;
        }
        const auto terms = initialize_terms(response, sent);
        return terms && *terms != state->session_terms;
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
        // initialize starts the session, so it goes alone; every other message of the session
        // goes on an exchange of its own.
        if (!is_initialize(message)) {
            co_await run_legacy(impl,
                                legacy_call(std::move(message), std::move(bearer_token_provider)));
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

        bool in_session = !state.session_id.empty();
        // Kept to read the answer against, and to send again once a new session has started. Held
        // outside the coroutine frame: GCC 11 mishandles a std::string that lives across a
        // suspension point there.
        const auto sent = std::make_shared<const std::string>(std::move(message));
        try {
            if (state.session_lost && !initialize_operation && state.reinitialize_on_expired_session &&
                !state.initialize_request.empty()) {
                // The last attempt to replace a forgotten session failed, so this request tries
                // again before it is sent.
                co_await reinitialize(impl, bearer_token_provider);
                in_session = !state.session_id.empty();
            }
            auto response = co_await exchange(impl, *sent, bearer_token_provider);
            if (response.result() == http::status::not_found && in_session && !initialize_operation &&
                state.reinitialize_on_expired_session && !state.initialize_request.empty()) {
                // The server no longer knows the session: a new one is started with the initialize
                // that began this one, and the request is sent once more in it.
                co_await reinitialize(impl, bearer_token_provider);
                in_session = !state.session_id.empty();
                response = co_await exchange(impl, *sent, bearer_token_provider);
            }
            impl->capture_session_id(response);
            impl->process_response(response, *sent, in_session);
            if (initialize_operation) {
                state.initialize_request_key.reset();
            }
            complete_operation(impl->state);
        } catch (...) {
            if (initialize_operation) {
                state.initialize_request_key.reset();
            }
            // A refused request leaves nothing to reuse: the next one opens a new connection.
            impl->reset_connection();
            complete_operation(impl->state);
            throw;
        }
    }

    /// Starts a new session by sending the initialize that started the last one, then
    /// notifications/initialized. The initialize result goes to the reader too, which no longer
    /// waits for it and drops it.
    static Task<void> reinitialize(std::shared_ptr<Impl> impl,
                                   std::function<std::string()> bearer_token_provider) {
        auto& state = *impl->state;
        {
            std::lock_guard lock(state.metadata_mutex);
            state.session_id.clear();
            state.last_event_id.clear();
        }
        state.session_lost = true;
        const auto initialize = std::make_shared<const std::string>(state.initialize_request);
        impl->capture_initialize_request(*initialize);
        const auto initialized = co_await exchange(impl, *initialize, bearer_token_provider);
        if (impl->changes_session_terms(initialized, *initialize)) {
            // The Client would go on with what the old session said of the server.
            state.initialize_request_key.reset();
            throw std::runtime_error(
                "The server started a new session with another protocol version or other "
                "capabilities; connect again to use it");
        }
        impl->capture_session_id(initialized);
        try {
            impl->process_response(initialized, *initialize, false);
        } catch (...) {
            state.initialize_request_key.reset();
            throw;
        }
        state.initialize_request_key.reset();
        const auto notification = std::make_shared<const std::string>(
            R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
        const bool in_session = !state.session_id.empty();
        const auto accepted = co_await exchange(impl, *notification, bearer_token_provider);
        impl->process_response(accepted, *notification, in_session);
        state.session_lost = false;
    }

    static bool is_initialize(std::string_view message) {
        const auto parsed = nlohmann::json::parse(message, nullptr, false);
        return parsed.is_object() && parsed.contains("id") &&
               parsed.value("method", "") == "initialize";
    }

    /// A message of the legacy session as a call: a request, which waits for its response, or a
    /// notification or response of the client's, which waits for nothing.
    static std::shared_ptr<ModernCall> legacy_call(std::string message,
                                                   std::function<std::string()> bearer_token_provider) {
        auto call = std::make_shared<ModernCall>();
        call->legacy = true;
        call->expects_response = false;
        call->bearer_token_provider = std::move(bearer_token_provider);
        const auto parsed = nlohmann::json::parse(message, nullptr, false);
        if (parsed.is_object()) {
            const auto method = parsed.find("method");
            const auto id = parsed.find("id");
            if (method != parsed.end() && method->is_string()) {
                call->method = method->get<std::string>();
                if (id != parsed.end() && (id->is_string() || id->is_number_integer())) {
                    call->expects_response = true;
                    call->id = *id;
                    call->id_key = id->get<RequestId>().correlation_key();
                }
            }
        }
        call->body = std::move(message);
        return call;
    }

    /// Waits while initialize, or the start of a new session, is under way.
    static Task<void> wait_for_operation(std::shared_ptr<Impl> impl) {
        auto& state = *impl->state;
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
    }

    /// Starts a new session in place of the one the server forgot, unless another message already
    /// did.
    static Task<void> replace_session(std::shared_ptr<Impl> impl,
                                      std::function<std::string()> bearer_token_provider) {
        auto& state = *impl->state;
        co_await wait_for_operation(impl);
        if (!state.session_lost) {
            co_return;
        }
        state.operation_timer.expires_at(std::chrono::steady_clock::time_point::max());
        state.operation_active = true;
        try {
            co_await reinitialize(impl, bearer_token_provider);
        } catch (...) {
            impl->reset_connection();
            complete_operation(impl->state);
            throw;
        }
        complete_operation(impl->state);
        start_listening(impl, std::move(bearer_token_provider));
    }

    /// Sends a message of the legacy session on its own exchange, and hands the reader what comes
    /// back. An event stream is read in the background, so the write ends once the answer starts
    /// and other messages need not wait for it. When the server has forgotten the session, a new
    /// one is started and the message sent again in it.
    static Task<void> run_legacy(std::shared_ptr<Impl> impl, std::shared_ptr<ModernCall> call) {
        auto& shared = *impl->state;
        const bool may_replace =
            shared.reinitialize_on_expired_session && !shared.initialize_request.empty();
        for (int attempt = 0;; ++attempt) {
            co_await wait_for_operation(impl);
            if (shared.session_lost && may_replace) {
                co_await replace_session(impl, call->bearer_token_provider);
            }
            const auto session = std::make_shared<const std::string>(shared.session_id);
            call->in_session = !session->empty();
            call->serial = ++shared.next_modern_serial;
            shared.modern_calls.emplace(call->serial, call);
            std::exception_ptr failure;
            bool streaming = false;
            bool forgotten = false;
            try {
                co_await modern_open(impl, call);
                forgotten = call->status == static_cast<unsigned int>(http::status::not_found) &&
                            !session->empty() && attempt == 0 && may_replace;
                // An event stream of known length is complete already, so it is read here.
                streaming = !forgotten && call->event_stream && call->parser->get().chunked();
                if (!forgotten && !streaming) {
                    co_await modern_read(impl, call);
                    impl->remember_last_event_id(call->decoder);
                }
            } catch (...) {
                failure = std::current_exception();
            }
            if (!failure && streaming) {
                net::co_spawn(impl->strand, legacy_stream_to_end(impl, call), net::detached);
                co_return;
            }
            const bool timed_out = call->deadline && call->deadline->expired();
            end_call(shared, *call);
            if (failure) {
                if (shared.closed.load(std::memory_order_acquire) || call->cancelled) {
                    throw boost::system::system_error(net::error::operation_aborted);
                }
                if (timed_out) {
                    throw boost::system::system_error(beast::error::timeout);
                }
                std::rethrow_exception(failure);
            }
            if (forgotten) {
                if (shared.session_id == *session) {
                    shared.session_lost = true;
                }
                continue;
            }
            if (call->final_message) {
                impl->enqueue_message(std::move(*call->final_message));
            }
            if (call->method == "notifications/initialized") {
                start_listening(impl, call->bearer_token_provider);
            }
            co_return;
        }
    }

    /// Reads the event stream a request of the legacy session opened, handing the reader each
    /// message as it comes. A stream that ends before the response is resumed with a GET naming its
    /// last event, as the specification has it, a few times at most. Whatever goes wrong is
    /// reported to the reader as a JSON-RPC error for the request.
    static Task<void> legacy_stream_to_end(std::shared_ptr<Impl> impl,
                                           std::shared_ptr<ModernCall> call) {
        auto& shared = *impl->state;
        std::exception_ptr failure;
        for (int resumes = 0;; ++resumes) {
            failure = nullptr;
            try {
                co_await modern_read(impl, call);
            } catch (...) {
                failure = std::current_exception();
            }
            impl->remember_last_event_id(call->decoder);
            if (call->final_message || !call->expects_response || call->cancelled ||
                shared.closed.load(std::memory_order_acquire) || call->decoder.last_id().empty() ||
                resumes >= g_max_stream_resumes) {
                break;
            }
            call->last_event_id = call->decoder.last_id();
            const auto delay = reconnect_delay(call->decoder);
            end_call(shared, *call);
            net::steady_timer pause(impl->strand, delay);
            boost::system::error_code ignored;
            co_await pause.async_wait(net::redirect_error(net::use_awaitable, ignored));
            if (shared.closed.load(std::memory_order_acquire)) {
                break;
            }
            call->get = true;
            call->serial = ++shared.next_modern_serial;
            shared.modern_calls.emplace(call->serial, call);
            try {
                co_await modern_open(impl, call);
            } catch (...) {
                failure = std::current_exception();
                break;
            }
            if (!call->event_stream) {
                failure = std::make_exception_ptr(HttpStatusError(
                    call->status, "The server did not resume the event stream", call->challenge));
                break;
            }
        }
        end_call(shared, *call);
        if (shared.closed.load(std::memory_order_acquire)) {
            co_return;
        }
        if (call->final_message) {
            impl->enqueue_message(std::move(*call->final_message));
        } else if (call->expects_response) {
            impl->enqueue_message(error_response(
                *call, g_CONNECTION_CLOSED,
                failure ? describe(failure) : "The server's event stream ended without a response"));
        }
    }

    /// Keeps the id of the last event a stream carried for last_event_id().
    void remember_last_event_id(const SseDecoder& decoder) const {
        if (decoder.last_id().empty()) {
            return;
        }
        std::lock_guard lock(state->metadata_mutex);
        state->last_event_id = decoder.last_id();
    }

    /// How long to wait before a stream is opened again: what its `retry` field asked for, within
    /// bounds, or a second.
    static std::chrono::milliseconds reconnect_delay(const SseDecoder& decoder) {
        return std::clamp(decoder.retry().value_or(g_default_reconnect_delay), g_min_reconnect_delay,
                          g_max_reconnect_delay);
    }

    /// Opens the session's GET stream, once per session.
    static void start_listening(const std::shared_ptr<Impl>& impl,
                                std::function<std::string()> bearer_token_provider) {
        auto& shared = *impl->state;
        if (shared.session_id.empty() || shared.listening_session == shared.session_id ||
            shared.closed.load(std::memory_order_acquire)) {
            return;
        }
        shared.listening_session = shared.session_id;
        net::co_spawn(impl->strand,
                      listen(impl, std::make_shared<const std::string>(shared.session_id),
                             std::move(bearer_token_provider)),
                      net::detached);
    }

    /// Reads the session's GET stream, which carries what the server sends outside any request,
    /// and opens it again from its last event after it ends, while the session lasts. A server
    /// that offers no such stream, answering 405, is not asked again in this session.
    static Task<void> listen(std::shared_ptr<Impl> impl, std::shared_ptr<const std::string> session,
                             std::function<std::string()> bearer_token_provider) {
        auto& shared = *impl->state;
        auto last_event_id = std::make_shared<std::string>();
        std::chrono::milliseconds delay = g_default_reconnect_delay;
        std::chrono::milliseconds backoff{0};
        for (;;) {
            if (shared.closed.load(std::memory_order_acquire) || shared.session_id != *session) {
                co_return;
            }
            auto call = std::make_shared<ModernCall>();
            call->legacy = true;
            call->get = true;
            call->expects_response = false;
            call->bearer_token_provider = bearer_token_provider;
            call->last_event_id = *last_event_id;
            call->serial = ++shared.next_modern_serial;
            shared.modern_calls.emplace(call->serial, call);
            bool opened = false;
            try {
                co_await modern_open(impl, call);
                opened = call->event_stream;
                if (opened) {
                    co_await modern_read(impl, call);
                }
            } catch (...) {
                // A stream that broke is opened again below; one that never opened is not.
            }
            if (!call->decoder.last_id().empty()) {
                *last_event_id = call->decoder.last_id();
                impl->remember_last_event_id(call->decoder);
            }
            delay = reconnect_delay(call->decoder);
            // A stream that ends carrying nothing is opened again after twice the pause before it,
            // so a server that ends every stream at once is asked less and less often.
            if (call->decoder.carried_anything()) {
                backoff = std::chrono::milliseconds::zero();
            } else {
                backoff = std::min(backoff == std::chrono::milliseconds::zero() ? delay : backoff * 2,
                                   g_max_reconnect_delay);
                delay = std::max(delay, backoff);
            }
            end_call(shared, *call);
            if (!opened || shared.closed.load(std::memory_order_acquire)) {
                co_return;
            }
            shared.listen_timer.expires_after(delay);
            boost::system::error_code ignored;
            co_await shared.listen_timer.async_wait(net::redirect_error(net::use_awaitable, ignored));
        }
    }

    /// Sends @p body in one POST of the current session and reads the response.
    static Task<StringResponse> exchange(std::shared_ptr<Impl> impl, std::string body,
                                         std::function<std::string()> bearer_token_provider) {
        auto& state = *impl->state;
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
            request.body() = std::move(body);
            request.prepare_payload();

            if (!state.stream) {
                throw std::runtime_error("HttpClientTransport stream is not initialized");
            }
            auto& stream = *state.stream;
            deadline.emplace(stream.get_executor(), stream.socket(),
                             std::chrono::seconds(constants::g_http_timeout_seconds));
            co_await http::async_write(stream, request, net::use_awaitable);
            // A handler takes as long as it takes, and the caller's timeout decides how long to
            // wait for it. This only frees the connection for the next request in the end.
            deadline.reset();
            if (const auto timeout = std::chrono::milliseconds(
                    impl->response_timeout_ms.load(std::memory_order_relaxed));
                timeout > std::chrono::milliseconds::zero()) {
                deadline.emplace(stream.get_executor(), stream.socket(), timeout);
            }

            beast::flat_buffer response_buffer;
            StringResponse response;
            co_await http::async_read(stream, response_buffer, response, net::use_awaitable);
            deadline.reset();
            if (!response.keep_alive()) {
                // The server closes the connection after this response (RFC 9112 section 9.6), so
                // the next request has to open a new one.
                impl->reset_connection();
            }
            co_return response;
        } catch (...) {
            const bool timed_out = deadline && deadline->expired();
            deadline.reset();
            impl->reset_connection();
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
            if (method == "notifications/cancelled") {
                // Closing the response stream is the cancellation; no notification is sent.
                bool ended_a_call = false;
                try {
                    const auto params = parsed.find("params");
                    if (params != parsed.end() && params->is_object() &&
                        params->contains("requestId")) {
                        ended_a_call = state->cancel_modern_call(
                            params->at("requestId").get<RequestId>().correlation_key());
                    }
                } catch (const std::exception&) {
                    // A cancellation that names no usable id cancels nothing.
                }
                handled = ended_a_call || !state->modern_version.empty();
                return nullptr;
            }
            if (state->modern_version.empty()) {
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
        if (value.is_number_float()) {
            // An integral float in the safe range is the same integer the server compares with.
            const auto number = value.get<double>();
            constexpr auto limit = static_cast<double>(detail::g_MAX_SAFE_HEADER_INTEGER);
            if (!(number >= -limit && number <= limit) ||
                number != static_cast<double>(static_cast<std::int64_t>(number))) {
                return std::nullopt;
            }
            return std::to_string(static_cast<std::int64_t>(number));
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
                const auto encoded = detail::encode_header_value(*text);
                if (encoded.size() > g_max_param_header_bytes) {
                    throw UnsendableCall("Argument " + param.name +
                                         " is too long for its Mcp-Param header");
                }
                request.set("Mcp-Param-" + param.name, encoded);
            }
        }
    }

    /// Drops the tools whose `x-mcp-header` annotations the specification forbids from a tools/list
    /// answer and remembers the annotations of the rest. A page without a cursor starts the cache
    /// over. Returns @p text, or the text without the excluded tools.
    std::string track_tools(const ModernCall& call, std::string text) {
        auto answer = detail::parse_peer_json(text);
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

    /// Takes one message off the stream: a response is kept as the answer, anything else goes to the
    /// reader at once.
    void accept_stream_message(ModernCall& call, std::string text) const {
        const auto parsed = detail::parse_peer_json(text);
        if (parsed.is_object() && parsed.contains("id") && !parsed.contains("method")) {
            // This stream carries one request's response. Anything else that looks like a response
            // would end the exchange and leave the request waiting for its timeout.
            if (call.expects_response && !call.final_message && parsed.at("id") == call.id) {
                call.final_message = std::move(text);
            }
        } else if (call.forward) {
            enqueue_message(std::move(text));
        }
    }

    /// A JSON-RPC error body an HTTP error status carries, with the request's id filled in when the
    /// server left it out, or nullopt when the body is anything else.
    static std::optional<std::string> jsonrpc_error_body(const ModernCall& call,
                                                         const std::string& body) {
        auto parsed = detail::parse_peer_json(body);
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
        call->decoder = SseDecoder(call->last_event_id);
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
        beast::error_code keep_alive_error;
        (void)stream->socket().set_option(net::socket_base::keep_alive(true), keep_alive_error);

        StringRequest request{call->get ? http::verb::get : http::verb::post, impl->path,
                              constants::g_http_version_11};
        request.set(http::field::host, impl->host);
        if (call->get) {
            request.set(http::field::accept, "text/event-stream");
            if (!call->last_event_id.empty()) {
                request.set("Last-Event-ID", call->last_event_id);
            }
        } else {
            request.set(http::field::content_type, "application/json");
            request.set(http::field::accept, "application/json, text/event-stream");
        }
        if (call->bearer_token_provider) {
            const auto token = call->bearer_token_provider();
            if (!token.empty()) {
                request.set(http::field::authorization, "Bearer " + token);
            }
        }
        if (call->legacy) {
            request.set("MCP-Protocol-Version", shared.protocol_version);
            if (!shared.session_id.empty()) {
                request.set("MCP-Session-Id", shared.session_id);
            }
        } else {
            impl->apply_modern_headers(request, *call);
        }
        if (!call->get) {
            request.body() = call->body;
        }
        request.prepare_payload();
        co_await http::async_write(*stream, request, net::use_awaitable);
        // A handler takes as long as it takes. How long to wait for it is the caller's timeout, which
        // ends the call by closing its socket; the socket's keep-alive finds a peer that vanished. A
        // request of a legacy session waits at most set_response_timeout().
        call->deadline.reset();
        if (call->legacy && !call->get) {
            if (const auto timeout = std::chrono::milliseconds(
                    impl->response_timeout_ms.load(std::memory_order_relaxed));
                timeout > std::chrono::milliseconds::zero()) {
                call->deadline.emplace(stream->get_executor(), stream->socket(), timeout);
            }
        }

        call->buffer.clear();
        call->parser = std::make_unique<http::response_parser<http::buffer_body>>();
        // No limit: an event stream is as long as the request runs. Not boost::none, which Boost
        // 1.74 compares as smaller than any Content-Length and so refuses every sized body.
        call->parser->body_limit(std::numeric_limits<std::uint64_t>::max());
        co_await http::async_read_header(*stream, call->buffer, *call->parser, net::use_awaitable);
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
            const bool may_carry_error = call->expects_response && !call->in_session &&
                                         (status == 400 || status == 404 || status == 405);
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
            // A server of an older revision may answer a request with 202 alone.
            if (call->expects_response && !call->legacy) {
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
            const auto answer = detail::parse_peer_json(*refresh->final_message);
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
        end_call(shared, *call);
        if (shared.closed.load(std::memory_order_acquire) || !call->forward) {
            co_return;
        }
        if (call->cancelled) {
            impl->enqueue_message(error_response(*call, g_REQUEST_CANCELLED, "Request cancelled"));
        } else if (failure) {
            impl->enqueue_message(error_response(*call, g_CONNECTION_CLOSED, describe(failure)));
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
        } catch (const UnsendableCall& error) {
            impl->enqueue_message(error_response(*call, g_INVALID_PARAMS, error.what()));
            co_return;
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
    /// Zero means no limit.
    std::atomic<std::chrono::milliseconds::rep> response_timeout_ms{
        std::chrono::milliseconds(std::chrono::minutes(10)).count()};
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

void HttpClientTransport::set_response_timeout(std::chrono::milliseconds timeout) {
    constexpr auto longest = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::duration::max() / 2);
    impl_->response_timeout_ms.store(timeout > longest ? 0 : timeout.count(),
                                     std::memory_order_relaxed);
}

void HttpClientTransport::set_reinitialize_on_expired_session(bool enabled) {
    auto state = impl_->state;
    net::post(impl_->strand, [state, enabled] { state->reinitialize_on_expired_session = enabled; });
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
            shared_state->listen_timer.cancel();
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

#include "../detail/peer_json.hpp"
#include "../detail/socket_deadline.hpp"

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
            if (is_supported_protocol_version(protocol_version)) {
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

        const bool had_session = !state.session_id.empty();
        // Kept to send again once a new session has started. Held outside the coroutine frame:
        // GCC 11 mishandles a std::string that lives across a suspension point there.
        const auto retry_body = std::make_shared<const std::string>(
            had_session && !initialize_operation ? message : std::string());
        try {
            if (state.session_lost && !initialize_operation && state.reinitialize_on_expired_session &&
                !state.initialize_request.empty()) {
                // The last attempt to replace a forgotten session failed, so this request tries
                // again before it is sent.
                co_await reinitialize(impl, bearer_token_provider);
            }
            auto response = co_await exchange(impl, std::move(message), bearer_token_provider);
            if (response.result() == http::status::not_found && had_session && !initialize_operation &&
                state.reinitialize_on_expired_session && !state.initialize_request.empty()) {
                // The server no longer knows the session: a new one is started with the initialize
                // that began this one, and the request is sent once more in it.
                co_await reinitialize(impl, bearer_token_provider);
                response = co_await exchange(impl, *retry_body, bearer_token_provider);
            }
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
            impl->process_response(initialized);
        } catch (...) {
            state.initialize_request_key.reset();
            throw;
        }
        state.initialize_request_key.reset();
        const auto notification = std::make_shared<const std::string>(
            R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
        const auto accepted = co_await exchange(impl, *notification, bearer_token_provider);
        impl->process_response(accepted);
        state.session_lost = false;
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

    net::strand<net::any_io_executor> strand;
    std::shared_ptr<SharedState> state;
    std::string host;
    std::string port;
    std::string path;
    mutable std::mutex provider_mutex;
    /// Guarded by provider_mutex: written by set_bearer_token_provider(), read once per request in
    /// pin_bearer_token_provider().
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

/// Safe to call at any time, including mid-flight: a request already started keeps the provider it
/// pinned, and the next one picks up the new value.
void HttpClientTransport::set_reinitialize_on_expired_session(bool enabled) {
    auto state = impl_->state;
    net::post(impl_->strand, [state, enabled] { state->reinitialize_on_expired_session = enabled; });
}

void HttpClientTransport::set_bearer_token_provider(std::function<std::string()> provider) {
    std::lock_guard lock(impl_->provider_mutex);
    impl_->bearer_token_provider = std::move(provider);
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

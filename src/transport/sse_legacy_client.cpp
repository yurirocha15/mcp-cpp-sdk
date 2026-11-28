#include "mcp/detail/constants.hpp"
#include "mcp/detail/net/beast_string.hpp"
#include "mcp/detail/net/connection.hpp"
#include "mcp/detail/net/http_connect.hpp"
#include "mcp/detail/net/network_failure.hpp"
#include "mcp/detail/net/param_headers.hpp"
#include "mcp/detail/net/proxy.hpp"
#include "mcp/detail/net/socket_deadline.hpp"
#include "mcp/detail/net/tls.hpp"
#include "mcp/detail/protocol/request_meta.hpp"
#include "mcp/detail/text/diagnostic_text.hpp"
#include "mcp/detail/transport/sse_decoder.hpp"

#include <mcp/core/async.hpp>
#include <mcp/core/constants.hpp>
#include <mcp/transport/http_client.hpp>
#include <mcp/transport/http_types.hpp>
#include <mcp/transport/sse_legacy.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <limits>
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

namespace net = boost::asio;
namespace beast = boost::beast;
namespace http = boost::beast::http;

namespace {

namespace client_constants = mcp::detail::constants::http_client;
namespace sse_constants = mcp::detail::constants::sse_legacy;

bool has_control_character(std::string_view text) {
    return std::ranges::any_of(text, [](char symbol) {
        const auto code = static_cast<unsigned char>(symbol);
        return code <= sse_constants::g_LAST_SPACE_OR_CONTROL ||
               code == sse_constants::g_DELETE_CHARACTER;
    });
}

/// Whether @p message is a request of protocol revision 2026-07-28, which this transport cannot
/// carry: server/discover, or a request whose `_meta` names a protocol version.
bool is_modern_message(const std::string& message) {
    const auto parsed = nlohmann::json::parse(message, nullptr, false);
    if (!parsed.is_object()) {
        return false;
    }
    const auto method = parsed.find("method");
    if (method == parsed.end() || !method->is_string()) {
        return false;
    }
    const auto& name = method->get_ref<const std::string&>();
    return name == mcp::constants::methods::g_SERVER_DISCOVER ||
           detail::is_modern_request(parsed, name);
}

/// RFC 3986 section 5.2.4: removes the `.` and `..` segments of @p path.
std::string remove_dot_segments(std::string_view path) {
    std::vector<std::string> segments;
    std::size_t position = 0;
    bool trailing_slash = false;
    while (position <= path.size()) {
        const auto slash = path.find('/', position);
        const auto end = slash == std::string_view::npos ? path.size() : slash;
        const auto segment = path.substr(position, end - position);
        trailing_slash = false;
        if (segment == "..") {
            if (segments.size() > 1) {
                segments.pop_back();
            }
            trailing_slash = true;
        } else if (segment == ".") {
            trailing_slash = true;
        } else {
            segments.emplace_back(segment);
        }
        if (slash == std::string_view::npos) {
            break;
        }
        position = slash + 1;
    }
    std::string result;
    for (std::size_t index = 0; index < segments.size(); ++index) {
        if (index != 0) {
            result.push_back('/');
        }
        result += segments[index];
    }
    if (trailing_slash && (result.empty() || result.back() != '/')) {
        result.push_back('/');
    }
    return result;
}

/// Whether @p reference starts with a URI scheme, as `https:`.
bool has_scheme(std::string_view reference) {
    const auto stop = reference.find_first_of("/?#:");
    if (stop == std::string_view::npos || stop == 0 || reference[stop] != ':') {
        return false;
    }
    const auto head = reference.substr(0, stop);
    if (std::isalpha(static_cast<unsigned char>(head.front())) == 0) {
        return false;
    }
    return head.find_first_not_of(
               "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789+-.") ==
           std::string_view::npos;
}

[[noreturn]] void refuse_endpoint(std::string_view reason, std::string_view data) {
    throw TransportError(TransportError::Kind::eProtocol,
                         "The endpoint of the server's event stream was refused: " +
                             std::string(reason) + " (" + detail::sanitize_for_diagnostics(data) + ")");
}

/// The request target a POST goes to: the endpoint event's data @p data resolved against the event
/// stream's URL @p base (RFC 3986 section 5.2). An endpoint on another scheme, host or port than
/// the stream's is refused.
std::string resolve_message_target(std::string_view data, const detail::HttpUrl& base) {
    const auto reference_text = detail::trim_blanks(data);
    std::string_view reference = reference_text;
    if (reference.empty()) {
        refuse_endpoint("it is empty", data);
    }
    if (has_control_character(reference)) {
        refuse_endpoint("it holds a control character or a space", data);
    }
    if (const auto fragment = reference.find('#'); fragment != std::string_view::npos) {
        reference = reference.substr(0, fragment);
    }

    const bool network_path = reference.starts_with("//");
    if (has_scheme(reference) || network_path) {
        const std::string absolute =
            network_path ? std::string(base.https ? "https:" : "http:") + std::string(reference)
                         : std::string(reference);
        detail::HttpUrl url;
        try {
            url = detail::parse_http_url(absolute, sse_constants::g_TRANSPORT_NAME);
        } catch (const std::invalid_argument&) {
            refuse_endpoint("it is not an http or https URL", data);
        }
        if (url.https != base.https || !detail::equals_ignoring_case(url.host, base.host) ||
            url.port != base.port) {
            refuse_endpoint("it names another scheme, host or port than the event stream", data);
        }
        return url.path;
    }

    const auto query_start = reference.find('?');
    const auto reference_path = reference.substr(0, query_start);
    const std::string_view query =
        query_start == std::string_view::npos ? std::string_view{} : reference.substr(query_start);

    std::string path;
    if (reference_path.empty()) {
        // Only a query: the stream's own path.
        path = base.path.substr(0, base.path.find('?'));
    } else if (reference_path.front() == '/') {
        path = remove_dot_segments(reference_path);
    } else {
        const auto base_path = std::string_view(base.path).substr(0, base.path.find('?'));
        const auto directory = base_path.substr(0, base_path.rfind('/') + 1);
        path = remove_dot_segments(std::string(directory) + std::string(reference_path));
    }
    if (path.empty() || path.front() != '/') {
        path.insert(0, "/");
    }
    return path + std::string(query);
}

}  // namespace

struct LegacySseClientTransport::Impl {
    /// What one POST holds. Kept on the heap, and not in the coroutine's frame, as GCC 11 mishandles
    /// a std::string that lives across a suspension point there.
    struct PostExchange {
        std::string body;
        std::function<std::string()> bearer_token_provider;
        http::request<http::string_body> request;
        http::response<http::string_body> response;
        beast::flat_buffer buffer;
    };

    /// What the event stream's GET holds, on the heap for the same reason.
    struct StreamExchange {
        http::request<http::empty_body> request;
        beast::flat_buffer buffer;
        std::unique_ptr<http::response_parser<http::buffer_body>> parser;
        detail::SseDecoder decoder;
        std::function<std::string()> bearer_token_provider;
    };

    Impl(const net::any_io_executor& executor, const std::string& url)
        : strand(net::make_strand(executor)), read_timer(strand), endpoint_timer(strand) {
        base = detail::parse_http_url(url, sse_constants::g_TRANSPORT_NAME);
        authority = detail::authority_of(base.host, base.port, base.https);
        server_name = detail::tls_server_name(base.host);
        rebuild_plan();
        read_timer.expires_at(std::chrono::steady_clock::time_point::max());
        endpoint_timer.expires_at(std::chrono::steady_clock::time_point::max());
    }

    /// Builds how the next connections are made from the current settings. Callers hold
    /// settings_mutex, or run before the transport is shared.
    void rebuild_plan() {
        auto next = std::make_shared<detail::ConnectPlan>();
        next->host = base.host;
        next->port = base.port;
        next->https = base.https;
        next->server_name = server_name;
        next->tls = tls_context;
        next->proxy = detail::select_proxy(proxy, base.https, base.host, base.port);
        plan = std::move(next);
    }

    [[nodiscard]] std::shared_ptr<const detail::ConnectPlan> current_plan() const {
        std::scoped_lock lock(settings_mutex);
        return plan;
    }

    [[nodiscard]] std::function<std::string()> pin_bearer_token_provider() const {
        std::scoped_lock lock(settings_mutex);
        return bearer_token_provider;
    }

    static std::vector<net::ip::tcp::endpoint> endpoints_of(
        const net::ip::tcp::resolver::results_type& results) {
        std::vector<net::ip::tcp::endpoint> endpoints;
        for (const auto& entry : results) {
            endpoints.push_back(entry.endpoint());
        }
        return endpoints;
    }

    template <typename Request>
    void set_headers(Request& request, const detail::ConnectPlan& connection_plan,
                     const std::function<std::string()>& provider) const {
        request.set(http::field::host, authority);
        if (connection_plan.forwards_requests() && !connection_plan.proxy->authorization.empty()) {
            request.set(http::field::proxy_authorization, connection_plan.proxy->authorization);
        }
        if (provider) {
            const auto token = provider();
            if (!token.empty()) {
                request.set(http::field::authorization, "Bearer " + token);
            }
        }
    }

    /// A new connection to the server, made the way @p connection_plan says.
    Task<std::shared_ptr<detail::Connection>> connect(
        std::shared_ptr<const detail::ConnectPlan> connection_plan) {
        net::ip::tcp::resolver resolver(strand);
        net::ip::tcp::resolver::results_type resolved;
        try {
            resolved = co_await resolver.async_resolve(
                connection_plan->connect_host(), connection_plan->connect_port(), net::use_awaitable);
        } catch (const boost::system::system_error& error) {
            throw detail::ConnectFailure(error.code());
        }
        if (closed.load(std::memory_order_acquire)) {
            throw TransportError(TransportError::Kind::eClosed, "LegacySseClientTransport is closed");
        }
        auto connection = detail::make_connection(strand, *connection_plan);
        // Reachable by close() before the handshake ends, so a close during it closes the socket.
        opening = connection;
        co_await detail::establish(connection, endpoints_of(resolved), connection_plan);
        opening.reset();
        co_return connection;
    }

    [[noreturn]] void throw_ended() const {
        if (failure) {
            std::rethrow_exception(failure);
        }
        throw TransportError(TransportError::Kind::eClosed,
                             closed_by_caller.load(std::memory_order_acquire)
                                 ? "LegacySseClientTransport is closed"
                                 : "The server's event stream ended");
    }

    void enqueue(std::string message) {
        queue.push(std::move(message));
        read_timer.cancel();
    }

    /// Takes an event of the stream. Returns whether it was the endpoint event.
    bool accept_event(detail::SseDecoder::SseEvent event) {
        if (!endpoint_target) {
            if (event.name != sse_constants::g_EVENT_ENDPOINT) {
                throw TransportError(TransportError::Kind::eProtocol,
                                     "The first event of the server's stream was not `endpoint`");
            }
            endpoint_target = resolve_message_target(event.data, base);
            endpoint_timer.cancel();
            return true;
        }
        if (event.name == sse_constants::g_EVENT_MESSAGE) {
            enqueue(std::move(event.data));
        }
        return false;
    }

    /// Ends the transport: the stream is over, whether it failed or finished.
    void finish(const std::exception_ptr& cause) {
        ended = true;
        if (cause && !closed_by_caller.load(std::memory_order_acquire)) {
            failure = detail::as_transport_failure(cause, sse_constants::g_TRANSPORT_NAME);
        }
        closed.store(true, std::memory_order_release);
        close_sockets();
    }

    void close_sockets() {
        read_timer.cancel();
        endpoint_timer.cancel();
        for (const auto& connection : {stream, post_stream, opening}) {
            if (connection) {
                beast::error_code ignored;
                connection->socket().close(ignored);
            }
        }
    }

    void reset_post_connection() {
        if (post_stream) {
            detail::close_connection(*post_stream);
            post_stream.reset();
            post_plan.reset();
        }
    }

    static void start(const std::shared_ptr<Impl>& impl) {
        if (impl->started) {
            return;
        }
        impl->started = true;
        net::co_spawn(impl->strand, run_stream(impl, impl->pin_bearer_token_provider()), net::detached);
    }

    static Task<std::string> run_read(std::shared_ptr<Impl> impl) {
        if (impl->read_active) {
            throw std::logic_error("LegacySseClientTransport supports one pending read");
        }
        impl->read_active = true;
        try {
            start(impl);
            for (;;) {
                if (!impl->queue.empty()) {
                    auto message = std::move(impl->queue.front());
                    impl->queue.pop();
                    impl->read_active = false;
                    co_return message;
                }
                if (impl->ended || impl->closed.load(std::memory_order_acquire)) {
                    impl->throw_ended();
                }
                impl->read_timer.expires_at(std::chrono::steady_clock::time_point::max());
                try {
                    co_await impl->read_timer.async_wait(net::use_awaitable);
                } catch (const boost::system::system_error& error) {
                    if (error.code() != net::error::operation_aborted) {
                        throw;
                    }
                }
            }
        } catch (...) {
            impl->read_active = false;
            throw;
        }
    }

    /// Opens the event stream and reads it until it ends.
    static Task<void> run_stream(std::shared_ptr<Impl> impl,
                                 std::function<std::string()> bearer_token_provider) {
        auto exchange = std::make_shared<StreamExchange>();
        exchange->bearer_token_provider = std::move(bearer_token_provider);
        std::exception_ptr cause;
        try {
            co_await read_stream(impl, exchange);
        } catch (...) {
            cause = std::current_exception();
        }
        impl->finish(cause);
    }

    static Task<void> read_stream(std::shared_ptr<Impl> impl,
                                  std::shared_ptr<StreamExchange> exchange) {
        const auto connection_plan = impl->current_plan();
        auto stream = co_await impl->connect(connection_plan);
        if (impl->closed.load(std::memory_order_acquire)) {
            throw TransportError(TransportError::Kind::eClosed, "LegacySseClientTransport is closed");
        }
        impl->stream = stream;

        exchange->request = http::request<http::empty_body>{
            http::verb::get, connection_plan->request_target(impl->base.path),
            client_constants::g_VERSION_11};
        impl->set_headers(exchange->request, *connection_plan, exchange->bearer_token_provider);
        exchange->request.set(http::field::accept,
                              std::string(sse_constants::g_CONTENT_TYPE_EVENT_STREAM));
        exchange->request.set(http::field::cache_control, "no-cache");

        // Until the endpoint event arrives; a stream is open for as long as the session lasts.
        std::optional<detail::SocketDeadline> deadline;
        deadline.emplace(stream->get_executor(), stream->socket(),
                         std::chrono::seconds(client_constants::g_TIMEOUT_SECONDS));
        try {
            co_await http::async_write(*stream, exchange->request, net::use_awaitable);
            exchange->parser = std::make_unique<http::response_parser<http::buffer_body>>();
            // No limit: the stream is as long as the session.
            exchange->parser->body_limit(std::numeric_limits<std::uint64_t>::max());
            co_await http::async_read_header(*stream, exchange->buffer, *exchange->parser,
                                             net::use_awaitable);
        } catch (...) {
            if (deadline && deadline->expired()) {
                throw TransportError(TransportError::Kind::eConnect,
                                     "The server did not answer the event stream's request in time");
            }
            throw;
        }
        auto& head = exchange->parser->get();
        const auto status = head.result_int();
        if (status < static_cast<unsigned int>(http::status::ok) ||
            status >= static_cast<unsigned int>(http::status::multiple_choices)) {
            std::string challenge;
            if (const auto header = head.find(http::field::www_authenticate); header != head.end()) {
                challenge = std::string(header->value());
            }
            throw HttpStatusError(status, "HTTP request failed with status " + std::to_string(status),
                                  std::move(challenge));
        }
        const auto content_type = head.find(http::field::content_type);
        if (content_type == head.end() ||
            !detail::to_std_string_view(content_type->value())
                 .starts_with(sse_constants::g_CONTENT_TYPE_EVENT_STREAM)) {
            throw TransportError(TransportError::Kind::eProtocol,
                                 "The server answered the event stream's request with another content "
                                 "type than text/event-stream");
        }

        std::array<char, client_constants::g_READ_CHUNK_BYTES> chunk{};
        while (!exchange->parser->is_done()) {
            exchange->parser->get().body().data = chunk.data();
            exchange->parser->get().body().size = chunk.size();
            try {
                co_await http::async_read_some(*stream, exchange->buffer, *exchange->parser,
                                               net::use_awaitable);
            } catch (const boost::system::system_error& error) {
                if (error.code() != http::error::need_buffer) {
                    if (deadline && deadline->expired()) {
                        throw TransportError(TransportError::Kind::eConnect,
                                             "The server sent no endpoint event in time");
                    }
                    throw;
                }
            }
            const std::size_t received = chunk.size() - exchange->parser->get().body().size;
            if (received == 0) {
                continue;
            }
            for (auto& event :
                 exchange->decoder.feed_events(std::string_view(chunk.data(), received))) {
                if (impl->accept_event(std::move(event))) {
                    deadline.reset();
                }
            }
        }
    }

    static Task<void> run_write(std::shared_ptr<Impl> impl, std::shared_ptr<PostExchange> exchange) {
        // Refused here, not sent, so that a Client that probes with server/discover goes on to
        // initialize at once.
        if (is_modern_message(exchange->body)) {
            throw TransportError(TransportError::Kind::eProtocol,
                                 "The HTTP+SSE transport carries only protocol revisions before "
                                 "2026-07-28");
        }
        if (impl->write_active) {
            throw std::logic_error("LegacySseClientTransport supports one pending write");
        }
        impl->write_active = true;
        try {
            co_await post(impl, exchange);
        } catch (...) {
            impl->write_active = false;
            throw;
        }
        impl->write_active = false;
    }

    static Task<void> post(std::shared_ptr<Impl> impl, std::shared_ptr<PostExchange> exchange) {
        if (impl->closed.load(std::memory_order_acquire)) {
            impl->throw_ended();
        }
        start(impl);
        while (!impl->endpoint_target) {
            if (impl->ended || impl->closed.load(std::memory_order_acquire)) {
                impl->throw_ended();
            }
            impl->endpoint_timer.expires_at(std::chrono::steady_clock::time_point::max());
            try {
                co_await impl->endpoint_timer.async_wait(net::use_awaitable);
            } catch (const boost::system::system_error& error) {
                if (error.code() != net::error::operation_aborted) {
                    throw;
                }
            }
        }

        std::optional<detail::SocketDeadline> deadline;
        try {
            const auto connection_plan = impl->current_plan();
            if (!impl->post_stream || impl->post_plan != connection_plan ||
                detail::closed_by_peer(*impl->post_stream)) {
                impl->reset_post_connection();
                impl->post_stream = co_await impl->connect(connection_plan);
                impl->post_plan = connection_plan;
            }
            const auto stream = impl->post_stream;

            exchange->request = http::request<http::string_body>{
                http::verb::post, connection_plan->request_target(*impl->endpoint_target),
                client_constants::g_VERSION_11};
            impl->set_headers(exchange->request, *connection_plan, exchange->bearer_token_provider);
            exchange->request.set(http::field::content_type, "application/json");
            exchange->request.body() = std::move(exchange->body);
            exchange->request.prepare_payload();

            deadline.emplace(stream->get_executor(), stream->socket(),
                             std::chrono::seconds(client_constants::g_TIMEOUT_SECONDS));
            co_await http::async_write(*stream, exchange->request, net::use_awaitable);
            co_await http::async_read(*stream, exchange->buffer, exchange->response,
                                      net::use_awaitable);
            deadline.reset();
            if (!exchange->response.keep_alive()) {
                impl->reset_post_connection();
            }
        } catch (...) {
            const bool timed_out = deadline && deadline->expired();
            deadline.reset();
            impl->reset_post_connection();
            if (impl->closed.load(std::memory_order_acquire)) {
                impl->throw_ended();
            }
            if (timed_out) {
                throw TransportError(TransportError::Kind::eIo,
                                     "The server did not answer the POST in time");
            }
            std::rethrow_exception(detail::as_transport_failure(std::current_exception(),
                                                                sse_constants::g_TRANSPORT_NAME));
        }

        const auto status = exchange->response.result_int();
        if (status < static_cast<unsigned int>(http::status::ok) ||
            status >= static_cast<unsigned int>(http::status::multiple_choices)) {
            std::string challenge;
            if (const auto header = exchange->response.find(http::field::www_authenticate);
                header != exchange->response.end()) {
                challenge = std::string(header->value());
            }
            throw HttpStatusError(status, "HTTP request failed with status " + std::to_string(status),
                                  std::move(challenge));
        }
    }

    net::strand<net::any_io_executor> strand;
    net::steady_timer read_timer;
    net::steady_timer endpoint_timer;

    /// The event stream's URL, split.
    detail::HttpUrl base;
    std::string authority;

    /// Guards the settings below.
    mutable std::mutex settings_mutex;
    std::string server_name;
    std::shared_ptr<const TlsClientContext> tls_context;
    ProxyOptions proxy;
    std::function<std::string()> bearer_token_provider;
    std::shared_ptr<const detail::ConnectPlan> plan;

    std::atomic<bool> closed{false};
    /// Set by close(), so that a stream that ends because of it reports no failure.
    std::atomic<bool> closed_by_caller{false};

    // Touched on the strand only.
    bool started{false};
    bool ended{false};
    bool read_active{false};
    bool write_active{false};
    std::queue<std::string> queue;
    /// The request target the server's endpoint event named, once it has.
    std::optional<std::string> endpoint_target;
    std::exception_ptr failure;
    std::shared_ptr<detail::Connection> stream;
    /// The connection being made, until its handshake ends.
    std::shared_ptr<detail::Connection> opening;
    std::shared_ptr<detail::Connection> post_stream;
    std::shared_ptr<const detail::ConnectPlan> post_plan;
};

LegacySseClientTransport::LegacySseClientTransport(const Executor& executor, const std::string& url)
    : impl_(std::make_shared<Impl>(executor.get(), url)) {}

LegacySseClientTransport::~LegacySseClientTransport() {
    try {
        close();
    } catch (const std::exception&) {
        // A destructor does not throw.
    }
}

void LegacySseClientTransport::set_bearer_token_provider(std::function<std::string()> provider) {
    std::scoped_lock lock(impl_->settings_mutex);
    impl_->bearer_token_provider = std::move(provider);
}

void LegacySseClientTransport::set_tls(const TlsClientOptions& options) {
    set_tls(TlsClientContext::create(options));
}

void LegacySseClientTransport::set_tls(std::shared_ptr<const TlsClientContext> context) {
    if (!impl_->base.https) {
        throw std::invalid_argument("TLS settings need an https:// URL");
    }
    std::scoped_lock lock(impl_->settings_mutex);
    impl_->tls_context = std::move(context);
    impl_->rebuild_plan();
}

void LegacySseClientTransport::set_server_name(std::string name) {
    if (!impl_->base.https) {
        throw std::invalid_argument("A TLS server name needs an https:// URL");
    }
    name = detail::tls_server_name(std::move(name));
    if (name.empty()) {
        throw std::invalid_argument("The TLS server name is empty");
    }
    std::scoped_lock lock(impl_->settings_mutex);
    impl_->server_name = std::move(name);
    impl_->rebuild_plan();
}

void LegacySseClientTransport::set_proxy(ProxyOptions options) {
    detail::validate_proxy_options(options);
    std::scoped_lock lock(impl_->settings_mutex);
    impl_->proxy = std::move(options);
    impl_->rebuild_plan();
}

Task<std::string> LegacySseClientTransport::read_message() {
    auto impl = impl_;
    return net::co_spawn(impl->strand, Impl::run_read(impl), net::use_awaitable);
}

Task<void> LegacySseClientTransport::write_message(std::string_view message) {
    auto impl = impl_;
    auto exchange = std::make_shared<Impl::PostExchange>();
    exchange->body = std::string(message);
    exchange->bearer_token_provider = impl->pin_bearer_token_provider();
    return net::co_spawn(impl->strand, Impl::run_write(impl, std::move(exchange)), net::use_awaitable);
}

void LegacySseClientTransport::close() {
    auto impl = impl_;
    impl->closed_by_caller.store(true, std::memory_order_release);
    if (impl->closed.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    net::post(impl->strand, [impl] { impl->close_sockets(); });
}

bool LegacySseClientTransport::ended() const noexcept {
    return impl_->closed.load(std::memory_order_acquire);
}

}  // namespace mcp

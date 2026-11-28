#pragma once

#include <mcp/core/constants.hpp>
#include <mcp/core/export.hpp>
#include <mcp/protocol/protocol.hpp>
#include <mcp/transport/http_types.hpp>
#include <mcp/transport/request_context.hpp>
#include <mcp/transport/tls.hpp>
#include <mcp/transport/transport.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <iterator>
#include <mcp/core/executor.hpp>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace mcp {

class HttpServerTransport;

namespace detail {
/// Number of sessions `transport` has ended in response to an HTTP DELETE. The Server compares it
/// across `initialize` requests to recognise the start of a new session on the same transport.
std::uint64_t terminated_session_count(const HttpServerTransport& transport) noexcept;

/// What Server::run() tells the HttpServerTransport it serves about itself.
struct HttpServerBinding {
    /// The protocol versions the server serves, named in the error that answers an initialize
    /// sent with a 2026-07-28 version header.
    std::vector<std::string> served_versions;
    /// Serves a request of 2026-07-28 and returns its serialized response; null while nothing
    /// serves them, when the transport hands them to whoever reads it. The server it belongs to is
    /// held weakly: once that server is gone it answers with an error.
    std::function<Task<std::string>(nlohmann::json, RequestContext)> modern_dispatch;
    /// Where modern_dispatch runs; required when modern_dispatch is set.
    Executor dispatch_executor;
};

/// Installed by Server::run().
/// @throws std::invalid_argument When the binding has a modern_dispatch but no dispatch_executor.
void bind_server(HttpServerTransport& transport, HttpServerBinding binding);
}  // namespace detail

/**
 * @brief A bounded in-memory event store for SSE resumability.
 *
 * Stores recent SSE events with monotonically increasing IDs. When the
 * store exceeds its capacity, the oldest events are evicted. Clients can replay
 * missed events by providing the last event ID they received. Each event belongs
 * to a stream, and a replay holds only the later events of the stream that the
 * given event belongs to.
 *
 * Not thread-safe. External synchronization is required for concurrent access.
 */
class EventStore {
   public:
    /**
     * @brief Construct an event store with a given capacity.
     *
     * @param capacity Maximum number of events to retain. Past
     *        mcp::constants::event_store::g_MAX_BYTES the oldest go first as well.
     */
    explicit EventStore(std::size_t capacity = mcp::constants::event_store::g_DEFAULT_CAPACITY)
        : capacity_(capacity) {}

    /**
     * @brief Append an event to the store and return its assigned ID.
     *
     * @param data The serialized event data (JSON-RPC message).
     * @param stream The stream the event is sent on. Events appended without one share a stream.
     * @return The monotonically increasing event ID assigned to this event.
     */
    std::string append(std::string data, std::string stream = {}) {
        auto id = std::to_string(++next_id_);
        stored_bytes_ += data.size();
        events_.push_back(StoredEvent{.id = id, .stream = std::move(stream), .data = std::move(data)});
        while (events_.size() > capacity_ ||
               (events_.size() > 1 && stored_bytes_ > mcp::constants::event_store::g_MAX_BYTES)) {
            stored_bytes_ -= events_.front().data.size();
            events_.pop_front();
        }
        return id;
    }

    /**
     * @brief The stream an event was sent on.
     *
     * @param event_id The ID of the event.
     * @return The stream given to append(), or std::nullopt if the event is not stored.
     */
    [[nodiscard]] std::optional<std::string> stream_of(const std::string& event_id) const {
        for (const auto& event : events_) {
            if (event.id == event_id) {
                return event.stream;
            }
        }
        return std::nullopt;
    }

    /**
     * @brief Retrieve the events after a given event ID on that event's stream.
     *
     * @param last_event_id The ID of the last event the client received.
     * @return A vector of (id, data) pairs for the later events of the stream that
     *         last_event_id belongs to, or std::nullopt if last_event_id has been evicted.
     */
    [[nodiscard]] std::optional<SseEventList> events_after(const std::string& last_event_id) const {
        if (events_.empty()) {
            return SseEventList{};
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

        const auto& stream = std::prev(it)->stream;
        SseEventList result;
        for (; it != events_.end(); ++it) {
            if (it->stream == stream) {
                result.emplace_back(it->id, it->data);
            }
        }
        return result;
    }

    /**
     * @brief Get all events currently in the store.
     *
     * @return A vector of (id, data) pairs for all stored events.
     */
    [[nodiscard]] SseEventList all_events() const {
        SseEventList result;
        result.reserve(events_.size());
        for (const auto& event : events_) {
            result.emplace_back(event.id, event.data);
        }
        return result;
    }

    /**
     * @brief Get the number of events currently stored.
     *
     * @return The current number of stored events.
     */
    [[nodiscard]] std::size_t size() const { return events_.size(); }

    /**
     * @brief Get the maximum capacity of the store.
     *
     * @return The maximum number of events retained before eviction.
     */
    [[nodiscard]] std::size_t capacity() const { return capacity_; }

    /**
     * @brief Clear all stored events.
     */
    void clear() {
        events_.clear();
        stored_bytes_ = 0;
        next_id_ = 0;
    }

   private:
    struct StoredEvent {
        std::string id;
        std::string stream;
        std::string data;
    };

    std::size_t capacity_;
    std::size_t stored_bytes_{0};
    std::deque<StoredEvent> events_;
    std::uint64_t next_id_{0};
};

/**
 * @brief HTTP server transport for MCP Streamable HTTP.
 *
 * Accepts HTTP connections, processes inbound JSON-RPC messages, and routes
 * outbound responses back to their matching HTTP requests. Supports SSE
 * replay via an in-memory event store.
 *
 * In its legacy session, a POST from a client that accepts SSE over HTTP/1.1 is answered with a
 * stream once the server sends anything else for the request first, such as a notification or a
 * request of its own: those go on the stream as they come, and the response ends it. A GET opens
 * the session's stream for messages that belong to no request, and a GET with Last-Event-ID
 * resumes a stream from there, still live when it has not ended.
 *
 * Alongside its one legacy session, it serves requests of protocol revision 2026-07-28, which
 * need no session: their mirrored headers are checked, they are answered with the status the
 * specification gives their errors, and they never read or receive `Mcp-Session-Id`. The answer
 * is plain JSON unless a handler sends a notification (a log message, progress) while serving the
 * request and the client accepts `text/event-stream`: then it becomes an SSE stream carrying those
 * notifications and finally the response. A `subscriptions/listen` request is an SSE stream that
 * lasts until the client closes it or the server ends the subscription. A client that
 * disconnects while its request runs cancels it, and so does a reset after a half-close. A client
 * that accepts `text/event-stream` and half-closes gets an SSE stream even when no notification is
 * sent, carrying comments that probe whether it is still there, because a client that closed
 * cleanly cannot be told from one that only half-closed until a write fails; the HTTP status of
 * such an answer is 200 even for an error, which the JSON-RPC body still carries. Notifications
 * wait in a queue limited by StreamOptions::max_queued_notifications and max_queued_bytes. These
 * requests are served once a Server::run() reads from the transport; before that they reach
 * whoever calls read_message().
 */
class MCP_API HttpServerTransport final : public ITransport {
   public:
    /**
     * @brief Construct an HTTP server transport bound to a host and port.
     *
     * @param executor Executor used for asynchronous operations.
     * @param host Local bind address.
     * @param port Local bind port.
     * @param event_store_capacity Maximum number of replayable SSE events to retain.
     */
    HttpServerTransport(
        const Executor& executor, std::string host, unsigned short port,
        std::size_t event_store_capacity = mcp::constants::event_store::g_DEFAULT_CAPACITY);

    ~HttpServerTransport() override;

    HttpServerTransport(const HttpServerTransport&) = delete;
    HttpServerTransport& operator=(const HttpServerTransport&) = delete;
    HttpServerTransport(HttpServerTransport&&) = delete;
    HttpServerTransport& operator=(HttpServerTransport&&) = delete;

    /**
     * @brief Access the event store used for SSE replay support.
     *
     * @return A const reference to the transport's in-memory event store.
     */
    [[nodiscard]] const EventStore& event_store() const;

    /**
     * @brief Get the local port currently bound by the HTTP acceptor.
     *
     * This is useful when the transport is constructed with port 0 and the
     * operating system assigns an ephemeral port.
     */
    [[nodiscard]] unsigned short port() const;

    /**
     * @brief Enable or disable JSON-only responses.
     *
     * When enabled, HTTP POST responses always use `application/json` and
     * outbound responses are not copied into the SSE replay event store.
     * This option is atomic and may be changed while the listener is running.
     *
     * @param json_only True to bypass SSE framing and event storage.
     */
    void set_json_only(bool json_only);

    /**
     * @brief Set how the answer to a 2026-07-28 request is streamed.
     *
     * @throws std::invalid_argument If an interval is not positive, or max_queued_notifications or
     *         max_queued_bytes is zero.
     * @throws std::logic_error If listen() has already been called or the transport is closed.
     */
    void set_stream_options(StreamOptions options);

    /**
     * @brief Set how the SSE streams of a session of 2025-11-25 or earlier are written.
     *
     * @throws std::invalid_argument If LegacyStreamOptions::sse_retry is not positive.
     * @throws std::logic_error If listen() has already been called or the transport is closed.
     */
    void set_legacy_stream_options(LegacyStreamOptions options);

    /// What the 2026-07-28 streams of this transport have done.
    [[nodiscard]] StreamStats stream_stats() const;

    /**
     * @brief Replace the allowlist used for requests carrying an Origin header.
     *
     * Requests without an Origin header remain valid for non-browser MCP clients. An Origin header
     * is rejected by default until its exact value appears in this allowlist.
     * Configure the allowlist before listen() or run() starts.
     *
     * @throws std::logic_error If listen() has already been called or the transport is closed.
     */
    void set_allowed_origins(std::vector<std::string> origins);

    /**
     * @brief Explicitly opt into accepting every Origin header value.
     *
     * Configure this before listen() or run() starts.
     *
     * @throws std::logic_error If listen() has already been called or the transport is closed.
     */
    void set_allow_all_origins(bool allow_all);

    /**
     * @brief Serve only requests whose Host header names one of @p hosts.
     *
     * Without a list, a request that reaches the transport through a loopback address must name
     * a loopback host in its Host header, `localhost` or a loopback address, or it is answered
     * 403; through other addresses Host is not checked. This defends a local server against DNS
     * rebinding. A reverse proxy on the same machine that passes on the public name in Host needs
     * that name listed. With a list, every request must name a host on it, whatever address it
     * reached, so list `localhost` too if local clients use it. Names compare without the port
     * and regardless of case. An empty list restores the loopback rule.
     *
     * @throws std::invalid_argument If an entry names no host.
     * @throws std::logic_error If listen() has already been called or the transport is closed.
     */
    void set_allowed_hosts(const std::vector<std::string>& hosts);

    /**
     * @brief Accept connections over TLS with these settings.
     *
     * Every connection must then complete a TLS handshake, within the options' handshake_timeout,
     * before a request is read; one that does not is closed. With ClientAuth::eRequired, a client
     * must show a certificate from one of the named client authorities.
     *
     * @throws TlsConfigError If the options cannot be used.
     * @throws std::logic_error If listen() has already been called or the transport is closed.
     */
    void set_tls(const TlsServerOptions& options);

    /**
     * @brief Accept connections over TLS with a context created before; see set_tls(). A null
     * context serves plain TCP again.
     *
     * @throws std::logic_error If listen() has already been called or the transport is closed.
     */
    void set_tls(std::shared_ptr<const TlsServerContext> context);

    /**
     * @brief Require and validate an HTTP Authorization: Bearer header.
     *
     * Passing an empty validator disables HTTP authentication.
     * Configure the validator before listen() or run() starts.
     *
     * A request whose token the validator refuses is answered 401 with the bearer challenge. A
     * validator that throws is answered 500, without a challenge, so a failing introspection
     * endpoint does not send clients into re-authorization. A validator that accepts a token may
     * name the principal it stands for (TokenValidation); handlers see it as Context::principal().
     *
     * @throws std::logic_error If listen() has already been called or the transport is closed.
     */
    void set_bearer_token_validator(BearerTokenValidator validator);

    /**
     * @brief Require an Authorization: Bearer header and validate it asynchronously.
     *
     * Use this when the decision needs I/O -- token introspection, a JWKS fetch -- so it suspends
     * instead of blocking the executor that is concurrently serving MCP traffic.
     *
     * Passing an empty validator disables HTTP authentication.
     * Configure the validator before listen() or run() starts.
     *
     * A request whose token the validator refuses is answered 401 with the bearer challenge. A
     * validator that throws is answered 500, without a challenge, so a failing introspection
     * endpoint does not send clients into re-authorization. A validator that accepts a token may
     * name the principal it stands for (TokenValidation); handlers see it as Context::principal().
     *
     * @throws std::logic_error If a synchronous validator is already installed, or if listen() has
     *         already been called or the transport is closed.
     */
    void set_async_bearer_token_validator(AsyncBearerTokenValidator validator);

    /**
     * @brief Cap the HTTP request body this transport will read.
     *
     * A request whose body exceeds the cap is answered `413 Payload Too Large` and its connection is
     * closed; it never reaches MCP dispatch. Defaults to
     * mcp::constants::http_server::g_DEFAULT_MAX_REQUEST_BODY_BYTES. Binary content travels as base64
     * inside the JSON body, so a cap near the size of the raw content rejects it.
     *
     * The origin and bearer token are checked on the request's headers, before its body is read,
     * so a request they refuse never has its body buffered. The body is parsed as JSON only after
     * those checks pass.
     *
     * Configure the cap before listen() or run() starts.
     *
     * @throws std::invalid_argument If `max_bytes` is zero.
     * @throws std::logic_error If listen() has already been called or the transport is closed.
     */
    void set_max_request_body_bytes(std::size_t max_bytes);

    /**
     * @brief Set what the connections and requests of this transport may hold at once.
     *
     * @details The headers of a request are read first, and the Origin and bearer token checks
     * run on them, so a request they refuse never has its body read. See HttpConnectionLimits for
     * the defaults. Must be called before listen().
     *
     * @throws std::invalid_argument If `max_requests` or `max_request_bytes` is 0.
     */
    void set_connection_limits(const HttpConnectionLimits& limits);

    /**
     * @brief Set how long a session may sit idle before a new client can take its place.
     *
     * @details This transport serves one session at a time. Without this, a client that left
     * without a DELETE would keep every other client out until the process restarts. An
     * initialize then ends a session that has no request waiting and has been idle this long.
     *
     * @param timeout The time, or zero to keep a session until its DELETE. The default is five
     *        minutes.
     */
    void set_session_idle_timeout(std::chrono::milliseconds timeout);

    /**
     * @brief Set the parameters sent in the `WWW-Authenticate` header of every 401.
     *
     * Without this call the transport sends the bare `Bearer` challenge. A client that has to
     * discover where to obtain a token needs at least `resource_metadata`; setting protected
     * resource metadata fills that field in automatically when it is left empty here.
     *
     * Configure the challenge before listen() or run() starts.
     *
     * @throws std::invalid_argument If a challenge value cannot be sent in a quoted-string.
     * @throws std::logic_error If listen() has already been called or the transport is closed.
     */
    void set_bearer_challenge(BearerChallengeConfig challenge);

    /**
     * @brief Name the scopes each request needs, so a token without them is answered 403.
     *
     * @details After a validator accepts a token, the provider is asked about every POSTed JSON-RPC
     * message that has a method (see ScopeRequirementProvider). When the token's scopes
     * (TokenValidation::scopes) lack one the provider named, the message is not dispatched and no
     * response stream is opened: the answer is `403` with a
     * `WWW-Authenticate: Bearer error="insufficient_scope", scope="...", resource_metadata="..."`
     * challenge naming every scope the operation needs, which a client such as
     * OAuthClientTransport answers by authorizing again with the wider scope. The challenge keeps
     * the realm and metadata URL of set_bearer_challenge() and set_protected_resource_metadata().
     * A validator that reports no scopes grants none, so a request that needs a scope is refused.
     * Requests that carry no JSON-RPC method (answers to the server's own requests, GET and DELETE)
     * are not checked.
     *
     * The scopes the provider returns are written into the challenge as quoted header text, so
     * each must consist of printable ASCII (0x20-0x7E); a provider that returns anything else is
     * answered `500` with no challenge, as one that throws is.
     *
     * Passing an empty provider removes the check. Configure it before listen() or run() starts.
     * A provider needs a token validator to report the scopes a token holds, so
     * listen() throws when one is set without the other, whichever was set first.
     *
     * @throws std::logic_error If listen() has already been called or the transport is closed. listen()
     * also throws it when a provider is set and no validator is.
     */
    void set_scope_requirements(ScopeRequirementProvider provider);

    /**
     * @brief Serve an RFC 9728 protected-resource metadata document.
     *
     * The document answers GET requests at its configured path without an Authorization header. When
     * the bearer challenge carries no `resource_metadata`, it is populated with this document's URL.
     *
     * Configure the metadata before listen() or run() starts.
     *
     * @throws std::invalid_argument If `resource` is empty or is not an absolute URL.
     * @throws std::logic_error If listen() has already been called or the transport is closed.
     */
    void set_protected_resource_metadata(ProtectedResourceMetadataConfig metadata);

    /**
     * @brief Exempt request paths from bearer validation and exclude them from MCP dispatch.
     *
     * An entry is excused from the bearer check and also removed from the set of paths MCP answers:
     * if the protected-resource metadata route does not claim it, the request is answered `404 Not
     * Found` rather than dispatched, so MCP is never served without authentication on an exempt path.
     *
     * Each entry is compared for equality against the path component of the request target, with any
     * query string or fragment removed first, so `/health` also exempts `/health?probe=1`. Empty by
     * default.
     *
     * This transport has no custom request handler, so an exempt path other than the metadata
     * route is always answered 404. Serving a health check without a token needs
     * StreamableHttpSessionManager, whose custom request handler can claim the path.
     *
     * Configure the paths before listen() or run() starts.
     *
     * @throws std::logic_error If listen() has already been called or the transport is closed.
     */
    void set_unauthenticated_paths(std::vector<std::string> paths);

    /// 2026-07-28 requests refused with `mcp::constants::jsonrpc::g_HEADER_MISMATCH` because their
    /// MCP-Protocol-Version, Mcp-Method, Mcp-Name or Mcp-Param-* header was missing, malformed,
    /// repeated or did not match the body.
    [[nodiscard]] std::uint64_t header_mismatch_rejections() const;

    /**
     * @brief Read the next queued JSON-RPC message from HTTP POST bodies.
     */
    Task<std::string> read_message() override;

    /**
     * @brief Read the next queued JSON-RPC message with the principal of the request that carried
     *        it.
     *
     * @details The principal is the id the bearer token validator returned in TokenValidation,
     * or else `h:` followed by the hex SHA-256 of the request's bearer token, or of its
     * Authorization header when that holds no bearer token, or empty when the request had none. It is
     * kept with each queued message, so the requests of several clients waiting together are each
     * attributed to their own sender.
     */
    Task<IncomingMessage> read_incoming() override;

    /**
     * @brief Send a JSON-RPC message: a response answers its pending HTTP request, and any other
     *        message goes on the session's GET stream, or on the stream of the only request
     *        waiting when no GET opened one.
     *
     * @param message Serialized JSON-RPC message.
     */
    Task<void> write_message(std::string_view message) override;

    /**
     * @brief Send a message the server sends while it serves a request, on that request's stream
     *        while it waits, or as write_message() does otherwise.
     *
     * @param message Serialized JSON-RPC message.
     * @param related_request The id of the client request, as JSON text.
     */
    Task<void> write_related_message(std::string_view message,
                                     std::string_view related_request) override;

    /**
     * @brief Close the transport and stop accepting connections.
     */
    void close() override;

    /**
     * @brief Start accepting HTTP connections.
     *
     * @return A task that completes when the accept loop exits.
     */
    Task<void> listen();

    /**
     * @brief This transport, which Server::run() serves as an HttpServerTransport.
     */
    HttpServerTransport* http_server_transport() noexcept override { return this; }

   private:
    friend std::uint64_t detail::terminated_session_count(
        const HttpServerTransport& transport) noexcept;
    friend void detail::bind_server(HttpServerTransport& transport, detail::HttpServerBinding binding);

    struct Impl;
    static Task<void> listen_impl(std::shared_ptr<Impl> impl);
    std::shared_ptr<Impl> impl_;
};

}  // namespace mcp

#pragma once

#include <mcp/core/constants.hpp>
#include <mcp/core/export.hpp>
#include <mcp/transport/http_server.hpp>
#include <mcp/transport/transport.hpp>

#include <mcp/core/executor.hpp>
#include <mcp/transport/http_types.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace mcp {

class Server;
struct ServerProtocolPolicy;

namespace detail {
/// Gives set_custom_request_handler(), declared with the Beast types in
/// <mcp/transport/http_beast.hpp>, its way into the manager.
struct CustomRouteAccess;
/// Gives Server::run_http() its way into the manager.
struct RunAccess;
}  // namespace detail

/** @brief What a StreamableHttpSessionManager holds for clients it has not heard from. */
struct SessionLimits {
    /// Sessions at once. An initialize past it is answered 503. A session costs about 31 KiB
    /// before it does anything, so this bounds what an unauthenticated client can make a server
    /// hold; a bearer token validator refuses such a client before any session exists.
    std::size_t max_sessions{mcp::constants::http_server::g_DEFAULT_MAX_SESSIONS};
    /// How long a session lives after its last request, unless a request of it is still running,
    /// or zero to keep it until its DELETE. A client that leaves without one is otherwise kept
    /// until the process ends.
    std::chrono::milliseconds idle_timeout{mcp::constants::http_server::g_DEFAULT_SESSION_IDLE_TIMEOUT};
};

/**
 * @brief Multi-session HTTP endpoint for MCP Streamable HTTP.
 *
 * @details Manages multiple concurrent MCP sessions over a single HTTP port.
 * Each session is backed by its own Server + MemoryTransport pair. Requests
 * are routed by the `MCP-Session-Id` header. New sessions are created on
 * `initialize` requests that arrive without a session ID.
 *
 * This matches the behavior of Python's StreamableHTTPSessionManager and
 * Go's StreamableHTTPServer: session creation on initialize, 404 on unknown
 * sessions, DELETE for session teardown.
 *
 * A request of protocol revision 2026-07-28 needs no session. Its answer is plain JSON unless a
 * handler sends a notification (a log message, progress) while serving it and the client accepts
 * `text/event-stream`: then the answer becomes an SSE stream carrying those notifications and
 * finally the response. A client that accepts `text/event-stream` and half-closes its connection
 * while the request runs gets an SSE stream as well, carrying comments that probe whether the
 * client is still there, and then the response: a client that closed cleanly cannot be told from
 * one that only half-closed until a write fails. The HTTP status of such an answer is 200 even for
 * an error, which the JSON-RPC body still carries. A client that disconnects while its request
 * runs cancels it, and so does a reset after a half-close. Notifications wait in a queue limited
 * by StreamOptions::max_queued_notifications and max_queued_bytes.
 *
 * This class is NOT an ITransport. It owns the HTTP listener and manages
 * the full lifecycle of per-session Server instances.
 *
 * Usage:
 * @code
 * asio::io_context io_ctx;
 *
 * auto server_factory = [](const mcp::Executor&) {
 *     ServerCapabilities caps;
 *     caps.tools = ServerCapabilities::ToolsCapability{};
 *     Implementation info{"my-server", "1.0"};
 *     auto server = std::make_unique<Server>(info, std::move(caps));
 *     server->add_tool<json, json>("echo", "Echo tool", schema, handler);
 *     return server;
 * };
 *
 * StreamableHttpSessionManager manager(io_ctx.get_executor(), "0.0.0.0", 8080,
 *                                       std::move(server_factory));
 * asio::co_spawn(io_ctx, manager.listen(), asio::detached);
 * io_ctx.run();
 * @endcode
 */
class MCP_API StreamableHttpSessionManager {
   public:
    /// Callback that creates a configured Server instance for a new session.
    using ServerFactory = std::function<std::unique_ptr<Server>(const Executor&)>;

    /**
     * @brief Construct a multi-session HTTP endpoint.
     *
     * @param executor Executor for async operations.
     * @param host Local bind address.
     * @param port Local bind port.
     * @param factory Callback that creates a configured Server for each new session.
     * @param event_store_capacity Per-session event store capacity.
     */
    StreamableHttpSessionManager(
        const Executor& executor, std::string host, unsigned short port, ServerFactory factory,
        std::size_t event_store_capacity = mcp::constants::event_store::g_DEFAULT_CAPACITY);

    /**
     * @brief Construct a multi-session HTTP endpoint that serves @p server, as Server::run_http()
     *        does, on the program's own event loop.
     *
     * @details @p server answers the 2026-07-28 requests, and each session of an older revision
     * is served by a server that shares its tools, resources, prompts, handlers and settings, so
     * the members of @p server that belong to one session, such as Server::send_request(), stand
     * for none of them. set_protocol_policy() and set_tool_executor() set them on @p server
     * itself, and a policy set with Server::set_protocol_policy() before listen() holds for the
     * endpoint as one set here does. While listen() runs, Server::stop() closes the manager.
     * @p server must outlive the manager and the task listen() returns.
     *
     * @param executor Executor for async operations.
     * @param host Local bind address.
     * @param port Local bind port, or 0 for one the system chooses (see port()).
     * @param server The server to serve.
     * @param event_store_capacity Per-session event store capacity.
     */
    StreamableHttpSessionManager(
        const Executor& executor, std::string host, unsigned short port, Server& server,
        std::size_t event_store_capacity = mcp::constants::event_store::g_DEFAULT_CAPACITY);

    ~StreamableHttpSessionManager();

    StreamableHttpSessionManager(const StreamableHttpSessionManager&) = delete;
    StreamableHttpSessionManager& operator=(const StreamableHttpSessionManager&) = delete;
    StreamableHttpSessionManager(StreamableHttpSessionManager&&) = delete;
    StreamableHttpSessionManager& operator=(StreamableHttpSessionManager&&) = delete;

    /**
     * @brief The local port the manager listens on: the one the constructor was given, or the one
     *        the system chose when it was given 0.
     */
    [[nodiscard]] unsigned short port() const;

    /**
     * @brief Replace the allowlist used for requests carrying an Origin header.
     *
     * Requests without an Origin header remain valid. Browser-originated requests are denied by
     * default until their exact Origin value is present in this list.
     * Configure the allowlist before listen() starts.
     *
     * @throws std::logic_error If listen() has already been called or the manager is closed.
     */
    void set_allowed_origins(std::vector<std::string> origins);

    /**
     * @brief Explicitly opt into accepting every Origin header value before listen() starts.
     * @throws std::logic_error If listen() has already been called or the manager is closed.
     */
    void set_allow_all_origins(bool allow_all);

    /**
     * @brief Serve only requests whose Host header names one of @p hosts, before listen() starts.
     *
     * The rule is HttpServerTransport::set_allowed_hosts()'s: without a list, a request that
     * reaches the manager through a loopback address must name a loopback host or is answered
     * 403, against DNS rebinding; with one, every request must name a listed host.
     *
     * @throws std::invalid_argument If an entry names no host.
     * @throws std::logic_error If listen() has already been called or the manager is closed.
     */
    void set_allowed_hosts(const std::vector<std::string>& hosts);

    /**
     * @brief Accept connections over TLS with these settings, before listen() starts; see
     * HttpServerTransport::set_tls().
     *
     * @throws TlsConfigError If the options cannot be used.
     * @throws std::logic_error If listen() has already been called or the manager is closed.
     */
    void set_tls(const TlsServerOptions& options);

    /**
     * @brief Accept connections over TLS with a context created before. A null context serves
     * plain TCP again.
     *
     * @throws std::logic_error If listen() has already been called or the manager is closed.
     */
    void set_tls(std::shared_ptr<const TlsServerContext> context);

    /**
     * @brief Require and validate an HTTP Authorization: Bearer header.
     *
     * Passing an empty validator disables HTTP authentication.
     * Configure the validator before listen() starts.
     *
     * A request whose token the validator refuses is answered 401 with the bearer challenge. A
     * validator that throws is answered 500, without a challenge, so a failing introspection
     * endpoint does not send clients into re-authorization. A validator that accepts a token may
     * name the principal it stands for (TokenValidation); handlers of requests served without a
     * session see it as Context::principal(). A session belongs to the principal that created it:
     * another principal gets 404. Without a named principal a token counts as one by itself, so a
     * client that refreshes its token starts a new session; naming the principal avoids that.
     * Without a validator, a session is not bound to the Authorization header.
     *
     * @throws std::logic_error If listen() has already been called or the manager is closed.
     */
    void set_bearer_token_validator(BearerTokenValidator validator);

    /**
     * @brief Require an Authorization: Bearer header and validate it asynchronously.
     *
     * Use this when the decision needs I/O -- token introspection, a JWKS fetch -- so it suspends
     * instead of blocking the executor that is concurrently serving MCP traffic.
     *
     * Passing an empty validator disables HTTP authentication.
     * Configure the validator before listen() starts.
     *
     * A request whose token the validator refuses is answered 401 with the bearer challenge. A
     * validator that throws is answered 500, without a challenge, so a failing introspection
     * endpoint does not send clients into re-authorization. A validator that accepts a token may
     * name the principal it stands for (TokenValidation); handlers of requests served without a
     * session see it as Context::principal(). A session belongs to the principal that created it:
     * another principal gets 404. Without a named principal a token counts as one by itself, so a
     * client that refreshes its token starts a new session; naming the principal avoids that.
     * Without a validator, a session is not bound to the Authorization header.
     *
     * @throws std::logic_error If a synchronous validator is already installed, or if listen() has
     *         already been called or the manager is closed.
     */
    void set_async_bearer_token_validator(AsyncBearerTokenValidator validator);

    /**
     * @brief Cap the HTTP request body this manager will read.
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
     * Configure the cap before listen() starts.
     *
     * @throws std::invalid_argument If `max_bytes` is zero.
     * @throws std::logic_error If listen() has already been called or the manager is closed.
     */
    void set_max_request_body_bytes(std::size_t max_bytes);

    /**
     * @brief Set the parameters sent in the `WWW-Authenticate` header of every 401.
     *
     * Without this call the manager sends the bare `Bearer` challenge. A client that has to
     * discover where to obtain a token needs at least `resource_metadata`; setting protected
     * resource metadata fills that field in automatically when it is left empty here.
     *
     * Configure the challenge before listen() starts.
     *
     * @throws std::invalid_argument If a challenge value cannot be sent in a quoted-string.
     * @throws std::logic_error If listen() has already been called or the manager is closed.
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
     * Passing an empty provider removes the check. Configure it before listen() starts.
     * A provider needs a token validator to report the scopes a token holds, so
     * listen() throws when one is set without the other, whichever was set first.
     *
     * @throws std::logic_error If listen() has already been called or the manager is closed. listen()
     * also throws it when a provider is set and no validator is.
     */
    void set_scope_requirements(ScopeRequirementProvider provider);

    /**
     * @brief Serve an RFC 9728 protected-resource metadata document.
     *
     * The document answers GET requests at its configured path without an Authorization header, ahead
     * of both the bearer check and the custom request handler. When the bearer challenge carries no
     * `resource_metadata`, it is populated with this document's URL.
     *
     * Configure the metadata before listen() starts.
     *
     * @throws std::invalid_argument If `resource` is empty or is not an absolute URL.
     * @throws std::logic_error If listen() has already been called or the manager is closed.
     */
    void set_protected_resource_metadata(ProtectedResourceMetadataConfig metadata);

    /**
     * @brief Exempt request paths from bearer validation and exclude them from MCP dispatch.
     *
     * An entry is excused from the bearer check and also removed from the set of paths MCP answers:
     * an exempt request still reaches the protected-resource metadata route and the custom request
     * handler, but if both decline it is answered `404 Not Found` rather than dispatched, so MCP is
     * never served without authentication on an exempt path.
     *
     * Each entry is compared for equality against the path component of the request target, with any
     * query string or fragment removed first, so `/health` also exempts `/health?probe=1`. Empty by
     * default.
     *
     * Configure the paths before listen() starts.
     *
     * @throws std::logic_error If listen() has already been called or the manager is closed.
     */
    void set_unauthenticated_paths(std::vector<std::string> paths);

    /**
     * @brief Get the number of active sessions.
     *
     * @return The number of live MCP sessions currently managed by this endpoint.
     */
    [[nodiscard]] std::size_t session_count() const;

    /**
     * @brief Set what the manager holds for clients it has not heard from.
     *
     * @param limits The limits. They apply from the next session and the next sweep.
     * @throws std::invalid_argument If `limits.max_sessions` is 0.
     */
    void set_session_limits(const SessionLimits& limits);

    /**
     * @brief Set what the connections and requests of this endpoint may hold at once.
     *
     * @details The headers of a request are read first, and the Origin and bearer token checks
     * run on them, so a request they refuse never has its body read. See HttpConnectionLimits for
     * the defaults.
     *
     * @throws std::invalid_argument If `max_requests` or `max_request_bytes` is 0.
     */
    void set_connection_limits(const HttpConnectionLimits& limits);

    /**
     * @brief Enable or disable JSON-only responses for managed sessions.
     *
     * When enabled, POST responses always use `application/json` and session
     * replay events are not stored.
     * This option is atomic and may be changed while the listener is running.
     *
     * @param json_only True to bypass SSE framing and replay storage.
     */
    void set_json_only(bool json_only);

    /**
     * @brief Enable or disable stateless direct JSON handling.
     *
     * When enabled, POST request/response messages are dispatched directly to a
     * Server instance without creating `Mcp-Session-Id` sessions, MemoryTransport
     * pairs, pending-response timers, SSE event stores, GET streams, or DELETE
     * teardown. Responses are always `application/json`.
     *
     * This mode is opt-in and leaves the default stateful Streamable HTTP
     * behavior unchanged.
     *
     * @param enabled True to use stateless direct JSON handling.
     * @throws std::logic_error If listen() has already been called or the manager is closed.
     */
    void set_stateless_json_mode(bool enabled);

    /**
     * @brief Set a separate executor for tool handlers.
     *
     * When set, tool handlers will run on this executor instead of the HTTP I/O executor.
     * This allows parallel tool execution independent of HTTP request handling.
     * If not set, tool handlers fall back to the HTTP executor (backward compatible).
     * A manager constructed with a Server calls that server's Server::set_handler_executor().
     *
     * @param exec The executor to use for tool execution.
     * @throws std::logic_error If listen() has already been called or the manager is closed.
     */
    void set_tool_executor(const Executor& exec);

    /**
     * @brief Choose which protocol revisions the endpoint serves.
     *
     * @details Applied to every Server the manager builds from its factory: each session's, the
     * stateless one and the shared modern server (unless set_modern_server() supplied it). A
     * manager constructed with a Server calls that server's Server::set_protocol_policy(). When
     * legacy clients are not served, no session is created: initialize is answered `404` with
     * `mcp::constants::jsonrpc::g_METHOD_NOT_FOUND` naming the versions served, and GET and DELETE
     * `405`.
     *
     * @throws std::invalid_argument If ServerProtocolPolicy::validate() refuses the policy.
     * @throws std::logic_error If listen() has already been called or the manager is closed, or
     *         when Server::set_protocol_policy() refuses it.
     */
    void set_protocol_policy(ServerProtocolPolicy policy);

    /**
     * @brief Serve every 2026-07-28 request with @p server.
     *
     * @details A 2026-07-28 request belongs to no session, so one Server answers all of them on
     * this endpoint, alongside the per-session Servers of legacy clients. Without this call it is
     * built from the factory on the first such request. Its tools, resources and prompts are what
     * every 2026-07-28 client sees, so they must not depend on the connection.
     *
     * @throws std::invalid_argument If @p server is null.
     * @throws std::logic_error If listen() has already been called or the manager is closed.
     */
    void set_modern_server(std::shared_ptr<Server> server);

    /**
     * @brief The Server that answers 2026-07-28 requests.
     *
     * @return The server given to set_modern_server(), or the one built from the factory, which
     *         this call builds if no request has yet. Called before listen(), it lets the server
     *         be configured before the first request.
     */
    [[nodiscard]] std::shared_ptr<Server> modern_server();

    /// 2026-07-28 requests refused with `mcp::constants::jsonrpc::g_HEADER_MISMATCH` because their
    /// MCP-Protocol-Version, Mcp-Method, Mcp-Name or Mcp-Param-* header was missing, malformed or did
    /// not match the body.
    [[nodiscard]] std::uint64_t header_mismatch_rejections() const;

    /**
     * @brief Set how the answers to 2026-07-28 requests are streamed.
     *
     * @throws std::invalid_argument If an interval is not positive, or max_queued_notifications or
     *         max_queued_bytes is zero.
     * @throws std::logic_error If listen() has already been called or the manager is closed.
     */
    void set_stream_options(StreamOptions options);

    /// What the 2026-07-28 streams have done so far.
    [[nodiscard]] StreamStats stream_stats() const;

    /**
     * @brief The executor the manager runs on, for building objects that belong on the same
     *        event loop, such as in Server::run_http()'s configure callback.
     */
    [[nodiscard]] Executor get_executor() const;

    /**
     * @brief Starts a graceful shutdown and waits for the requests being served.
     *
     * @details The first call starts the drain, which every call then waits for: a request that
     * begins from then on is answered `503` with `Connection: close`; the `subscriptions/listen`
     * streams of 2026-07-28 that this manager carries end as Server::close_subscriptions() ends
     * them, and so do the standalone GET streams of legacy sessions. The manager still serves what
     * a legacy client sends to finish a request in flight: its answers to the server's questions,
     * its notifications, such as `notifications/cancelled`, and a DELETE that ends its session. It
     * keeps accepting connections and keeps the open ones, so these arrive however the client
     * sends them. The answer to a request whose headers were already read is still written to its
     * connection, with `Connection: close` when it begins after the drain did. The returned task
     * completes when no such request remains, or after @p timeout, whichever comes first. A request
     * counts until its answer is completely written, so a legacy POST whose SSE response waits for its
     * handler counts, while the streams the drain ends do not.
     *
     * The drain does not close the manager: call close() afterwards. Drain is safe to call
     * more than once and from any thread. Await the task on the manager's executor.
     *
     * @param timeout How long to wait at most.
     * @throws std::invalid_argument If @p timeout is negative.
     */
    Task<void> drain(std::chrono::milliseconds timeout);

    /**
     * @brief Close the manager and all sessions.
     *
     * @details The handlers of 2026-07-28 requests that wait for a retry on the modern server are
     * given up (Server::abandon_waiting_handlers()).
     */
    void close();

    /**
     * @brief Start accepting HTTP connections.
     *
     * @return A task that completes when the listener stops accepting connections.
     */
    Task<void> listen();

   private:
    friend struct detail::CustomRouteAccess;
    friend struct detail::RunAccess;

    struct Impl;
    static Task<void> listen_impl(std::shared_ptr<Impl> impl);
    static Task<void> listen_as_run(std::shared_ptr<Impl> impl, Task<void> listening);
    static void close_impl(const std::shared_ptr<Impl>& impl);
    static Task<void> wait_until_drained(std::shared_ptr<Impl> impl, std::chrono::milliseconds timeout);
    std::shared_ptr<Impl> impl_;
};

}  // namespace mcp

#pragma once

#include <mcp/core/constants.hpp>
#include <mcp/core/export.hpp>
#include <mcp/server/server.hpp>
#include <mcp/transport/http_server.hpp>
#include <mcp/transport/transport.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/beast/http.hpp>
#include <mcp/transport/http_types.hpp>

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace mcp {

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
class MCP_API StreamableHttpSessionManager {
   public:
    /// Callback that creates a configured Server instance for a new session.
    using ServerFactory = std::function<std::unique_ptr<Server>(const boost::asio::any_io_executor&)>;

    /// Optional callback for handling non-MCP HTTP requests (e.g., health checks).
    /// Return std::nullopt to let the session manager handle the request normally.
    using CustomRequestHandler = std::function<std::optional<StringResponse>(const StringRequest&)>;

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
        const boost::asio::any_io_executor& executor, std::string host, unsigned short port,
        ServerFactory factory,
        std::size_t event_store_capacity = constants::g_event_store_default_capacity);

    ~StreamableHttpSessionManager();

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
     * @throws std::logic_error If listen() has already been called or the manager is closed.
     */
    void set_custom_request_handler(CustomRequestHandler handler);

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
     * @brief Require and validate an HTTP Authorization: Bearer header.
     *
     * Passing an empty validator disables HTTP authentication.
     * Configure the validator before listen() starts.
     *
     * @throws std::logic_error If listen() has already been called or the manager is closed.
     */
    void set_bearer_token_validator(BearerTokenValidator validator);

    /**
     * @brief Get the number of active sessions.
     *
     * @return The number of live MCP sessions currently managed by this endpoint.
     */
    [[nodiscard]] std::size_t session_count() const;

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
     *
     * @param exec The executor to use for tool execution.
     * @throws std::logic_error If listen() has already been called or the manager is closed.
     */
    void set_tool_executor(const boost::asio::any_io_executor& exec);

    /**
     * @brief Close the manager and all sessions.
     */
    void close();

    /**
     * @brief Start accepting HTTP connections.
     *
     * @return A task that completes when the listener stops accepting connections.
     */
    Task<void> listen();

   private:
    struct Impl;
    static Task<void> listen_impl(std::shared_ptr<Impl> impl);
    std::shared_ptr<Impl> impl_;
};

}  // namespace mcp

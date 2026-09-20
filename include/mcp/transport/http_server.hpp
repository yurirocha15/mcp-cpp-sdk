#pragma once

#include <mcp/core/constants.hpp>
#include <mcp/core/export.hpp>
#include <mcp/protocol/protocol.hpp>
#include <mcp/transport/http_types.hpp>
#include <mcp/transport/transport.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace mcp {

/**
 * @brief A bounded in-memory event store for SSE resumability.
 *
 * Stores recent SSE events with monotonically increasing IDs. When the
 * store exceeds its capacity, the oldest events are evicted. Clients can replay
 * missed events by providing the last event ID they received.
 *
 * Not thread-safe. External synchronization is required for concurrent access.
 */
class EventStore {
   public:
    /**
     * @brief Construct an event store with a given capacity.
     *
     * @param capacity Maximum number of events to retain.
     */
    explicit EventStore(std::size_t capacity = constants::g_event_store_default_capacity)
        : capacity_(capacity) {}

    /**
     * @brief Append an event to the store and return its assigned ID.
     *
     * @param data The serialized event data (JSON-RPC message).
     * @return The monotonically increasing event ID assigned to this event.
     */
    std::string append(std::string data) {
        auto id = std::to_string(++next_id_);
        events_.push_back(StoredEvent{id, std::move(data)});
        while (events_.size() > capacity_) {
            events_.pop_front();
        }
        return id;
    }

    /**
     * @brief Retrieve all events after a given event ID.
     *
     * @param last_event_id The ID of the last event the client received.
     * @return A vector of (id, data) pairs for events after the given ID,
     *         or std::nullopt if the last_event_id has been evicted.
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

        SseEventList result;
        for (; it != events_.end(); ++it) {
            result.emplace_back(it->id, it->data);
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
        next_id_ = 0;
    }

   private:
    struct StoredEvent {
        std::string id;
        std::string data;
    };

    std::size_t capacity_;
    std::deque<StoredEvent> events_;
    std::uint64_t next_id_{0};
};

/**
 * @brief HTTP server transport for MCP Streamable HTTP.
 *
 * Accepts HTTP connections, processes inbound JSON-RPC messages, and routes
 * outbound responses back to their matching HTTP requests. Supports SSE
 * replay via an in-memory event store.
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
        const boost::asio::any_io_executor& executor, std::string host, unsigned short port,
        std::size_t event_store_capacity = mcp::constants::g_event_store_default_capacity);

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
     * @brief Require and validate an HTTP Authorization: Bearer header.
     *
     * Passing an empty validator disables HTTP authentication.
     * Configure the validator before listen() or run() starts.
     *
     * @throws std::logic_error If listen() has already been called or the transport is closed.
     */
    void set_bearer_token_validator(BearerTokenValidator validator);

    /**
     * @brief Require an Authorization: Bearer header and validate it asynchronously.
     *
     * Use this when the decision needs I/O — token introspection, a JWKS fetch — so it suspends
     * instead of blocking the executor that is concurrently serving MCP traffic. A server that
     * installs only the synchronous validator pays nothing for this path.
     *
     * Passing an empty validator disables HTTP authentication.
     * Configure the validator before listen() or run() starts.
     *
     * @throws std::logic_error If a synchronous validator is already installed, or if listen() has
     *         already been called or the transport is closed.
     */
    void set_async_bearer_token_validator(AsyncBearerTokenValidator validator);

    /**
     * @brief Cap the HTTP request body this transport will read.
     *
     * A request whose body exceeds the cap is answered `413 Payload Too Large` and its connection
     * is closed; it never reaches MCP dispatch. Defaults to
     * mcp::constants::g_default_max_request_body_bytes. Lower it only deliberately: binary content
     * travels as base64 inside the JSON body, so a cap near the size of the raw content rejects
     * payloads that previously worked.
     *
     * Configure the cap before listen() or run() starts.
     *
     * @throws std::invalid_argument If `max_bytes` is zero.
     * @throws std::logic_error If listen() has already been called or the transport is closed.
     */
    void set_max_request_body_bytes(std::size_t max_bytes);

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
     * @brief Serve an RFC 9728 protected-resource metadata document.
     *
     * The document answers GET requests at its configured path without an Authorization header,
     * so a client holding no token can read it. When the bearer challenge carries no
     * `resource_metadata`, it is populated with this document's URL, which is all an unauthorized
     * client needs to start an OAuth flow against this server.
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
     * Both halves matter. An entry is excused from the bearer check, and it is also removed from
     * the set of paths MCP answers: if the protected-resource metadata route does not claim it,
     * the request is answered `404 Not Found` rather than dispatched. MCP is otherwise served on
     * every path, so exempting one without excluding it would serve MCP there with no
     * authentication at all — listing the path MCP runs on would silently disable authentication
     * outright. Answering 404 makes that misconfiguration fail loudly instead.
     *
     * Each entry is compared for equality against the path component of the request target, with
     * any query string or fragment removed first, so `/health` also exempts `/health?probe=1`.
     * Empty by default.
     *
     * Configure the paths before listen() or run() starts.
     *
     * @throws std::logic_error If listen() has already been called or the transport is closed.
     */
    void set_unauthenticated_paths(std::vector<std::string> paths);

    /**
     * @brief Read the next queued JSON-RPC message from HTTP POST bodies.
     */
    Task<std::string> read_message() override;

    /**
     * @brief Send a JSON-RPC response for the pending HTTP request.
     *
     * @param message Serialized JSON-RPC response.
     */
    Task<void> write_message(std::string_view message) override;

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

   private:
    struct Impl;
    static Task<void> listen_impl(std::shared_ptr<Impl> impl);
    std::shared_ptr<Impl> impl_;
};

}  // namespace mcp

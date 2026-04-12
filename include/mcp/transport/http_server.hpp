#pragma once

#include <mcp/protocol.hpp>
#include <mcp/transport.hpp>

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
 * @details Stores recent SSE events with monotonically increasing IDs. When the
 * store exceeds its capacity, the oldest events are evicted. Clients can replay
 * missed events by providing the last event ID they received.
 *
 * Thread safety: Not thread-safe. Caller must synchronize access (e.g. via strand).
 */
class EventStore {
   public:
    /**
     * @brief Construct an event store with a given capacity.
     *
     * @param capacity Maximum number of events to retain.
     */
    explicit EventStore(std::size_t capacity = 1024) : capacity_(capacity) {}

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
    std::optional<std::vector<std::pair<std::string, std::string>>> events_after(
        const std::string& last_event_id) const {
        if (events_.empty()) {
            return std::vector<std::pair<std::string, std::string>>{};
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

        std::vector<std::pair<std::string, std::string>> result;
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
    std::vector<std::pair<std::string, std::string>> all_events() const {
        std::vector<std::pair<std::string, std::string>> result;
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
 * @details Accepts HTTP requests, enqueues inbound JSON-RPC messages for MCP
 * server processing, and maps outbound JSON-RPC responses back to the matching
 * HTTP requests. Supports SSE replay via an in-memory event store.
 */
class HttpServerTransport final : public ITransport {
   public:
    /**
     * @brief Construct an HTTP server transport bound to a host and port.
     *
     * @param executor Executor used for asynchronous operations.
     * @param host Local bind address.
     * @param port Local bind port.
     * @param event_store_capacity Maximum number of replayable SSE events to retain.
     */
    HttpServerTransport(const boost::asio::any_io_executor& executor, std::string host,
                        unsigned short port, std::size_t event_store_capacity = 1024);

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
     * @brief Read the next queued JSON-RPC message from HTTP POST bodies.
     *
     * @return A task resolving to the next JSON-RPC payload.
     */
    Task<std::string> read_message() override;

    /**
     * @brief Store a JSON-RPC response for the pending HTTP request.
     *
     * @details Matches the response by JSON-RPC id and signals the waiting HTTP
     * handler through a per-request timer.
     *
     * @param message Serialized JSON-RPC response.
     * @return A task that completes when the response is stored.
     */
    Task<void> write_message(std::string_view message) override;

    /**
     * @brief Close the transport and stop accepting connections.
     */
    void close() override;

    /**
     * @brief Start accepting HTTP connections.
     *
     * @details Runs an accept loop and spawns one coroutine per TCP connection.
     *
     * @return A task that completes when the accept loop exits.
     */
    Task<void> listen();

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mcp

#pragma once

#include <mcp/core/export.hpp>
#include <mcp/transport/transport.hpp>

#include <nlohmann/json_fwd.hpp>

#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace boost::asio {
class any_io_executor;
}

namespace mcp {

namespace detail {
struct MemoryTransportState;
}

/**
 * @brief In-memory transport implementation for testing.
 *
 * MemoryTransport provides a pair of connected transports where messages
 * written to one are available for reading on the other. This is useful
 * for testing MCP server/client interactions without real I/O.
 *
 * Endpoint state is retained by pending operations, so destroying a
 * MemoryTransport wrapper does not invalidate an in-flight read or write.
 * Once the wrapper and its final in-flight operation are gone, the peer is
 * closed and its pending readers are woken.
 * Queue and pending-read state is serialized by an internal Boost.Asio strand.
 * Concurrent reads use independent wake-up operations and do not cancel each other.
 *
 * @note Always create instances via create_memory_transport_pair(). Never
 *       construct MemoryTransport directly unless set_peer() is called before use.
 */
class MCP_API MemoryTransport final : public ITransport {
   public:
    /**
     * @brief Constructs a MemoryTransport with the given executor.
     *
     * @param executor The executor for async operations.
     */
    explicit MemoryTransport(const boost::asio::any_io_executor& executor);
    ~MemoryTransport() override;

    MemoryTransport(const MemoryTransport&) = delete;
    MemoryTransport& operator=(const MemoryTransport&) = delete;
    MemoryTransport(MemoryTransport&&) = delete;
    MemoryTransport& operator=(MemoryTransport&&) = delete;

    /**
     * @brief Reads the next message from the internal queue.
     *
     * Suspends until a message is available or the transport is closed.
     *
     * @return A task yielding the next queued message.
     * @throws std::runtime_error If the transport is closed.
     */
    Task<std::string> read_message() override;

    /**
     * @brief Reads and parses the next message as JSON.
     *
     * @return A task yielding the next queued JSON value.
     * @throws std::runtime_error If the transport is closed.
     */
    Task<nlohmann::json> read_json();

    /**
     * @brief Writes a JSON value to the peer's queue.
     *
     * @param message The JSON value to send.
     * @return A task that completes when the peer queue has been updated.
     * @throws std::runtime_error If either endpoint is closed or the peer is unavailable.
     */
    Task<void> write_json(nlohmann::json message);

    /**
     * @brief Writes a message to the peer's queue and wakes the peer.
     *
     * The message bytes are copied before this function returns, so a caller may
     * safely modify or destroy the storage behind @p message before awaiting the task.
     *
     * @param message The message to send.
     * @return A task that completes when the peer queue has been updated.
     * @throws std::runtime_error If either endpoint is closed or the peer is unavailable.
     */
    Task<void> write_message(std::string_view message) override;

    /**
     * @brief Closes the transport and its peer.
     *
     * Safe to call concurrently and multiple times. Pending readers on both
     * endpoints are woken and future reads and writes fail.
     */
    void close() override;

    /**
     * @brief Sets the peer transport for bidirectional communication.
     *
     * @param peer Shared pointer to the peer MemoryTransport, or null to unlink it.
     */
    void set_peer(const std::shared_ptr<MemoryTransport>& peer);

   private:
    std::shared_ptr<detail::MemoryTransportState> state_;
};

/**
 * @brief Creates a pair of connected MemoryTransport instances.
 *
 * Messages written to the first transport are readable on the second,
 * and vice versa. Both transports share the same executor.
 *
 * The peer relationship uses weak ownership to avoid reference cycles.
 * Pending operations retain only the endpoint states they need.
 *
 * @param executor The executor for both transports.
 * @return A pair of connected transport instances that can exchange in-memory messages.
 */
MCP_API std::pair<std::shared_ptr<ITransport>, std::shared_ptr<ITransport>>
create_memory_transport_pair(const boost::asio::any_io_executor& executor);

}  // namespace mcp

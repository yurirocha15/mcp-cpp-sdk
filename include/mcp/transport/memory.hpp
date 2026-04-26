#pragma once

#include <mcp/transport.hpp>

#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <chrono>
#include <memory>
#include <queue>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace mcp {

/**
 * @brief In-memory transport implementation for testing.
 *
 * MemoryTransport provides a pair of connected transports where messages
 * written to one are available for reading on the other. This is useful
 * for testing MCP server/client interactions without real I/O.
 *
 * Thread-safety is achieved via boost::asio::strand (no mutexes needed).
 * The async pattern follows ScriptedTransport exactly: strand + timer + queue.
 *
 * @note Always create instances via create_memory_transport_pair(). Never
 *       construct MemoryTransport directly — the peer link would be unset.
 */
class MemoryTransport final : public ITransport {
   public:
    /**
     * @brief Constructs a MemoryTransport with the given executor.
     *
     * @param executor The executor for async operations.
     */
    explicit MemoryTransport(const boost::asio::any_io_executor& executor)
        : strand_(boost::asio::make_strand(executor)), timer_(strand_) {
        timer_.expires_at(std::chrono::steady_clock::time_point::max());
    }

    /**
     * @brief Reads the next message from the internal queue.
     *
     * Suspends using a timer until a message is available or the transport is closed.
     * Follows the exact pattern from ScriptedTransport.
     *
     * @return A task yielding the next queued message.
     * @throws std::runtime_error If the transport is closed.
     */
    Task<std::string> read_message() override {
        for (;;) {
            if (closed_) {
                throw std::runtime_error("transport closed");
            }
            if (!incoming_.empty()) {
                auto msg = std::move(incoming_.front());
                incoming_.pop();
                co_return msg;
            }
            timer_.expires_at(std::chrono::steady_clock::time_point::max());
            try {
                co_await timer_.async_wait(boost::asio::use_awaitable);
            } catch (const boost::system::system_error& err) {
                if (err.code() != boost::asio::error::operation_aborted) {
                    throw;
                }
            }
        }
    }

    /**
     * @brief Writes a message to the peer's queue and wakes the peer.
     *
     * @param message The message to send.
     * @return A task that completes when the peer queue has been updated.
     * @throws std::runtime_error If the transport is closed or the peer is not set or has been
     * destroyed.
     */
    Task<void> write_message(std::string_view message) override {
        co_await boost::asio::post(strand_, boost::asio::use_awaitable);
        if (closed_) {
            throw std::runtime_error("transport closed");
        }
        auto peer = peer_.lock();
        if (!peer) {
            throw std::runtime_error("peer transport not set");
        }
        // Post message to peer's strand
        boost::asio::post(peer->strand_, [peer, msg = std::string(message)]() mutable {
            peer->incoming_.push(std::move(msg));
            peer->timer_.cancel();
        });
    }

    /**
     * @brief Closes the transport and the peer.
     *
     * Sets closed flag, cancels pending timers, and closes the peer transport.
     */
    void close() override {
        if (closed_) {
            return;
        }
        closed_ = true;
        timer_.cancel();
        if (auto peer = peer_.lock(); peer && !peer->closed_) {
            peer->close();
        }
    }

    /**
     * @brief Sets the peer transport for bidirectional communication.
     *
     * @param peer Shared pointer to the peer MemoryTransport.
     */
    void set_peer(const std::shared_ptr<MemoryTransport>& peer) { peer_ = peer; }

   private:
    boost::asio::strand<boost::asio::any_io_executor> strand_;
    boost::asio::steady_timer timer_;
    std::queue<std::string> incoming_;
    std::weak_ptr<MemoryTransport> peer_;
    bool closed_ = false;
};

/**
 * @brief Creates a pair of connected MemoryTransport instances.
 *
 * Messages written to the first transport are readable on the second,
 * and vice versa. Both transports share the same executor.
 *
 * The peer relationship uses weak_ptr to avoid reference cycles; each
 * transport holds only a weak reference to the other.
 *
 * @param executor The executor for both transports.
 * @return A pair of connected transport instances that can exchange in-memory messages.
 */
inline std::pair<std::shared_ptr<ITransport>, std::shared_ptr<ITransport>> create_memory_transport_pair(
    const boost::asio::any_io_executor& executor) {
    auto transport_a = std::make_shared<MemoryTransport>(executor);
    auto transport_b = std::make_shared<MemoryTransport>(executor);
    transport_a->set_peer(transport_b);
    transport_b->set_peer(transport_a);
    return {transport_a, transport_b};
}

}  // namespace mcp

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
 */
class MemoryTransport final : public ITransport {
   public:
    /**
     * @brief Constructs a MemoryTransport with the given executor.
     *
     * @param executor The executor for async operations
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
     * @return Task yielding the next message
     * @throws std::runtime_error if transport is closed
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
     * @param message The message to send
     * @return Task that completes when message is enqueued
     * @throws std::runtime_error if transport is closed or peer is null
     */
    Task<void> write_message(std::string_view message) override {
        co_await boost::asio::post(strand_, boost::asio::use_awaitable);
        if (closed_) {
            throw std::runtime_error("transport closed");
        }
        if (!peer_) {
            throw std::runtime_error("peer transport not set");
        }
        // Post message to peer's strand
        boost::asio::post(peer_->strand_, [peer = peer_, msg = std::string(message)]() mutable {
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
        if (peer_ && !peer_->closed_) {
            peer_->close();
        }
    }

    /**
     * @brief Sets the peer transport for bidirectional communication.
     *
     * @param peer Shared pointer to the peer MemoryTransport
     */
    void set_peer(MemoryTransport* peer) { peer_ = peer; }

   private:
    boost::asio::strand<boost::asio::any_io_executor> strand_;
    boost::asio::steady_timer timer_;
    std::queue<std::string> incoming_;
    MemoryTransport* peer_ = nullptr;
    bool closed_ = false;
};

/**
 * @brief Creates a pair of connected MemoryTransport instances.
 *
 * Messages written to the first transport are readable on the second,
 * and vice versa. Both transports share the same executor.
 *
 * The returned unique_ptrs have custom deleters that properly manage
 * the shared ownership between the two transports.
 *
 * @param executor The executor for both transports
 * @return Pair of connected MemoryTransport unique pointers
 */
inline std::pair<std::unique_ptr<ITransport>, std::unique_ptr<ITransport>> create_memory_transport_pair(
    const boost::asio::any_io_executor& executor) {
    auto* transport_a = new MemoryTransport(executor);
    auto* transport_b = new MemoryTransport(executor);

    transport_a->set_peer(transport_b);
    transport_b->set_peer(transport_a);

    return {std::unique_ptr<ITransport>(transport_a), std::unique_ptr<ITransport>(transport_b)};
}

}  // namespace mcp

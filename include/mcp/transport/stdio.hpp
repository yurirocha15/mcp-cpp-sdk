#pragma once

#include <mcp/transport.hpp>

#include <atomic>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <iostream>
#include <memory>
#include <queue>
#include <string>
#include <string_view>
#include <thread>

namespace mcp {

/**
 * @brief Transport implementation for newline-delimited stdio communication.
 *
 * @details Reads JSON-RPC messages line-by-line from an input stream and writes
 * responses to an output stream. A dedicated reader thread blocks on the input
 * stream and posts messages into the boost::asio event loop. Writes are
 * serialized through a boost::asio::strand.
 *
 * By default uses std::cin / std::cout, but accepts arbitrary streams for
 * testing.
 */
class StdioTransport final : public ITransport {
   public:
    /**
     * @brief Construct a StdioTransport.
     *
     * @param executor The executor to use for async operations.
     * @param input    Input stream to read messages from (default: std::cin).
     * @param output   Output stream to write messages to (default: std::cout).
     */
    explicit StdioTransport(const boost::asio::any_io_executor& executor,
                            std::istream& input = std::cin, std::ostream& output = std::cout)
        : output_(output),
          strand_(boost::asio::make_strand(executor)),
          state_(std::make_shared<SharedState>(strand_)) {
        state_->timer.expires_at(std::chrono::steady_clock::time_point::max());
        start_reader(input);
    }

    ~StdioTransport() override {
        state_->closed.store(true, std::memory_order_release);
        if (reader_thread_.joinable()) {
            reader_thread_.join();
        }
    }

    StdioTransport(const StdioTransport&) = delete;
    StdioTransport& operator=(const StdioTransport&) = delete;
    StdioTransport(StdioTransport&&) = delete;
    StdioTransport& operator=(StdioTransport&&) = delete;

    /**
     * @brief Read the next newline-delimited message from the input stream.
     *
     * @details Suspends the calling coroutine until a complete line is
     * available. Throws std::runtime_error if the transport is closed or
     * the input stream reaches EOF.
     *
     * @return A task that resolves to the next message string.
     */
    Task<std::string> read_message() override {
        auto& state = *state_;
        for (;;) {
            if (!state.queue.empty()) {
                auto msg = std::move(state.queue.front());
                state.queue.pop();
                co_return msg;
            }

            if (state.closed.load(std::memory_order_acquire)) {
                throw std::runtime_error("StdioTransport is closed");
            }

            // Suspend until reader thread cancels the timer with a new message.
            state.timer.expires_at(std::chrono::steady_clock::time_point::max());
            try {
                co_await state.timer.async_wait(boost::asio::use_awaitable);
            } catch (const boost::system::system_error& err) {
                if (err.code() != boost::asio::error::operation_aborted) {
                    throw;
                }
            }
        }
    }

    /**
     * @brief Write a message followed by a newline to the output stream.
     *
     * @details Writes are serialized through a strand so concurrent calls
     * are safe without a mutex.
     *
     * @param message The message to write.
     */
    Task<void> write_message(std::string_view message) override {
        co_await boost::asio::post(strand_, boost::asio::use_awaitable);
        output_ << message << '\n';
        output_.flush();
        co_return;
    }

    /**
     * @brief Close the transport.
     *
     * @details Sets the closed flag, preventing further message processing.
     * Safe to call multiple times.
     */
    void close() override { state_->closed.store(true, std::memory_order_release); }

   private:
    // Shared state outlives the transport when the reader thread is detached.
    struct SharedState {
        explicit SharedState(boost::asio::strand<boost::asio::any_io_executor>& strand)
            : timer(strand) {}

        boost::asio::steady_timer timer;
        std::queue<std::string> queue;
        std::atomic<bool> closed{false};
    };

    void start_reader(std::istream& input) {
        auto state = state_;
        reader_thread_ = std::thread([&input, state]() {
            std::string line;
            while (std::getline(input, line)) {
                if (state->closed.load(std::memory_order_acquire)) {
                    break;
                }
                auto msg = std::move(line);
                boost::asio::post(state->timer.get_executor(), [state, m = std::move(msg)]() mutable {
                    state->queue.push(std::move(m));
                    state->timer.cancel();
                });
            }
            boost::asio::post(state->timer.get_executor(), [state]() {
                state->closed.store(true, std::memory_order_release);
                state->timer.cancel();
            });
        });
    }

    std::ostream& output_;
    boost::asio::strand<boost::asio::any_io_executor> strand_;
    std::shared_ptr<SharedState> state_;
    std::thread reader_thread_;
};

}  // namespace mcp

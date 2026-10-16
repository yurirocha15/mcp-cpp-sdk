#include <mcp/transport/stdio.hpp>

#include <atomic>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/system_error.hpp>
#include <chrono>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace mcp {

struct StdioTransport::Impl {
    struct SharedState {
        SharedState(const boost::asio::any_io_executor& executor, std::ostream& output)
            : output(output), strand(boost::asio::make_strand(executor)), read_signal(strand) {
            read_signal.expires_at(std::chrono::steady_clock::time_point::max());
        }

        void wake_reader() noexcept {
            boost::system::error_code ignored;
            read_signal.cancel(ignored);
        }

        std::ostream& output;
        boost::asio::strand<boost::asio::any_io_executor> strand;
        boost::asio::steady_timer read_signal;
        std::queue<std::string> queue;
        bool read_pending{false};
        bool input_ended{false};
        std::atomic<bool> closed{false};
        // A custom stream buffer can call close() from inside write/flush.
        // Recursive locking keeps that re-entrant close from deadlocking.
        std::recursive_mutex output_mutex;
    };

    Impl(const boost::asio::any_io_executor& executor, std::istream& input, std::ostream& output)
        : input(input), state(std::make_shared<SharedState>(executor, output)) {}

    ~Impl() {
        try {
            close_state(state);
        } catch (...) {
            state->closed.store(true, std::memory_order_release);
        }
        join_reader();
    }

    std::shared_ptr<SharedState> ensure_reader_started() {
        auto shared_state = state;
        if (shared_state->closed.load(std::memory_order_acquire)) {
            return shared_state;
        }

        std::lock_guard lock(reader_mutex);
        if (!reader_started && !shared_state->closed.load(std::memory_order_acquire)) {
            reader_started = true;
            reader_thread = std::thread([&in = input, shared_state]() {
                try {
                    std::string line;
                    while (std::getline(in, line)) {
                        if (shared_state->closed.load(std::memory_order_acquire)) {
                            break;
                        }

                        boost::asio::post(shared_state->strand,
                                          [shared_state, message = std::move(line)]() mutable {
                                              shared_state->queue.push(std::move(message));
                                              shared_state->wake_reader();
                                          });
                    }
                } catch (...) {
                    // Stream failures are reported to readers as end-of-input.
                }

                boost::asio::post(shared_state->strand, [shared_state]() {
                    shared_state->input_ended = true;
                    shared_state->wake_reader();
                });
            });
        }
        return shared_state;
    }

    static void close_state(const std::shared_ptr<SharedState>& shared_state) {
        const bool first_close = !shared_state->closed.exchange(true, std::memory_order_acq_rel);

        // A write that already acquired the output cannot outlive close(). A
        // write queued behind this barrier re-checks closed before touching the
        // caller-owned stream.
        std::unique_lock output_barrier(shared_state->output_mutex);
        output_barrier.unlock();

        if (first_close) {
            boost::asio::post(shared_state->strand, [shared_state]() { shared_state->wake_reader(); });
        }
    }

    void join_reader() noexcept {
        std::thread reader;
        {
            std::lock_guard lock(reader_mutex);
            reader = std::move(reader_thread);
        }
        if (reader.joinable()) {
            reader.join();
        }
    }

    static Task<std::string> read_on_strand(std::shared_ptr<SharedState> shared_state) {
        if (shared_state->read_pending) {
            throw std::logic_error("StdioTransport supports only one outstanding read");
        }

        struct ReadReservation {
            explicit ReadReservation(SharedState& state) : state(state) { state.read_pending = true; }
            ~ReadReservation() { state.read_pending = false; }

            SharedState& state;
        } reservation(*shared_state);

        for (;;) {
            if (shared_state->closed.load(std::memory_order_acquire)) {
                throw std::runtime_error("StdioTransport is closed");
            }

            if (!shared_state->queue.empty()) {
                auto message = std::move(shared_state->queue.front());
                shared_state->queue.pop();
                co_return message;
            }

            if (shared_state->input_ended) {
                throw std::runtime_error("StdioTransport is closed");
            }

            shared_state->read_signal.expires_at(std::chrono::steady_clock::time_point::max());
            boost::system::error_code error;
            co_await shared_state->read_signal.async_wait(
                boost::asio::redirect_error(boost::asio::use_awaitable, error));
            if (error && error != boost::asio::error::operation_aborted) {
                throw boost::system::system_error(error);
            }
        }
    }

    static Task<void> write_on_strand(std::shared_ptr<SharedState> shared_state, std::string message) {
        if (shared_state->closed.load(std::memory_order_acquire)) {
            throw std::runtime_error("StdioTransport is closed");
        }

        std::lock_guard lock(shared_state->output_mutex);
        if (shared_state->closed.load(std::memory_order_acquire)) {
            throw std::runtime_error("StdioTransport is closed");
        }

        shared_state->output.write(message.data(), static_cast<std::streamsize>(message.size()));
        shared_state->output.put('\n');
        shared_state->output.flush();
        if (!shared_state->output) {
            throw std::runtime_error("StdioTransport failed to write to the output stream");
        }
        co_return;
    }

    std::istream& input;
    std::shared_ptr<SharedState> state;
    std::mutex reader_mutex;
    bool reader_started{false};
    std::thread reader_thread;
};

StdioTransport::StdioTransport(const boost::asio::any_io_executor& executor, std::istream& input,
                               std::ostream& output)
    : impl_(std::make_unique<Impl>(executor, input, output)) {}

StdioTransport::~StdioTransport() = default;

Task<std::string> StdioTransport::read_message() {
    auto state = impl_->ensure_reader_started();
    return boost::asio::co_spawn(state->strand, Impl::read_on_strand(state),
                                 boost::asio::use_awaitable);
}

Task<void> StdioTransport::write_message(std::string_view message) {
    auto state = impl_->state;
    auto owned_message = std::string(message);
    return boost::asio::co_spawn(state->strand, Impl::write_on_strand(state, std::move(owned_message)),
                                 boost::asio::use_awaitable);
}

void StdioTransport::close() {
    auto state = impl_->state;
    Impl::close_state(state);
}

}  // namespace mcp

#include <mcp/transport/stdio.hpp>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#include <atomic>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/system_error.hpp>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <memory>
#include <mutex>
#include <ostream>
#include <queue>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <thread>
#include <utility>

namespace mcp {
namespace {

constexpr int kStdoutDescriptor = 1;
constexpr int kStderrDescriptor = 2;

#if defined(_WIN32)
int duplicate_descriptor(int descriptor) { return ::_dup(descriptor); }
int replace_descriptor(int source, int target) { return ::_dup2(source, target); }
int release_descriptor(int descriptor) { return ::_close(descriptor); }
int open_null_device() { return ::_open("NUL", _O_WRONLY); }
std::ptrdiff_t write_descriptor(int descriptor, const char* data, std::size_t size) {
    return ::_write(descriptor, data, static_cast<unsigned int>(size));
}
#else
int duplicate_descriptor(int descriptor) { return ::dup(descriptor); }
int replace_descriptor(int source, int target) { return ::dup2(source, target); }
int release_descriptor(int descriptor) { return ::close(descriptor); }
int open_null_device() { return ::open("/dev/null", O_WRONLY); }
std::ptrdiff_t write_descriptor(int descriptor, const char* data, std::size_t size) {
    return ::write(descriptor, data, size);
}
#endif

/// Writes straight through to a file descriptor. The transport flushes after
/// every message, so a buffer here would only add a second place for half a
/// message to sit.
class DescriptorStreambuf final : public std::streambuf {
   public:
    explicit DescriptorStreambuf(int descriptor) : descriptor_(descriptor) {}

   protected:
    std::streamsize xsputn(const char_type* data, std::streamsize size) override {
        std::streamsize written = 0;
        while (written < size) {
            const auto result =
                write_descriptor(descriptor_, data + written, static_cast<std::size_t>(size - written));
            if (result < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return written;
            }
            if (result == 0) {
                return written;
            }
            written += static_cast<std::streamsize>(result);
        }
        return written;
    }

    int_type overflow(int_type value) override {
        if (traits_type::eq_int_type(value, traits_type::eof())) {
            return traits_type::not_eof(value);
        }
        const char_type byte = traits_type::to_char_type(value);
        return xsputn(&byte, 1) == 1 ? value : traits_type::eof();
    }

   private:
    int descriptor_;
};

/// Hands the protocol stream to the transport alone.
///
/// Duplicating standard output is not enough by itself: a duplicate shares the
/// same open file description, so a stray printf still lands in the same byte
/// stream, and its separate buffer can now flush in the middle of a framed
/// message instead of between two of them. The application's standard output
/// has to point somewhere else, and its diagnostics belong on standard error.
class OwnedStdout {
   public:
    OwnedStdout() : descriptor_(acquire()), buffer_(descriptor_), stream_(&buffer_) {}

    ~OwnedStdout() {
        // Whatever the application buffered belongs on the redirected stream,
        // not in the protocol once standard output is handed back.
        std::cout.flush();
        std::fflush(stdout);

        replace_descriptor(descriptor_, kStdoutDescriptor);
        release_descriptor(descriptor_);
        owner_active().store(false, std::memory_order_release);
    }

    OwnedStdout(const OwnedStdout&) = delete;
    OwnedStdout& operator=(const OwnedStdout&) = delete;
    OwnedStdout(OwnedStdout&&) = delete;
    OwnedStdout& operator=(OwnedStdout&&) = delete;

    std::ostream& stream() noexcept { return stream_; }

   private:
    static std::atomic<bool>& owner_active() {
        static std::atomic<bool> active{false};
        return active;
    }

    static int acquire() {
        bool unowned = false;
        if (!owner_active().compare_exchange_strong(unowned, true, std::memory_order_acq_rel)) {
            throw std::runtime_error(
                "StdioTransport: another transport already owns this process's standard output");
        }

        // Anything already queued for standard output belongs on the real
        // standard output, ahead of the first framed message.
        std::cout.flush();
        std::fflush(stdout);

        const int descriptor = duplicate_descriptor(kStdoutDescriptor);
        if (descriptor < 0) {
            owner_active().store(false, std::memory_order_release);
            throw std::runtime_error(
                "StdioTransport could not duplicate this process's standard output");
        }

        if (replace_descriptor(kStderrDescriptor, kStdoutDescriptor) < 0) {
            // A process started with standard error closed still needs its
            // standard output kept off the protocol stream.
            const int null_device = open_null_device();
            const bool diverted =
                null_device >= 0 && replace_descriptor(null_device, kStdoutDescriptor) >= 0;
            if (null_device >= 0) {
                release_descriptor(null_device);
            }
            if (!diverted) {
                release_descriptor(descriptor);
                owner_active().store(false, std::memory_order_release);
                throw std::runtime_error(
                    "StdioTransport could not redirect this process's standard output away from "
                    "the protocol stream");
            }
        }

        return descriptor;
    }

    int descriptor_;
    DescriptorStreambuf buffer_;
    std::ostream stream_;
};

}  // namespace

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

    Impl(const boost::asio::any_io_executor& executor, std::istream& input, OwnStdoutTag)
        : owned_output(std::make_unique<OwnedStdout>()),
          input(input),
          state(std::make_shared<SharedState>(executor, owned_output->stream())) {}

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

    // Declared first so it outlives `state`, which holds a reference into it,
    // and so standard output is handed back only after the last write.
    std::unique_ptr<OwnedStdout> owned_output;
    std::istream& input;
    std::shared_ptr<SharedState> state;
    std::mutex reader_mutex;
    bool reader_started{false};
    std::thread reader_thread;
};

StdioTransport::StdioTransport(const boost::asio::any_io_executor& executor, std::istream& input,
                               std::ostream& output)
    : impl_(std::make_unique<Impl>(executor, input, output)) {}

StdioTransport::StdioTransport(const boost::asio::any_io_executor& executor, std::istream& input,
                               OwnStdoutTag tag)
    : impl_(std::make_unique<Impl>(executor, input, tag)) {}

std::unique_ptr<StdioTransport> StdioTransport::create_owning_stdout(
    const boost::asio::any_io_executor& executor, std::istream& input) {
    return std::unique_ptr<StdioTransport>(new StdioTransport(executor, input, OwnStdoutTag{}));
}

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

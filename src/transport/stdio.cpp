#include <mcp/transport/stdio.hpp>
#include "../detail/spawn_awaitable.hpp"

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#endif

#include <array>
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
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
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
#include <vector>

#if defined(__GLIBCXX__)
#include <ext/stdio_filebuf.h>
#endif

namespace mcp {
namespace {

constexpr int kStdinDescriptor = 0;
constexpr int kStdoutDescriptor = 1;
constexpr int kStderrDescriptor = 2;
// The protocol's copy of stdout must not take one of the standard descriptors.
// In a process started with stderr closed, 2 is the lowest free descriptor, and
// pointing stdout at stderr would then point it back at the protocol stream.
constexpr int kFirstPrivateDescriptor = 3;

/// The WebSocket transports' default limit, so a message fits every transport or none.
constexpr std::size_t g_default_max_message_bytes = 16U * 1024U * 1024U;
constexpr std::size_t g_read_chunk_bytes = 64U * 1024U;
/// Input is read ahead of read_message() up to this many bytes.
constexpr std::size_t g_max_read_ahead_bytes = 16U * 1024U * 1024U;
/// What an empty line costs in the queue, so short lines cannot fill memory unnoticed.
constexpr std::size_t g_input_line_overhead = 64;
/// How long the destructor waits for accepted writes to reach a peer that is reading.
constexpr std::chrono::seconds g_final_flush_wait{1};

enum class LineResult : std::uint8_t {
    eLine,      ///< A line ended by a newline.
    eLastLine,  ///< A line ended by the end of the input.
    eEnd,       ///< The input ended, or failed, with nothing to deliver.
    eTooLong,   ///< The line is over the limit.
};

#if defined(_WIN32)
// _dup() takes the lowest free descriptor, so set aside any it hands out below
// kFirstPrivateDescriptor and keep asking.
int duplicate_descriptor(int descriptor) {
    int standard[kFirstPrivateDescriptor];
    int held = 0;
    int duplicate = ::_dup(descriptor);
    while (duplicate >= 0 && duplicate < kFirstPrivateDescriptor) {
        standard[held++] = duplicate;
        duplicate = ::_dup(descriptor);
    }
    while (held > 0) {
        ::_close(standard[--held]);
    }
    return duplicate;
}
int replace_descriptor(int source, int target) { return ::_dup2(source, target); }
int release_descriptor(int descriptor) { return ::_close(descriptor); }
int open_null_device() { return ::_open("NUL", _O_WRONLY); }
std::ptrdiff_t read_descriptor(int descriptor, char* data, std::size_t size) {
    return ::_read(descriptor, data, static_cast<unsigned int>(size));
}
std::ptrdiff_t write_descriptor(int descriptor, const char* data, std::size_t size) {
    return ::_write(descriptor, data, static_cast<unsigned int>(size));
}
#else
int duplicate_descriptor(int descriptor) {
    return ::fcntl(descriptor, F_DUPFD_CLOEXEC, kFirstPrivateDescriptor);
}
int replace_descriptor(int source, int target) { return ::dup2(source, target); }
int release_descriptor(int descriptor) { return ::close(descriptor); }
int open_null_device() { return ::open("/dev/null", O_WRONLY); }
std::ptrdiff_t read_descriptor(int descriptor, char* data, std::size_t size) {
    return ::read(descriptor, data, size);
}
std::ptrdiff_t write_descriptor(int descriptor, const char* data, std::size_t size) {
    return ::write(descriptor, data, size);
}
#endif

/// Keeps a write to a closed pipe on @p descriptor from raising SIGPIPE. macOS raises it on the whole
/// process, not on the writing thread, so the writer's blocked signal (see write_lines()) would not
/// stop it there. The open file is set not to raise it, which every descriptor sharing it sees.
void suppress_pipe_signal([[maybe_unused]] int descriptor) {
#if defined(__APPLE__)
    ::fcntl(descriptor, F_SETNOSIGPIPE, 1);
#endif
}

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

/// The buffers the process's standard streams started with. A program can replace them, and then
/// std::cin and std::cout no longer stand for the descriptors.
std::streambuf* const g_process_stdin_buffer = std::cin.rdbuf();
std::streambuf* const g_process_stdout_buffer = std::cout.rdbuf();

/// Whether @p buffer is the process's own buffer for @p descriptor: the one the stream started with,
/// or the one libstdc++ puts in its place when the program calls std::ios::sync_with_stdio(false).
bool is_process_buffer(std::streambuf* buffer, std::streambuf* original, int descriptor) {
    if (buffer == original) {
        return true;
    }
#if defined(__GLIBCXX__)
    auto* file_buffer = dynamic_cast<__gnu_cxx::stdio_filebuf<char>*>(buffer);
    return file_buffer != nullptr && file_buffer->fd() == descriptor;
#else
    static_cast<void>(descriptor);
    return false;
#endif
}

/// Standard output as a stream of its own. std::cout writes through the C library, whose lock is
/// held for the whole of a blocked write, and exit() takes that lock to flush: a peer that stopped
/// reading would keep the process from ever exiting.
class RawStdout {
   public:
    RawStdout() : buffer_(kStdoutDescriptor), stream_(&buffer_) {
        std::cout.flush();
        std::fflush(stdout);
        suppress_pipe_signal(kStdoutDescriptor);
    }

    std::ostream& stream() noexcept { return stream_; }

   private:
    DescriptorStreambuf buffer_;
    std::ostream stream_;
};

/// Reads lines from standard input directly. std::cin reads through the C library, which holds its
/// lock for the whole of a blocked read, so exit() would wait for the peer's next line. Here the
/// wait also watches @p wake, which lets close() end it.
class DescriptorLines {
   public:
    DescriptorLines(int descriptor, int wake)
        : descriptor_(descriptor), wake_(wake), chunk_(g_read_chunk_bytes) {}

    LineResult next(std::string& line, std::size_t limit) {
        line.clear();
        for (;;) {
            if (begin_ == end_ && !fill()) {
                return line.empty() ? LineResult::eEnd : LineResult::eLastLine;
            }
            const char* start = chunk_.data() + begin_;
            const auto available = end_ - begin_;
            const auto* newline = static_cast<const char*>(std::memchr(start, '\n', available));
            const auto taken =
                newline != nullptr ? static_cast<std::size_t>(newline - start) : available;
            if (line.size() + taken > limit) {
                return LineResult::eTooLong;
            }
            line.append(start, taken);
            begin_ += taken + (newline != nullptr ? 1 : 0);
            if (newline != nullptr) {
                return LineResult::eLine;
            }
        }
    }

   private:
    /// False at the end of the input, on a read that failed, and when woken.
    bool fill() {
#if !defined(_WIN32)
        for (;;) {
            std::array<pollfd, 2> watched{pollfd{descriptor_, POLLIN, 0}, pollfd{wake_, POLLIN, 0}};
            const nfds_t count = wake_ >= 0 ? 2 : 1;
            if (::poll(watched.data(), count, -1) < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return false;
            }
            if (wake_ >= 0 && watched[1].revents != 0) {
                return false;
            }
            break;
        }
#endif
        for (;;) {
            const auto result = read_descriptor(descriptor_, chunk_.data(), chunk_.size());
            if (result < 0 && errno == EINTR) {
                continue;
            }
            if (result <= 0) {
                return false;
            }
            begin_ = 0;
            end_ = static_cast<std::size_t>(result);
            return true;
        }
    }

    int descriptor_;
    int wake_;
    std::vector<char> chunk_;
    std::size_t begin_{0};
    std::size_t end_{0};
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
        suppress_pipe_signal(descriptor);

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
    struct SharedState;

    /// Completes one write on the strand; the write's bytes may already be gone by then.
    struct WriteWaiter {
        explicit WriteWaiter(const boost::asio::any_io_executor& executor) : signal(executor) {
            signal.expires_at(std::chrono::steady_clock::time_point::max());
        }

        boost::asio::steady_timer signal;
        bool done{false};
        bool written{false};
    };

    struct Outgoing {
        std::string message;
        std::shared_ptr<WriteWaiter> waiter;
    };

    /// What the reader and writer threads share with the transport. Only this object may outlive
    /// the transport: a thread blocked on the process's standard streams cannot be interrupted, and
    /// state holds timers that must not outlive the io_context. Everything is guarded by `mutex`.
    struct Pump {
        std::mutex mutex;
        std::condition_variable wake;
        /// Null once the transport is gone. The threads then only finish what they are doing.
        std::shared_ptr<SharedState> state;
        std::deque<Outgoing> outgoing;
        std::shared_ptr<WriteWaiter> writing_waiter;
        bool writing{false};
        bool stop_writing{false};
        bool reader_done{false};
        bool writer_done{false};
        std::thread::id writer_id;
        /// Bytes of input read ahead of what read_message() took.
        std::size_t queued_input_bytes{0};
        std::size_t max_message_bytes{g_default_max_message_bytes};
        /// A pipe that ends a wait on standard input; -1 when there is none.
        int wake_read{-1};
        int wake_write{-1};

        Pump() = default;
        Pump(const Pump&) = delete;
        Pump& operator=(const Pump&) = delete;
        Pump(Pump&&) = delete;
        Pump& operator=(Pump&&) = delete;
        ~Pump() {
#if !defined(_WIN32)
            if (wake_read >= 0) {
                ::close(wake_read);
                ::close(wake_write);
            }
#endif
        }

        /// Opens the pipe before the reader starts. @p closed tells whether close() has already
        /// run; it found no pipe to wake then, so the pipe starts out woken.
        void open_wake_pipe(const std::atomic<bool>& closed) {
#if !defined(_WIN32)
            std::array<int, 2> ends{-1, -1};
            if (::pipe(ends.data()) == 0) {
                ::fcntl(ends[0], F_SETFD, FD_CLOEXEC);
                ::fcntl(ends[1], F_SETFD, FD_CLOEXEC);
                std::lock_guard lock(mutex);
                wake_read = ends[0];
                wake_write = ends[1];
                if (closed.load(std::memory_order_acquire)) {
                    wake_input_wait();
                }
            }
#else
            static_cast<void>(closed);
#endif
        }

        void wake_input_wait() const noexcept {
#if !defined(_WIN32)
            if (wake_write >= 0) {
                const char byte = 0;
                (void)!::write(wake_write, &byte, 1);
            }
#endif
        }

        [[nodiscard]] bool output_drained() const { return outgoing.empty() && !writing; }
    };

    struct SharedState {
        SharedState(const boost::asio::any_io_executor& executor, std::ostream& output)
            : output(output),
              strand(boost::asio::make_strand(executor)),
              read_signal(strand),
              pump(std::make_shared<Pump>()) {
            read_signal.expires_at(std::chrono::steady_clock::time_point::max());
        }

        void wake_reader() noexcept { read_signal.cancel(); }

        std::ostream& output;
        boost::asio::strand<boost::asio::any_io_executor> strand;
        boost::asio::steady_timer read_signal;
        std::queue<std::string> queue;
        bool read_pending{false};
        bool input_ended{false};
        std::string input_error;
        std::atomic<bool> closed{false};
        /// Standard output can stall for as long as the peer likes, and it outlives the transport,
        /// so nothing waits for it. A caller's stream must not be touched after close().
        bool output_may_stall{false};
        std::shared_ptr<Pump> pump;
    };

    Impl(const boost::asio::any_io_executor& executor, std::istream& input, std::ostream& output)
        : raw_stdout(is_process_stdout(output) ? std::make_shared<RawStdout>() : nullptr),
          input(input),
          state(std::make_shared<SharedState>(executor, raw_stdout ? raw_stdout->stream() : output)) {
        state->output_may_stall = raw_stdout != nullptr;
        state->pump->state = state;
    }

    Impl(const boost::asio::any_io_executor& executor, std::istream& input, OwnStdoutTag)
        : owned_output(std::make_shared<OwnedStdout>()),
          input(input),
          state(std::make_shared<SharedState>(executor, owned_output->stream())) {
        state->output_may_stall = true;
        state->pump->state = state;
    }

    static bool is_process_stdout(const std::ostream& output) {
        return &output == &std::cout &&
               is_process_buffer(output.rdbuf(), g_process_stdout_buffer, kStdoutDescriptor);
    }

    [[nodiscard]] bool reads_process_stdin() const {
        return &input == &std::cin &&
               is_process_buffer(input.rdbuf(), g_process_stdin_buffer, kStdinDescriptor);
    }

    /// Whether the reader can take standard input from the descriptor. Input still buffered in a
    /// replaced stream buffer would be skipped that way. Called before the reader starts.
    [[nodiscard]] bool reads_stdin_descriptor() const {
        return reads_process_stdin() &&
               (input.rdbuf() == g_process_stdin_buffer || input.rdbuf()->in_avail() <= 0);
    }

    ~Impl() {
        try {
            close_state(state);
        } catch (...) {
            state->closed.store(true, std::memory_order_release);
        }
        auto pump = state->pump;
        bool reader_stuck = false;
        bool writer_stuck = false;
        {
            std::unique_lock lock(pump->mutex);
            if (state->output_may_stall) {
                pump->wake.wait_for(lock, g_final_flush_wait,
                                    [&pump] { return pump->output_drained(); });
                writer_stuck = !pump->output_drained();
            }
            // Without a wake pipe nothing can end a wait on standard input, and the descriptor
            // outlives every thread. No other stream may be touched after this.
            reader_stuck = reads_process_stdin() && pump->wake_read < 0 && !pump->reader_done;
            pump->state.reset();
            pump->wake.notify_all();
        }
        end_thread(reader_thread, reader_stuck);
        end_thread(writer_thread, writer_stuck);
    }

    static void end_thread(std::thread& thread, bool stuck) noexcept {
        if (!thread.joinable()) {
            return;
        }
        if (stuck) {
            thread.detach();
        } else {
            thread.join();
        }
    }

    /// Reads one line into @p line, as std::getline does but refusing one above @p limit bytes.
    static LineResult read_line(std::istream& in, std::string& line, std::vector<char>& chunk,
                                std::size_t limit) {
        line.clear();
        const auto capacity = static_cast<std::streamsize>(chunk.size());
        for (;;) {
            in.getline(chunk.data(), capacity);
            const auto extracted = in.gcount();
            const bool chunk_full = in.fail() && !in.eof() && extracted == capacity - 1;
            const bool delimited = in.good();
            const auto stored = static_cast<std::size_t>(delimited ? extracted - 1 : extracted);
            if (line.size() + stored > limit) {
                return LineResult::eTooLong;
            }
            line.append(chunk.data(), stored);
            if (delimited) {
                return LineResult::eLine;
            }
            if (!chunk_full) {
                return stored > 0 ? LineResult::eLastLine : LineResult::eEnd;
            }
            in.clear();
        }
    }

    template <typename NextLine>
    static void read_lines(NextLine next_line, const std::shared_ptr<Pump>& pump) noexcept {
        std::string failure;
        try {
            std::string line;
            bool more = true;
            while (more) {
                const auto limit = [&pump] {
                    std::lock_guard lock(pump->mutex);
                    return pump->max_message_bytes;
                }();
                const auto result = next_line(line, limit);
                if (result == LineResult::eEnd) {
                    break;
                }
                if (result == LineResult::eTooLong) {
                    failure = "StdioTransport input line exceeds " + std::to_string(limit) + " bytes";
                    break;
                }
                more = result == LineResult::eLine;

                std::unique_lock lock(pump->mutex);
                // Whoever reads the transport may have stopped, and a peer can keep sending.
                pump->wake.wait(lock, [&pump] {
                    return !pump->state || pump->state->closed.load(std::memory_order_acquire) ||
                           pump->queued_input_bytes < g_max_read_ahead_bytes;
                });
                auto shared_state = pump->state;
                if (!shared_state || shared_state->closed.load(std::memory_order_acquire)) {
                    break;
                }
                pump->queued_input_bytes += line.size() + g_input_line_overhead;
                boost::asio::post(shared_state->strand,
                                  [shared_state, message = std::move(line)]() mutable {
                                      shared_state->queue.push(std::move(message));
                                      shared_state->wake_reader();
                                  });
                line = std::string();
            }
        } catch (...) {
            // Stream failures are reported to readers as end-of-input.
        }

        std::lock_guard lock(pump->mutex);
        pump->reader_done = true;
        if (auto shared_state = pump->state) {
            boost::asio::post(shared_state->strand,
                              [shared_state, failure = std::move(failure)]() mutable {
                                  shared_state->input_ended = true;
                                  shared_state->input_error = std::move(failure);
                                  shared_state->wake_reader();
                              });
        }
        pump->wake.notify_all();
    }

    std::shared_ptr<SharedState> ensure_reader_started() {
        auto shared_state = state;
        if (shared_state->closed.load(std::memory_order_acquire)) {
            return shared_state;
        }

        std::lock_guard lock(threads_mutex);
        if (!reader_started && !shared_state->closed.load(std::memory_order_acquire)) {
            reader_started = true;
            if (reads_stdin_descriptor()) {
                shared_state->pump->open_wake_pipe(shared_state->closed);
                reader_thread = std::thread([pump = shared_state->pump]() {
                    DescriptorLines lines(kStdinDescriptor, pump->wake_read);
                    read_lines([&lines](std::string& line,
                                        std::size_t limit) { return lines.next(line, limit); },
                               pump);
                });
            } else {
                reader_thread = std::thread([&in = input, pump = shared_state->pump]() {
                    std::vector<char> chunk(g_read_chunk_bytes);
                    read_lines(
                        [&in, &chunk](std::string& line, std::size_t limit) {
                            return read_line(in, line, chunk, limit);
                        },
                        pump);
                });
            }
        }
        return shared_state;
    }

    /// Reports @p waiter's write finished, on the strand. Takes `pump.mutex` being held.
    static void complete_write(const std::shared_ptr<SharedState>& shared_state,
                               std::shared_ptr<WriteWaiter> waiter, bool written) {
        boost::asio::post(shared_state->strand, [waiter = std::move(waiter), written]() {
            waiter->done = true;
            waiter->written = written;
            waiter->signal.cancel();
        });
    }

    static void write_lines(std::ostream& output, const std::shared_ptr<Pump>& pump) noexcept {
#if !defined(_WIN32)
        // A reader that went away is a failed write here, not a signal that ends the process. The
        // signal stays pending on this thread, so nothing else in the process changes; on macOS
        // the standard output descriptor does not raise it at all (see suppress_pipe_signal()).
        sigset_t pipe_signal;
        sigemptyset(&pipe_signal);
        sigaddset(&pipe_signal, SIGPIPE);
        pthread_sigmask(SIG_BLOCK, &pipe_signal, nullptr);
#endif
        std::unique_lock lock(pump->mutex);
        pump->writer_id = std::this_thread::get_id();
        for (;;) {
            pump->wake.wait(lock, [&pump] { return pump->stop_writing || !pump->outgoing.empty(); });
            if (pump->outgoing.empty()) {
                break;
            }
            auto item = std::move(pump->outgoing.front());
            pump->outgoing.pop_front();
            pump->writing = true;
            pump->writing_waiter = std::move(item.waiter);
            lock.unlock();

            bool written = false;
            try {
                output.write(item.message.data(), static_cast<std::streamsize>(item.message.size()));
                output.put('\n');
                output.flush();
                written = static_cast<bool>(output);
            } catch (...) {
                // A stream set to throw fails the same way as one that does not.
                written = false;
            }

            lock.lock();
            pump->writing = false;
            auto waiter = std::move(pump->writing_waiter);
            pump->writing_waiter.reset();
            if (pump->state && waiter) {
                complete_write(pump->state, std::move(waiter), written);
            }
            if (!written && pump->state) {
                // The peer is no longer reading: nothing more can be answered, so the transport
                // closes and the reader stops instead of starting work whose answers would be lost.
                boost::asio::post(pump->state->strand,
                                  [shared_state = pump->state] { close_state(shared_state); });
            }
            pump->wake.notify_all();
        }
        pump->writer_done = true;
        pump->wake.notify_all();
    }

    void ensure_writer_started() {
        std::lock_guard lock(threads_mutex);
        if (!writer_started) {
            writer_started = true;
            writer_thread =
                std::thread([&output = state->output, pump = state->pump, keep_owned = owned_output,
                             keep_raw = raw_stdout]() { write_lines(output, pump); });
        }
    }

    static void close_state(const std::shared_ptr<SharedState>& shared_state) {
        const bool first_close = !shared_state->closed.exchange(true, std::memory_order_acq_rel);
        auto& pump = *shared_state->pump;

        {
            std::unique_lock lock(pump.mutex);
            pump.stop_writing = true;
            pump.wake.notify_all();
            pump.wake_input_wait();
            if (shared_state->output_may_stall) {
                // What was accepted is still written, but no one waits for a peer that stopped
                // reading: a handler blocked here would hold up shutdown.
                for (auto& item : pump.outgoing) {
                    if (item.waiter) {
                        complete_write(shared_state, std::move(item.waiter), false);
                    }
                }
                if (pump.writing_waiter) {
                    complete_write(shared_state, std::move(pump.writing_waiter), false);
                }
            } else if (std::this_thread::get_id() != pump.writer_id) {
                // A write that already reached the output cannot outlive close(). A custom stream
                // buffer can call close() from inside write/flush, on the writer thread.
                pump.wake.wait(lock, [&pump] { return pump.output_drained(); });
            }
        }

        if (first_close) {
            boost::asio::post(shared_state->strand, [shared_state]() { shared_state->wake_reader(); });
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
            // A line read before close() is still delivered; close() only stops waiting for more.
            if (!shared_state->queue.empty()) {
                auto message = std::move(shared_state->queue.front());
                shared_state->queue.pop();
                {
                    std::lock_guard lock(shared_state->pump->mutex);
                    shared_state->pump->queued_input_bytes -= message.size() + g_input_line_overhead;
                    shared_state->pump->wake.notify_all();
                }
                co_return message;
            }

            if (!shared_state->input_error.empty()) {
                throw std::runtime_error(shared_state->input_error);
            }
            if (shared_state->closed.load(std::memory_order_acquire) || shared_state->input_ended) {
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

        auto waiter = std::make_shared<WriteWaiter>(shared_state->strand);
        {
            std::lock_guard lock(shared_state->pump->mutex);
            if (shared_state->closed.load(std::memory_order_acquire)) {
                throw std::runtime_error("StdioTransport is closed");
            }
            shared_state->pump->outgoing.push_back({std::move(message), waiter});
            shared_state->pump->wake.notify_all();
        }

        while (!waiter->done) {
            boost::system::error_code error;
            co_await waiter->signal.async_wait(
                boost::asio::redirect_error(boost::asio::use_awaitable, error));
        }
        if (shared_state->closed.load(std::memory_order_acquire) && !waiter->written) {
            throw std::runtime_error("StdioTransport is closed");
        }
        if (!waiter->written) {
            throw std::runtime_error("StdioTransport failed to write to the output stream");
        }
    }

    /// Shared with the writer thread, so standard output is handed back only after the last write.
    std::shared_ptr<OwnedStdout> owned_output;
    std::shared_ptr<RawStdout> raw_stdout;
    std::istream& input;
    std::shared_ptr<SharedState> state;
    std::mutex threads_mutex;
    bool reader_started{false};
    bool writer_started{false};
    std::thread reader_thread;
    std::thread writer_thread;
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
    return mcp::detail::spawn_awaitable(state->strand, Impl::read_on_strand(state));
}

Task<void> StdioTransport::write_message(std::string_view message) {
    impl_->ensure_writer_started();
    auto state = impl_->state;
    auto owned_message = std::string(message);
    return mcp::detail::spawn_awaitable(state->strand,
                                        Impl::write_on_strand(state, std::move(owned_message)));
}

void StdioTransport::close() {
    auto state = impl_->state;
    Impl::close_state(state);
}

void StdioTransport::set_max_message_bytes(std::size_t bytes) {
    std::lock_guard lock(impl_->state->pump->mutex);
    impl_->state->pump->max_message_bytes = bytes;
}

}  // namespace mcp

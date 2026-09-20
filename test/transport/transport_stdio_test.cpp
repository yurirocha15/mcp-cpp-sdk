#include "mcp/transport/stdio.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <iterator>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace {

class BlockingStreambuf : public std::streambuf {
   public:
    void feed(std::string data) {
        {
            std::lock_guard lock(mutex_);
            buffer_ += std::move(data);
        }
        condition_.notify_all();
    }

    void close() {
        {
            std::lock_guard lock(mutex_);
            closed_ = true;
        }
        condition_.notify_all();
    }

   protected:
    int_type underflow() override {
        std::unique_lock lock(mutex_);
        condition_.wait(lock, [this] { return position_ < buffer_.size() || closed_; });
        if (position_ == buffer_.size()) {
            return traits_type::eof();
        }
        return traits_type::to_int_type(buffer_[position_]);
    }

    int_type uflow() override {
        std::unique_lock lock(mutex_);
        condition_.wait(lock, [this] { return position_ < buffer_.size() || closed_; });
        if (position_ == buffer_.size()) {
            return traits_type::eof();
        }
        return traits_type::to_int_type(buffer_[position_++]);
    }

   private:
    std::mutex mutex_;
    std::condition_variable condition_;
    std::string buffer_;
    std::size_t position_{0};
    bool closed_{false};
};

class TrackingOutputBuffer : public std::streambuf {
   public:
    bool observed_overlap() const noexcept { return observed_overlap_.load(std::memory_order_acquire); }

    std::string str() const {
        std::lock_guard lock(data_mutex_);
        return data_;
    }

   protected:
    std::streamsize xsputn(const char_type* data, std::streamsize size) override {
        Activity activity(*this);
        std::this_thread::sleep_for(std::chrono::microseconds(200));
        std::lock_guard lock(data_mutex_);
        data_.append(data, static_cast<std::size_t>(size));
        return size;
    }

    int_type overflow(int_type value) override {
        if (traits_type::eq_int_type(value, traits_type::eof())) {
            return traits_type::not_eof(value);
        }

        Activity activity(*this);
        std::this_thread::sleep_for(std::chrono::microseconds(200));
        std::lock_guard lock(data_mutex_);
        data_.push_back(traits_type::to_char_type(value));
        return value;
    }

    int sync() override {
        Activity activity(*this);
        std::this_thread::sleep_for(std::chrono::microseconds(200));
        return 0;
    }

   private:
    class Activity {
       public:
        explicit Activity(TrackingOutputBuffer& owner) : owner_(owner) {
            if (owner_.active_calls_.fetch_add(1, std::memory_order_acq_rel) != 0) {
                owner_.observed_overlap_.store(true, std::memory_order_release);
            }
        }

        ~Activity() { owner_.active_calls_.fetch_sub(1, std::memory_order_acq_rel); }

       private:
        TrackingOutputBuffer& owner_;
    };

    mutable std::mutex data_mutex_;
    std::string data_;
    std::atomic<unsigned int> active_calls_{0};
    std::atomic<bool> observed_overlap_{false};
};

class BlockingOutputBuffer : public std::streambuf {
   public:
    bool wait_until_write_starts(std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, timeout, [this] { return write_started_; });
    }

    void release_write() {
        {
            std::lock_guard lock(mutex_);
            write_released_ = true;
        }
        condition_.notify_all();
    }

    std::string str() const {
        std::lock_guard lock(mutex_);
        return data_;
    }

   protected:
    std::streamsize xsputn(const char_type* data, std::streamsize size) override {
        std::unique_lock lock(mutex_);
        write_started_ = true;
        condition_.notify_all();
        condition_.wait(lock, [this] { return write_released_; });
        data_.append(data, static_cast<std::size_t>(size));
        return size;
    }

    int_type overflow(int_type value) override {
        if (traits_type::eq_int_type(value, traits_type::eof())) {
            return traits_type::not_eof(value);
        }
        std::lock_guard lock(mutex_);
        data_.push_back(traits_type::to_char_type(value));
        return value;
    }

   private:
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::string data_;
    bool write_started_{false};
    bool write_released_{false};
};

}  // namespace

class StdioTransportTest : public ::testing::Test {
   protected:
    boost::asio::io_context io_ctx_;
};

TEST_F(StdioTransportTest, ReadMessageReturnsPipedInput) {
    std::istringstream input("hello world\n");
    std::ostringstream output;

    mcp::StdioTransport transport(io_ctx_.get_executor(), input, output);

    std::string result;
    boost::asio::co_spawn(
        io_ctx_, [&]() -> mcp::Task<void> { result = co_await transport.read_message(); },
        boost::asio::detached);
    io_ctx_.run();

    EXPECT_EQ(result, "hello world");
}

TEST_F(StdioTransportTest, ReadMultipleMessages) {
    std::istringstream input("first\nsecond\nthird\n");
    std::ostringstream output;

    mcp::StdioTransport transport(io_ctx_.get_executor(), input, output);

    std::vector<std::string> results;
    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            results.push_back(co_await transport.read_message());
            results.push_back(co_await transport.read_message());
            results.push_back(co_await transport.read_message());
        },
        boost::asio::detached);
    io_ctx_.run();

    ASSERT_EQ(results.size(), 3);
    EXPECT_EQ(results[0], "first");
    EXPECT_EQ(results[1], "second");
    EXPECT_EQ(results[2], "third");
}

TEST_F(StdioTransportTest, WriteMessageAppendsNewline) {
    std::istringstream input;
    std::ostringstream output;

    mcp::StdioTransport transport(io_ctx_.get_executor(), input, output);

    boost::asio::co_spawn(
        io_ctx_, [&]() -> mcp::Task<void> { co_await transport.write_message("test output"); },
        boost::asio::detached);
    io_ctx_.run();

    EXPECT_EQ(output.str(), "test output\n");
}

TEST_F(StdioTransportTest, WriteMultipleMessages) {
    std::istringstream input;
    std::ostringstream output;

    mcp::StdioTransport transport(io_ctx_.get_executor(), input, output);

    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            co_await transport.write_message("line1");
            co_await transport.write_message("line2");
        },
        boost::asio::detached);
    io_ctx_.run();

    EXPECT_EQ(output.str(), "line1\nline2\n");
}

TEST_F(StdioTransportTest, ConcurrentWritesOwnPayloadAndRemainSerialized) {
    constexpr std::size_t write_count = 64;
    constexpr std::size_t thread_count = 4;

    std::istringstream input;
    TrackingOutputBuffer output_buffer;
    std::ostream output(&output_buffer);
    mcp::StdioTransport transport(io_ctx_.get_executor(), input, output);

    std::vector<std::string> expected;
    expected.reserve(write_count);
    for (std::size_t index = 0; index < write_count; ++index) {
        expected.push_back("message-" + std::to_string(index) + ":" +
                           std::string(2048, static_cast<char>('a' + index % 26)));
    }

    std::vector<std::future<void>> completions(write_count);
    std::vector<std::thread> submitters;
    for (std::size_t thread_index = 0; thread_index < thread_count; ++thread_index) {
        submitters.emplace_back([&, thread_index]() {
            for (std::size_t index = thread_index; index < write_count; index += thread_count) {
                auto source = expected[index];
                auto write = transport.write_message(source);
                source.assign("mutated after write_message returned");
                completions[index] =
                    boost::asio::co_spawn(io_ctx_, std::move(write), boost::asio::use_future);
            }
        });
    }
    for (auto& submitter : submitters) {
        submitter.join();
    }

    std::vector<std::thread> runners;
    for (std::size_t index = 0; index < thread_count; ++index) {
        runners.emplace_back([this]() { io_ctx_.run(); });
    }
    for (auto& runner : runners) {
        runner.join();
    }
    for (auto& completion : completions) {
        EXPECT_NO_THROW(completion.get());
    }

    EXPECT_FALSE(output_buffer.observed_overlap());
    std::unordered_set<std::string> actual;
    std::istringstream lines(output_buffer.str());
    for (std::string line; std::getline(lines, line);) {
        actual.insert(std::move(line));
    }
    EXPECT_EQ(actual.size(), write_count);
    for (const auto& message : expected) {
        EXPECT_EQ(actual.count(message), 1);
    }
}

TEST_F(StdioTransportTest, RejectsASecondOutstandingRead) {
    BlockingStreambuf input_buffer;
    std::istream input(&input_buffer);
    std::ostringstream output;
    mcp::StdioTransport transport(io_ctx_.get_executor(), input, output);

    auto first = boost::asio::co_spawn(io_ctx_, transport.read_message(), boost::asio::use_future);
    auto second = boost::asio::co_spawn(io_ctx_, transport.read_message(), boost::asio::use_future);

    std::thread first_runner([this]() { io_ctx_.run(); });
    std::thread second_runner([this]() { io_ctx_.run(); });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    bool one_completed = false;
    while (std::chrono::steady_clock::now() < deadline) {
        one_completed = first.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready ||
                        second.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready;
        if (one_completed) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    input_buffer.feed("accepted\n");
    input_buffer.close();
    first_runner.join();
    second_runner.join();

    EXPECT_TRUE(one_completed);
    unsigned int accepted = 0;
    unsigned int rejected = 0;
    auto classify = [&](std::future<std::string>& result) {
        try {
            EXPECT_EQ(result.get(), "accepted");
            ++accepted;
        } catch (const std::logic_error&) {
            ++rejected;
        } catch (const std::exception& error) {
            ADD_FAILURE() << "Unexpected read failure: " << error.what();
        }
    };
    classify(first);
    classify(second);
    EXPECT_EQ(accepted, 1U);
    EXPECT_EQ(rejected, 1U);
}

TEST_F(StdioTransportTest, PendingWriteOwnsStateAfterTransportDestruction) {
    std::istringstream input;
    std::ostringstream output;
    std::optional<mcp::Task<void>> pending;
    {
        mcp::StdioTransport transport(io_ctx_.get_executor(), input, output);
        std::string message = "owned payload";
        pending.emplace(transport.write_message(message));
        message.assign("mutated");
    }

    auto completion = boost::asio::co_spawn(io_ctx_, std::move(*pending), boost::asio::use_future);
    io_ctx_.run();
    EXPECT_THROW(completion.get(), std::runtime_error);
    EXPECT_TRUE(output.str().empty());
}

TEST_F(StdioTransportTest, DestructionWaitsForAnActiveWrite) {
    std::istringstream input;
    BlockingOutputBuffer output_buffer;
    std::ostream output(&output_buffer);
    auto transport = std::make_unique<mcp::StdioTransport>(io_ctx_.get_executor(), input, output);

    auto completion =
        boost::asio::co_spawn(io_ctx_, transport->write_message("in flight"), boost::asio::use_future);
    std::thread runner([this]() { io_ctx_.run(); });
    const bool write_started = output_buffer.wait_until_write_starts(std::chrono::seconds(2));
    if (!write_started) {
        output_buffer.release_write();
        runner.join();
        transport.reset();
        FAIL() << "Timed out waiting for the write to start";
        return;
    }

    std::atomic<bool> destruction_finished{false};
    std::thread destroyer([&]() {
        transport.reset();
        destruction_finished.store(true, std::memory_order_release);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_FALSE(destruction_finished.load(std::memory_order_acquire));

    output_buffer.release_write();
    destroyer.join();
    runner.join();
    EXPECT_TRUE(destruction_finished.load(std::memory_order_acquire));
    EXPECT_NO_THROW(completion.get());
    EXPECT_EQ(output_buffer.str(), "in flight\n");
}

TEST_F(StdioTransportTest, PendingReadOwnsStateAfterTransportDestruction) {
    std::istringstream input;
    std::ostringstream output;
    std::optional<mcp::Task<std::string>> pending;
    {
        mcp::StdioTransport transport(io_ctx_.get_executor(), input, output);
        pending.emplace(transport.read_message());
    }

    auto completion = boost::asio::co_spawn(io_ctx_, std::move(*pending), boost::asio::use_future);
    io_ctx_.run();
    EXPECT_THROW(completion.get(), std::runtime_error);
}

TEST_F(StdioTransportTest, CloseIsIdempotent) {
    std::istringstream input;
    std::ostringstream output;

    mcp::StdioTransport transport(io_ctx_.get_executor(), input, output);
    io_ctx_.run();

    transport.close();
    transport.close();
}

TEST_F(StdioTransportTest, DestructorDoesNotHang) {
    std::istringstream input;
    std::ostringstream output;

    {
        mcp::StdioTransport transport(io_ctx_.get_executor(), input, output);
        io_ctx_.run();
    }
}

TEST_F(StdioTransportTest, ReadJsonRpcMessage) {
    std::istringstream input(R"({"jsonrpc":"2.0","method":"initialize","id":1})"
                             "\n");
    std::ostringstream output;

    mcp::StdioTransport transport(io_ctx_.get_executor(), input, output);

    std::string result;
    boost::asio::co_spawn(
        io_ctx_, [&]() -> mcp::Task<void> { result = co_await transport.read_message(); },
        boost::asio::detached);
    io_ctx_.run();

    EXPECT_EQ(result, R"({"jsonrpc":"2.0","method":"initialize","id":1})");
}

TEST_F(StdioTransportTest, PolymorphicThroughBasePointer) {
    std::istringstream input("polymorphic\n");
    std::ostringstream output;

    std::shared_ptr<mcp::ITransport> transport =
        std::make_shared<mcp::StdioTransport>(io_ctx_.get_executor(), input, output);

    std::string result;
    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            result = co_await transport->read_message();
            co_await transport->write_message("response");
        },
        boost::asio::detached);
    io_ctx_.run();

    EXPECT_EQ(result, "polymorphic");
    EXPECT_EQ(output.str(), "response\n");

    transport->close();
}

// The protocol channel on this transport is a real process stream, so the tests
// that distinguish a shared stdout from an owned one have to work at the file
// descriptor level. POSIX only; the Windows implementation of
// create_owning_stdout() uses the CRT equivalents and is not exercised here.
#if !defined(_WIN32)

#include <fcntl.h>
#include <unistd.h>

namespace {

// Points the process's stdout and stderr at files the test can read back, and
// puts them back where it found them however the test leaves.
class CapturedProcessStreams {
   public:
    CapturedProcessStreams(const std::filesystem::path& out, const std::filesystem::path& err)
        : saved_stdout_(::dup(STDOUT_FILENO)), saved_stderr_(::dup(STDERR_FILENO)) {
        std::cout.flush();
        std::cerr.flush();
        std::fflush(nullptr);
        redirect(STDOUT_FILENO, out);
        redirect(STDERR_FILENO, err);
    }

    ~CapturedProcessStreams() { restore(); }

    CapturedProcessStreams(const CapturedProcessStreams&) = delete;
    CapturedProcessStreams& operator=(const CapturedProcessStreams&) = delete;

    void restore() {
        std::cout.flush();
        std::cerr.flush();
        std::fflush(nullptr);
        if (saved_stdout_ >= 0) {
            ::dup2(saved_stdout_, STDOUT_FILENO);
            ::close(saved_stdout_);
            saved_stdout_ = -1;
        }
        if (saved_stderr_ >= 0) {
            ::dup2(saved_stderr_, STDERR_FILENO);
            ::close(saved_stderr_);
            saved_stderr_ = -1;
        }
    }

   private:
    static void redirect(int target, const std::filesystem::path& path) {
        const int file = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        ASSERT_GE(file, 0);
        ASSERT_GE(::dup2(file, target), 0);
        ::close(file);
    }

    int saved_stdout_;
    int saved_stderr_;
};

std::string read_file(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

// What a peer speaking JSON-RPC over the pipe would have to parse.
std::vector<std::string> protocol_lines(const std::filesystem::path& path) {
    std::vector<std::string> lines;
    std::istringstream contents(read_file(path));
    std::string line;
    while (std::getline(contents, line)) {
        if (!line.empty()) {
            lines.push_back(line);
        }
    }
    return lines;
}

constexpr const char* kFramedMessage = R"({"jsonrpc":"2.0","id":1,"result":{}})";
constexpr const char* kStrayPrintf = "libfoo: connected to database";
constexpr const char* kStrayCout = "cache warmed in 12ms";

// Everything an application might innocently put on stdout while the session is
// running.
void emit_stray_application_output() {
    std::printf("%s\n", kStrayPrintf);
    std::fflush(stdout);
    std::cout << kStrayCout << std::endl;
}

std::filesystem::path scratch_file(const char* name) {
    return std::filesystem::temp_directory_path() / name;
}

}  // namespace

// The footgun itself, asserted rather than assumed: the default std::cout
// output shares the protocol channel with the rest of the process.
TEST_F(StdioTransportTest, DefaultOutputSharesTheProtocolChannelWithTheApplication) {
    const auto out = scratch_file("mcp_stdio_shared_out.txt");
    const auto err = scratch_file("mcp_stdio_shared_err.txt");

    {
        CapturedProcessStreams capture(out, err);
        std::istringstream input;
        mcp::StdioTransport transport(io_ctx_.get_executor(), input, std::cout);

        boost::asio::co_spawn(
            io_ctx_,
            [&]() -> mcp::Task<void> {
                co_await transport.write_message(kFramedMessage);
                emit_stray_application_output();
                co_await transport.write_message(kFramedMessage);
            },
            boost::asio::detached);
        io_ctx_.run();
        std::cout.flush();
    }

    const auto lines = protocol_lines(out);
    ASSERT_EQ(lines.size(), 4U) << "expected the two framed messages plus two stray lines";
    EXPECT_EQ(lines[0], kFramedMessage);
    EXPECT_EQ(lines[1], kStrayPrintf);
    EXPECT_EQ(lines[2], kStrayCout);
    EXPECT_EQ(lines[3], kFramedMessage);

    std::filesystem::remove(out);
    std::filesystem::remove(err);
}

// The regression test: an owning transport keeps the channel to itself.
TEST_F(StdioTransportTest, OwnedStdoutKeepsStrayApplicationOutputOffTheProtocolChannel) {
    const auto out = scratch_file("mcp_stdio_owned_out.txt");
    const auto err = scratch_file("mcp_stdio_owned_err.txt");

    {
        CapturedProcessStreams capture(out, err);
        std::istringstream input;
        auto transport = mcp::StdioTransport::create_owning_stdout(io_ctx_.get_executor(), input);

        boost::asio::co_spawn(
            io_ctx_,
            [&]() -> mcp::Task<void> {
                co_await transport->write_message(kFramedMessage);
                emit_stray_application_output();
                co_await transport->write_message(kFramedMessage);
            },
            boost::asio::detached);
        io_ctx_.run();
        std::cout.flush();
        transport.reset();
    }

    const auto lines = protocol_lines(out);
    ASSERT_EQ(lines.size(), 2U) << "protocol channel carried: " << read_file(out);
    EXPECT_EQ(lines[0], kFramedMessage);
    EXPECT_EQ(lines[1], kFramedMessage);

    // The application's output is not lost, only moved to the diagnostics stream.
    const auto diagnostics = read_file(err);
    EXPECT_NE(diagnostics.find(kStrayPrintf), std::string::npos);
    EXPECT_NE(diagnostics.find(kStrayCout), std::string::npos);

    std::filesystem::remove(out);
    std::filesystem::remove(err);
}

// Destroying the owning transport has to give the process its stdout back,
// otherwise a short-lived session silently swallows everything that follows.
TEST_F(StdioTransportTest, OwnedStdoutIsRestoredWhenTheTransportIsDestroyed) {
    const auto out = scratch_file("mcp_stdio_restore_out.txt");
    const auto err = scratch_file("mcp_stdio_restore_err.txt");

    {
        CapturedProcessStreams capture(out, err);
        std::istringstream input;
        {
            auto transport = mcp::StdioTransport::create_owning_stdout(io_ctx_.get_executor(), input);
        }
        std::cout << "after the session" << std::endl;
    }

    EXPECT_NE(read_file(out).find("after the session"), std::string::npos);

    std::filesystem::remove(out);
    std::filesystem::remove(err);
}

// A second owner would duplicate the already-redirected stdout and publish the
// protocol onto stderr, so the attempt has to fail loudly instead.
TEST_F(StdioTransportTest, OnlyOneTransportMayOwnStdoutAtATime) {
    const auto out = scratch_file("mcp_stdio_single_out.txt");
    const auto err = scratch_file("mcp_stdio_single_err.txt");

    // Assertions live outside the capture, otherwise their diagnostics are
    // written to the captured file and thrown away with it.
    bool second_owner_rejected = false;
    std::string rejection;
    {
        CapturedProcessStreams capture(out, err);
        std::istringstream input;
        auto first = mcp::StdioTransport::create_owning_stdout(io_ctx_.get_executor(), input);
        try {
            auto second = mcp::StdioTransport::create_owning_stdout(io_ctx_.get_executor(), input);
        } catch (const std::runtime_error& error) {
            second_owner_rejected = true;
            rejection = error.what();
        }
    }

    EXPECT_TRUE(second_owner_rejected) << "a second owner would publish the protocol onto stderr";
    EXPECT_NE(rejection.find("standard output"), std::string::npos) << rejection;

    std::filesystem::remove(out);
    std::filesystem::remove(err);
}

#endif  // !defined(_WIN32)

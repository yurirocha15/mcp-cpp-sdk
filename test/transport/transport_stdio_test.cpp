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
#include <future>
#include <mutex>
#include <optional>
#include <sstream>
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

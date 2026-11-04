#include "mcp/detail/signal.hpp"
#include "mcp/server/server.hpp"

#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstddef>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <mutex>
#include <optional>
#include <ostream>
#include <sstream>
#include <streambuf>
#include <string>
#include <thread>

#if !defined(_WIN32)
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

nlohmann::json make_initialize_request(std::string_view id) {
    return {{"jsonrpc", "2.0"},
            {"id", id},
            {"method", "initialize"},
            {"params",
             {{"protocolVersion", mcp::g_LATEST_PROTOCOL_VERSION},
              {"clientInfo", {{"name", "test-client"}, {"version", "0.1"}}},
              {"capabilities", nlohmann::json::object()}}}};
}

nlohmann::json make_tool_call_request(std::string_view id, const std::string& tool_name,
                                      const nlohmann::json& args) {
    return {{"jsonrpc", "2.0"},
            {"id", id},
            {"method", "tools/call"},
            {"params", {{"name", tool_name}, {"arguments", args}}}};
}

nlohmann::json make_initialized_notification() {
    return {{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}};
}

nlohmann::json greet_schema() {
    return {
        {"type", "object"},
        {"properties", {{"name", {{"type", "string"}}}}},
        {"required", nlohmann::json::array({"name"})},
    };
}

std::vector<nlohmann::json> parse_responses(const std::string& raw) {
    std::vector<nlohmann::json> responses;
    std::istringstream stream(raw);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty()) {
            responses.push_back(nlohmann::json::parse(line));
        }
    }
    return responses;
}

/// A streambuf that serves pre-loaded data one character at a time,
/// blocking in underflow() until data is available or close() is called.
class BlockingStreambuf : public std::streambuf {
   public:
    void feed(const std::string& data) {
        std::lock_guard<std::mutex> lock(mu_);
        buffer_ += data;
        cv_.notify_one();
    }

    void close() {
        std::lock_guard<std::mutex> lock(mu_);
        closed_ = true;
        cv_.notify_one();
    }

   protected:
    int underflow() override {
        std::unique_lock<std::mutex> lock(mu_);
        cv_.wait(lock, [&] { return pos_ < buffer_.size() || closed_; });
        if (pos_ >= buffer_.size()) {
            return traits_type::eof();
        }
        return traits_type::to_int_type(buffer_[pos_]);
    }

    int uflow() override {
        std::unique_lock<std::mutex> lock(mu_);
        cv_.wait(lock, [&] { return pos_ < buffer_.size() || closed_; });
        if (pos_ >= buffer_.size()) {
            return traits_type::eof();
        }
        return traits_type::to_int_type(buffer_[pos_++]);
    }

   private:
    std::mutex mu_;
    std::condition_variable cv_;
    std::string buffer_;
    std::size_t pos_ = 0;
    bool closed_ = false;
};

/// A streambuf that collects what the server writes under a mutex and lets the test thread block
/// until whole response lines have arrived.
///
/// Polling a std::ostringstream that the server thread is writing into would be a data race on the
/// stream's own buffer, and run_stdio() offers its caller no way to learn that a response has been
/// produced.
class CollectingStreambuf : public std::streambuf {
   public:
    /// Blocks until `count` newline-terminated lines have been written or the budget expires.
    /// Returns whether the count was reached.
    bool wait_for_lines(std::size_t count, std::chrono::milliseconds budget) {
        std::unique_lock<std::mutex> lock(mu_);
        return cv_.wait_for(lock, budget, [&] { return lines_ >= count; });
    }

    std::string str() {
        std::lock_guard<std::mutex> lock(mu_);
        return buffer_;
    }

   protected:
    int overflow(int ch) override {
        if (ch == traits_type::eof()) {
            return traits_type::not_eof(ch);
        }
        const auto byte = traits_type::to_char_type(ch);
        {
            std::lock_guard<std::mutex> lock(mu_);
            buffer_.push_back(byte);
            if (byte == '\n') {
                ++lines_;
            }
        }
        cv_.notify_all();
        return ch;
    }

    std::streamsize xsputn(const char* data, std::streamsize count) override {
        {
            std::lock_guard<std::mutex> lock(mu_);
            buffer_.append(data, static_cast<std::size_t>(count));
            lines_ += static_cast<std::size_t>(std::count(data, data + count, '\n'));
        }
        cv_.notify_all();
        return count;
    }

   private:
    std::mutex mu_;
    std::condition_variable cv_;
    std::string buffer_;
    std::size_t lines_ = 0;
};

constexpr std::chrono::milliseconds g_response_budget{10000};

#if !defined(_WIN32)

namespace {

/// A server on real standard streams, in a child process. The blocking reads and writes these tests
/// are about only exist on real descriptors.
class StdioServerProcess {
   public:
    /// @p setup adds what the test needs to the child's server. @p unsynced has the child call
    /// std::ios::sync_with_stdio(false) first, which replaces the standard streams' buffers.
    explicit StdioServerProcess(const std::function<void(mcp::Server&)>& setup = {},
                                bool unsynced = false) {
        int to_child[2];
        int from_child[2];
        EXPECT_EQ(::pipe(to_child), 0);
        EXPECT_EQ(::pipe(from_child), 0);
        child_ = ::fork();
        if (child_ == 0) {
            ::dup2(to_child[0], STDIN_FILENO);
            ::dup2(from_child[1], STDOUT_FILENO);
            for (const int descriptor : {to_child[0], to_child[1], from_child[0], from_child[1]}) {
                ::close(descriptor);
            }
            if (unsynced) {
                std::ios::sync_with_stdio(false);
            }
            mcp::Implementation info;
            info.name = "stdio-child";
            info.version = "1.0";
            mcp::ServerCapabilities capabilities;
            capabilities.tools = mcp::ServerCapabilities::ToolsCapability{};
            mcp::Server server(std::move(info), std::move(capabilities));
            if (setup) {
                setup(server);
            }
            server.run_stdio();
            // A real exit: it flushes the C library's streams, which waits for any thread holding
            // their locks in a blocked read or write.
            std::exit(0);
        }
        ::close(to_child[0]);
        ::close(from_child[1]);
        to_child_ = to_child[1];
        from_child_ = from_child[0];
    }

    ~StdioServerProcess() {
        close_stdin();
        close_stdout_reader();
        if (child_ > 0 && !reaped_) {
            ::kill(child_, SIGKILL);
            ::waitpid(child_, nullptr, 0);
        }
    }

    StdioServerProcess(const StdioServerProcess&) = delete;
    StdioServerProcess& operator=(const StdioServerProcess&) = delete;

    void send(const std::string& text) const {
        ASSERT_EQ(::write(to_child_, text.data(), text.size()), static_cast<ssize_t>(text.size()));
    }

    /// Blocks until the server answered once, which shows its signal handler is installed.
    void wait_until_serving() const {
        send(R"({"jsonrpc":"2.0","id":1,"method":"ping"})"
             "\n");
        char byte = 0;
        ASSERT_EQ(::read(from_child_, &byte, 1), 1);
    }

    /// Fills the pipe the server writes to, by asking for answers nobody reads, until the server
    /// blocks writing: the bytes waiting in the pipe then stop growing while answers are still owed.
    void fill_stdout_pipe() const {
        const std::string ping = R"({"jsonrpc":"2.0","id":2,"method":"ping"})"
                                 "\n";
        int pending = 0;
        int last = -1;
        int unchanged = 0;
        // Sends in small steps: the server stops reading its input once it cannot write.
        while (unchanged < 3) {
            std::string batch;
            for (int i = 0; i < 64; ++i) {
                batch += ping;
            }
            send(batch);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            ASSERT_EQ(::ioctl(from_child_, FIONREAD, &pending), 0);
            unchanged = pending > 0 && pending == last ? unchanged + 1 : 0;
            last = pending;
        }
    }

    void signal(int number) const { ASSERT_EQ(::kill(child_, number), 0); }

    /// Everything the server writes until it closes standard output.
    std::string read_to_end() const {
        std::string text;
        char chunk[65536];
        for (;;) {
            const auto count = ::read(from_child_, chunk, sizeof(chunk));
            if (count <= 0) {
                return text;
            }
            text.append(chunk, static_cast<std::size_t>(count));
        }
    }

    void close_stdin() {
        if (to_child_ >= 0) {
            ::close(to_child_);
            to_child_ = -1;
        }
    }

    void close_stdout_reader() {
        if (from_child_ >= 0) {
            ::close(from_child_);
            from_child_ = -1;
        }
    }

    /// The child's wait status, or nullopt when it is still running after two minutes: a bound for
    /// reporting a hang, not a measure of how fast it should be.
    std::optional<int> wait_for_exit() {
        for (int i = 0; i < 24000; ++i) {
            int status = 0;
            if (::waitpid(child_, &status, WNOHANG) == child_) {
                reaped_ = true;
                return status;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return std::nullopt;
    }

   private:
    pid_t child_{-1};
    int to_child_{-1};
    int from_child_{-1};
    bool reaped_{false};
};

/// Adds a tool "spin" that writes 'S' to @p marker when it starts, runs until it is cancelled, then
/// writes 'C' and answers.
void add_spin_tool(mcp::Server& server, int marker) {
    server.add_tool<nlohmann::json, nlohmann::json>(
        "spin", "Runs until cancelled", nlohmann::json{{"type", "object"}},
        [marker](mcp::Context& ctx, nlohmann::json /*args*/) -> mcp::Task<nlohmann::json> {
            auto written = ::write(marker, "S", 1);
            auto executor = co_await boost::asio::this_coro::executor;
            while (!ctx.is_cancelled()) {
                boost::asio::steady_timer tick(executor, std::chrono::milliseconds(1));
                co_await tick.async_wait(boost::asio::use_awaitable);
            }
            written = ::write(marker, "C", 1);
            static_cast<void>(written);
            co_return nlohmann::json{{"cancelled", true}};
        });
}

/// The next byte written to @p marker, or 0 when the writer closes it or nothing arrives within
/// 90 seconds: a bound for reporting a hang, not a measure of how fast it should be.
char next_marker(int marker) {
    pollfd ready{marker, POLLIN, 0};
    if (::poll(&ready, 1, 90000) <= 0) {
        return 0;
    }
    char byte = 0;
    return ::read(marker, &byte, 1) == 1 ? byte : 0;
}

std::string spin_session() {
    return make_initialize_request("1").dump() + "\n" + make_initialized_notification().dump() + "\n" +
           make_tool_call_request("s", "spin", nlohmann::json::object()).dump() + "\n";
}

}  // namespace

// The end of standard input asks the server to finish: a handler still running is told to stop,
// its answer is written, and the server exits.
TEST(RunStdioProcess, InputEndCancelsARunningHandler) {
    int marker[2];
    ASSERT_EQ(::pipe(marker), 0);
    StdioServerProcess server([&marker](mcp::Server& child) { add_spin_tool(child, marker[1]); });
    ::close(marker[1]);
    server.send(spin_session());
    ASSERT_EQ(next_marker(marker[0]), 'S');
    server.close_stdin();
    const auto seen = next_marker(marker[0]);
    ::close(marker[0]);
    ASSERT_EQ(seen, 'C');
    const auto responses = parse_responses(server.read_to_end());
    const auto status = server.wait_for_exit();
    ASSERT_TRUE(status.has_value());
    EXPECT_TRUE(WIFEXITED(*status));
    const auto answered = std::any_of(responses.begin(), responses.end(), [](const auto& response) {
        return response.value("id", nlohmann::json()) == "s";
    });
    EXPECT_TRUE(answered);
}

// A signal ends the run at once: it does not wait for a running handler, which is told to stop.
TEST(RunStdioProcess, SignalCancelsARunningHandler) {
    int marker[2];
    ASSERT_EQ(::pipe(marker), 0);
    StdioServerProcess server([&marker](mcp::Server& child) { add_spin_tool(child, marker[1]); });
    ::close(marker[1]);
    server.send(spin_session());
    ASSERT_EQ(next_marker(marker[0]), 'S');
    server.signal(SIGTERM);
    const auto seen = next_marker(marker[0]);
    ::close(marker[0]);
    EXPECT_EQ(seen, 'C');
    const auto status = server.wait_for_exit();
    ASSERT_TRUE(status.has_value());
    EXPECT_TRUE(WIFEXITED(*status));
}

// The parent keeps standard input open here, so nothing but the signal can end the run.
TEST(RunStdioProcess, SignalEndsTheRunWhileStandardInputStaysOpen) {
    for (const int number : {SIGTERM, SIGINT}) {
        StdioServerProcess server;
        server.wait_until_serving();
        server.signal(number);
        const auto status = server.wait_for_exit();
        ASSERT_TRUE(status.has_value());
        EXPECT_TRUE(WIFEXITED(*status));
        EXPECT_EQ(WEXITSTATUS(*status), 0);
    }
}

TEST(RunStdioProcess, SignalEndsTheRunWhileThePeerStopsReading) {
    StdioServerProcess server;
    server.wait_until_serving();
    server.fill_stdout_pipe();
    server.signal(SIGTERM);
    const auto status = server.wait_for_exit();
    ASSERT_TRUE(status.has_value());
    EXPECT_TRUE(WIFEXITED(*status));
}

// std::ios::sync_with_stdio(false) gives std::cin and std::cout new buffers; they still stand for the
// process's descriptors, so a signal still ends the run.
TEST(RunStdioProcess, SignalEndsTheRunWithUnsyncedStandardStreams) {
    StdioServerProcess server({}, true);
    server.wait_until_serving();
    server.signal(SIGTERM);
    const auto status = server.wait_for_exit();
    ASSERT_TRUE(status.has_value());
    EXPECT_TRUE(WIFEXITED(*status));
}

// A peer that stopped reading can be answered no more, so the run ends without waiting for the
// end of its input.
TEST(RunStdioProcess, ClosedStandardOutputEndsTheRun) {
    StdioServerProcess server;
    server.wait_until_serving();
    server.close_stdout_reader();
    server.send(R"({"jsonrpc":"2.0","id":2,"method":"ping"})"
                "\n");
    const auto status = server.wait_for_exit();
    ASSERT_TRUE(status.has_value());
    EXPECT_TRUE(WIFEXITED(*status));
}

// The answer to a request read before the input ended is written in full, however slowly the peer
// reads it.
TEST(RunStdioProcess, AnswerStartedBeforeTheInputEndsIsWrittenInFull) {
    constexpr std::size_t size = 800000;
    StdioServerProcess server([](mcp::Server& child) {
        child.add_tool<nlohmann::json, nlohmann::json>(
            "big", "Returns a large result", nlohmann::json{{"type", "object"}},
            [](const nlohmann::json&) -> mcp::Task<nlohmann::json> {
                co_return nlohmann::json{{"text", std::string(size, 'x')}};
            });
    });
    server.send(make_initialize_request("1").dump() + "\n");
    server.send(make_initialized_notification().dump() + "\n");
    server.send(make_tool_call_request("2", "big", nlohmann::json::object()).dump() + "\n");
    server.close_stdin();
    // Longer than the transport waits for output when it is destroyed.
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    const auto responses = parse_responses(server.read_to_end());
    ASSERT_EQ(responses.size(), 2);
    EXPECT_EQ(responses[1]["result"]["structuredContent"]["text"].get<std::string>().size(), size);
    const auto status = server.wait_for_exit();
    ASSERT_TRUE(status.has_value());
    EXPECT_TRUE(WIFEXITED(*status));
}

TEST(RunStdioProcess, ClosedStandardOutputIsNotAFatalSignal) {
    StdioServerProcess server;
    server.wait_until_serving();
    server.close_stdout_reader();
    server.send(R"({"jsonrpc":"2.0","id":2,"method":"ping"})"
                "\n");
    server.close_stdin();
    const auto status = server.wait_for_exit();
    ASSERT_TRUE(status.has_value());
    EXPECT_FALSE(WIFSIGNALED(*status)) << "signal " << WTERMSIG(*status);
}

#endif

}  // namespace

class ServerStdioTest : public ::testing::Test {
   protected:
    ServerStdioTest() {
        mcp::Implementation info;
        info.name = "test-server";
        info.version = "1.0";

        mcp::ServerCapabilities caps;
        caps.tools = mcp::ServerCapabilities::ToolsCapability{};

        server_ = std::make_unique<mcp::Server>(std::move(info), std::move(caps));

        server_->add_tool<nlohmann::json, nlohmann::json>(
            "greet", "Greets the user", greet_schema(),
            [](const nlohmann::json& args) -> mcp::Task<nlohmann::json> {
                co_return nlohmann::json{{"greeting", "Hello, " + args.at("name").get<std::string>()}};
            });
    }

    std::unique_ptr<mcp::Server> server_;
};

TEST_F(ServerStdioTest, RunStdioInitializesAndResponds) {
    BlockingStreambuf sbuf;
    std::istream input(&sbuf);
    CollectingStreambuf obuf;
    std::ostream output(&obuf);

    sbuf.feed(make_initialize_request("1").dump() + "\n");

    std::thread server_thread([&] { server_->run_stdio(input, output); });

    EXPECT_TRUE(obuf.wait_for_lines(1, g_response_budget)) << "no response line was written";

    sbuf.close();
    server_thread.join();

    auto responses = parse_responses(obuf.str());
    ASSERT_EQ(responses.size(), 1);
    EXPECT_EQ(responses[0]["id"], "1");
    EXPECT_TRUE(responses[0].contains("result"));
    EXPECT_EQ(responses[0]["result"]["serverInfo"]["name"], "test-server");
}

TEST_F(ServerStdioTest, RunStdioHandlesToolCall) {
    BlockingStreambuf sbuf;
    std::istream input(&sbuf);
    CollectingStreambuf obuf;
    std::ostream output(&obuf);

    sbuf.feed(make_initialize_request("1").dump() + "\n");
    sbuf.feed(make_initialized_notification().dump() + "\n");
    sbuf.feed(make_tool_call_request("2", "greet", {{"name", "World"}}).dump() + "\n");

    std::thread server_thread([&] { server_->run_stdio(input, output); });

    EXPECT_TRUE(obuf.wait_for_lines(2, g_response_budget)) << "fewer than two response lines";

    sbuf.close();
    server_thread.join();

    auto responses = parse_responses(obuf.str());
    ASSERT_GE(responses.size(), 2);

    auto tool_response = responses[1];
    EXPECT_EQ(tool_response["id"], "2");
    ASSERT_TRUE(tool_response.contains("result"));
    EXPECT_EQ(tool_response["result"]["structuredContent"]["greeting"], "Hello, World");
}

// The end of the input lets the requests already read finish: each still gets its answer.
TEST_F(ServerStdioTest, EveryRequestReadBeforeTheInputEndsIsAnswered) {
    server_->add_tool<nlohmann::json, nlohmann::json>(
        "pause", "Answers after a short wait", nlohmann::json{{"type", "object"}},
        [](const nlohmann::json&) -> mcp::Task<nlohmann::json> {
            auto executor = co_await boost::asio::this_coro::executor;
            boost::asio::steady_timer timer(executor, std::chrono::milliseconds(20));
            co_await timer.async_wait(boost::asio::use_awaitable);
            co_return nlohmann::json{{"done", true}};
        });
    constexpr int calls = 50;
    std::string text =
        make_initialize_request("1").dump() + "\n" + make_initialized_notification().dump() + "\n";
    for (int i = 0; i < calls; ++i) {
        text +=
            make_tool_call_request("c" + std::to_string(i), "pause", nlohmann::json::object()).dump() +
            "\n";
    }
    std::istringstream input(text);
    std::ostringstream output;

    server_->run_stdio(input, output);

    const auto responses = parse_responses(output.str());
    ASSERT_EQ(responses.size(), static_cast<std::size_t>(calls) + 1);
    for (const auto& response : responses) {
        EXPECT_TRUE(response.contains("result")) << response.dump();
    }
}

// Parse errors are answered without the read loop waiting for the write; the end of the input still
// waits for those answers.
TEST_F(ServerStdioTest, ParseErrorsBeforeTheInputEndsAreAnswered) {
    constexpr int lines = 20;
    std::string text;
    for (int i = 0; i < lines; ++i) {
        text += "{not json\n";
    }
    std::istringstream input(text);
    std::ostringstream output;

    server_->run_stdio(input, output);

    const auto responses = parse_responses(output.str());
    ASSERT_EQ(responses.size(), static_cast<std::size_t>(lines));
    for (const auto& response : responses) {
        EXPECT_EQ(response["error"]["code"], -32700) << response.dump();
    }
}

TEST_F(ServerStdioTest, RunStdioExitsCleanlyOnEmptyInput) {
    std::istringstream input("");
    std::ostringstream output;

    server_->run_stdio(input, output);

    EXPECT_TRUE(output.str().empty());
}

// Returning is the whole assertion: what this detects is run_stdio() failing to shut down on a
// signal, which shows up as a join that never completes rather than as a wrong value. The absence
// of an EXPECT here is deliberate.
TEST_F(ServerStdioTest, RunStdioSignalCausesShutdown) {
    BlockingStreambuf sbuf;
    std::istream input(&sbuf);
    std::ostringstream output;

    std::thread server_thread([&] { server_->run_stdio(input, output); });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    mcp::detail::trigger_shutdown_signal();
    sbuf.close();
    server_thread.join();
}

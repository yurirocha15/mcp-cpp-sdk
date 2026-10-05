#include "mcp/detail/signal.hpp"
#include "mcp/server/server.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstddef>
#include <mutex>
#include <ostream>
#include <sstream>
#include <streambuf>
#include <string>
#include <thread>

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

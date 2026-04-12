#include "mcp/server.hpp"

#include <gtest/gtest.h>

#include <condition_variable>
#include <csignal>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#ifndef _WIN32
#include <sys/types.h>
#include <unistd.h>
#endif

namespace {

nlohmann::json make_initialize_request(std::string_view id) {
    return {{"jsonrpc", "2.0"},
            {"id", id},
            {"method", "initialize"},
            {"params",
             {{"protocolVersion", mcp::LATEST_PROTOCOL_VERSION},
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
    std::ostringstream output;

    sbuf.feed(make_initialize_request("1").dump() + "\n");

    std::thread server_thread([&] { server_->run_stdio(input, output); });

    // Wait for the response to appear
    for (int i = 0; i < 50; ++i) {
        if (!output.str().empty()) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    sbuf.close();
    server_thread.join();

    auto responses = parse_responses(output.str());
    ASSERT_EQ(responses.size(), 1);
    EXPECT_EQ(responses[0]["id"], "1");
    EXPECT_TRUE(responses[0].contains("result"));
    EXPECT_EQ(responses[0]["result"]["serverInfo"]["name"], "test-server");
}

TEST_F(ServerStdioTest, RunStdioHandlesToolCall) {
    BlockingStreambuf sbuf;
    std::istream input(&sbuf);
    std::ostringstream output;

    sbuf.feed(make_initialize_request("1").dump() + "\n");
    sbuf.feed(make_tool_call_request("2", "greet", {{"name", "World"}}).dump() + "\n");

    std::thread server_thread([&] { server_->run_stdio(input, output); });

    // Wait for both responses
    for (int i = 0; i < 100; ++i) {
        auto lines = parse_responses(output.str());
        if (lines.size() >= 2) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    sbuf.close();
    server_thread.join();

    auto responses = parse_responses(output.str());
    ASSERT_GE(responses.size(), 2);

    auto tool_response = responses[1];
    EXPECT_EQ(tool_response["id"], "2");
    ASSERT_TRUE(tool_response.contains("result"));
    EXPECT_EQ(tool_response["result"]["greeting"], "Hello, World");
}

TEST_F(ServerStdioTest, RunStdioExitsCleanlyOnEmptyInput) {
    std::istringstream input("");
    std::ostringstream output;

    server_->run_stdio(input, output);

    EXPECT_TRUE(output.str().empty());
}

TEST_F(ServerStdioTest, RunStdioSignalCausesShutdown) {
    BlockingStreambuf sbuf;
    std::istream input(&sbuf);
    std::ostringstream output;

    std::thread signal_sender([&sbuf] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        // Use kill(getpid()) instead of raise() so the signal is delivered to the
        // whole process. On macOS, raise() only targets the calling thread, which
        // means boost::asio::signal_set (running on the io_context thread) never
        // sees it and the default handler fires instead, causing a crash.
#ifdef _WIN32
        std::raise(SIGINT);
#else
        ::kill(::getpid(), SIGINT);
#endif
        sbuf.close();
    });

    server_->run_stdio(input, output);

    signal_sender.join();
}

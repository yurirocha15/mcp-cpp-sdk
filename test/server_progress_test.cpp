#include "mcp/server.hpp"

#include <gtest/gtest.h>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <functional>
#include <memory>
#include <queue>
#include <string>
#include <string_view>
#include <vector>

namespace {

class ScriptedTransport final : public mcp::ITransport {
   public:
    explicit ScriptedTransport(const boost::asio::any_io_executor& executor)
        : strand_(boost::asio::make_strand(executor)), timer_(strand_) {
        timer_.expires_at(std::chrono::steady_clock::time_point::max());
    }

    mcp::Task<std::string> read_message() override {
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

    mcp::Task<void> write_message(std::string_view message) override {
        co_await boost::asio::post(strand_, boost::asio::use_awaitable);
        written_.emplace_back(message);
        if (on_write_) {
            on_write_(written_.back());
        }
    }

    void close() override {
        closed_ = true;
        timer_.cancel();
    }

    void enqueue_message(std::string msg) {
        boost::asio::post(strand_, [this, m = std::move(msg)]() mutable {
            incoming_.push(std::move(m));
            timer_.cancel();
        });
    }

    void set_on_write(std::function<void(std::string_view)> callback) {
        on_write_ = std::move(callback);
    }

    [[nodiscard]] const std::vector<std::string>& written() const { return written_; }

   private:
    boost::asio::strand<boost::asio::any_io_executor> strand_;
    boost::asio::steady_timer timer_;
    std::queue<std::string> incoming_;
    std::vector<std::string> written_;
    std::function<void(std::string_view)> on_write_;
    bool closed_ = false;
};

struct NoopParams {
    int dummy = 0;
};

inline void to_json(nlohmann::json& j, const NoopParams& p) { j = nlohmann::json{{"dummy", p.dummy}}; }
inline void from_json(const nlohmann::json& j, NoopParams& p) { j.at("dummy").get_to(p.dummy); }

struct NoopResult {
    bool done = true;
};

inline void to_json(nlohmann::json& j, const NoopResult& r) { j = nlohmann::json{{"done", r.done}}; }
inline void from_json(const nlohmann::json& j, NoopResult& r) { j.at("done").get_to(r.done); }

nlohmann::json make_initialize_request(std::string_view req_id) {
    return {{"jsonrpc", "2.0"},
            {"id", req_id},
            {"method", "initialize"},
            {"params",
             {{"protocolVersion", mcp::LATEST_PROTOCOL_VERSION},
              {"clientInfo", {{"name", "test-client"}, {"version", "0.1"}}},
              {"capabilities", nlohmann::json::object()}}}};
}

}  // namespace

class ServerProgressTest : public ::testing::Test {
   protected:
    boost::asio::io_context io_ctx_;
};

TEST_F(ServerProgressTest, ProgressNotificationSentWithToken) {
    auto* raw_transport = new ScriptedTransport(io_ctx_.get_executor());
    auto transport = std::unique_ptr<mcp::ITransport>(raw_transport);

    mcp::Implementation info;
    info.name = "test-server";
    info.version = "1.0";

    mcp::ServerCapabilities caps;
    mcp::ServerCapabilities::ToolsCapability tools_cap;
    caps.tools = std::move(tools_cap);

    mcp::Server server(std::move(info), std::move(caps));

    server.add_tool<NoopParams, NoopResult>(
        "progress_tool", "Reports progress", nlohmann::json{{"type", "object"}},
        [](mcp::Context& ctx, NoopParams /*params*/) -> mcp::Task<NoopResult> {
            co_await ctx.report_progress(0.5, 1.0, "halfway there");
            co_return NoopResult{true};
        });

    std::vector<nlohmann::json> responses;
    raw_transport->set_on_write([&responses, raw_transport](std::string_view msg) {
        responses.push_back(nlohmann::json::parse(msg));
        // init + progress notification + tool result = 3
        if (responses.size() == 3) {
            raw_transport->close();
        }
    });

    raw_transport->enqueue_message(make_initialize_request("1").dump());

    nlohmann::json call_req;
    call_req["jsonrpc"] = "2.0";
    call_req["id"] = "2";
    call_req["method"] = "tools/call";
    call_req["params"] = nlohmann::json{{"name", "progress_tool"},
                                        {"arguments", {{"dummy", 0}}},
                                        {"_meta", {{"progressToken", "tok-123"}}}};
    raw_transport->enqueue_message(call_req.dump());

    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> { co_await server.run(std::move(transport), io_ctx_.get_executor()); },
        boost::asio::detached);

    io_ctx_.run();

    ASSERT_EQ(responses.size(), 3);

    // Second message is the progress notification
    auto& progress = responses[1];
    EXPECT_EQ(progress["method"], "notifications/progress");
    EXPECT_FALSE(progress.contains("id"));
    ASSERT_TRUE(progress.contains("params"));
    EXPECT_EQ(progress["params"]["progressToken"], "tok-123");
    EXPECT_DOUBLE_EQ(progress["params"]["progress"].get<double>(), 0.5);
    EXPECT_DOUBLE_EQ(progress["params"]["total"].get<double>(), 1.0);
    EXPECT_EQ(progress["params"]["message"], "halfway there");

    // Third is the tool result
    EXPECT_EQ(responses[2]["id"], "2");
    ASSERT_TRUE(responses[2].contains("result"));
}

TEST_F(ServerProgressTest, ProgressSilentWithoutToken) {
    auto* raw_transport = new ScriptedTransport(io_ctx_.get_executor());
    auto transport = std::unique_ptr<mcp::ITransport>(raw_transport);

    mcp::Implementation info;
    info.name = "test-server";
    info.version = "1.0";

    mcp::ServerCapabilities caps;
    mcp::ServerCapabilities::ToolsCapability tools_cap;
    caps.tools = std::move(tools_cap);

    mcp::Server server(std::move(info), std::move(caps));

    server.add_tool<NoopParams, NoopResult>(
        "silent_progress", "Reports progress but no token", nlohmann::json{{"type", "object"}},
        [](mcp::Context& ctx, NoopParams /*params*/) -> mcp::Task<NoopResult> {
            co_await ctx.report_progress(0.5, 1.0, "should not appear");
            co_return NoopResult{true};
        });

    std::vector<nlohmann::json> responses;
    raw_transport->set_on_write([&responses, raw_transport](std::string_view msg) {
        responses.push_back(nlohmann::json::parse(msg));
        // init + tool result = 2 (no progress notification because no token)
        if (responses.size() == 2) {
            raw_transport->close();
        }
    });

    raw_transport->enqueue_message(make_initialize_request("1").dump());

    // No _meta.progressToken in this request
    nlohmann::json call_req;
    call_req["jsonrpc"] = "2.0";
    call_req["id"] = "2";
    call_req["method"] = "tools/call";
    call_req["params"] = nlohmann::json{{"name", "silent_progress"}, {"arguments", {{"dummy", 0}}}};
    raw_transport->enqueue_message(call_req.dump());

    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> { co_await server.run(std::move(transport), io_ctx_.get_executor()); },
        boost::asio::detached);

    io_ctx_.run();

    // Only 2 responses: init + tool result (no progress notification)
    ASSERT_EQ(responses.size(), 2);
    EXPECT_EQ(responses[0]["id"], "1");
    EXPECT_EQ(responses[1]["id"], "2");
    ASSERT_TRUE(responses[1].contains("result"));
}

TEST_F(ServerProgressTest, MultipleProgressNotifications) {
    auto* raw_transport = new ScriptedTransport(io_ctx_.get_executor());
    auto transport = std::unique_ptr<mcp::ITransport>(raw_transport);

    mcp::Implementation info;
    info.name = "test-server";
    info.version = "1.0";

    mcp::ServerCapabilities caps;
    mcp::ServerCapabilities::ToolsCapability tools_cap;
    caps.tools = std::move(tools_cap);

    mcp::Server server(std::move(info), std::move(caps));

    server.add_tool<NoopParams, NoopResult>(
        "multi_progress", "Reports multiple progress updates", nlohmann::json{{"type", "object"}},
        [](mcp::Context& ctx, NoopParams /*params*/) -> mcp::Task<NoopResult> {
            co_await ctx.report_progress(1.0, 3.0);
            co_await ctx.report_progress(2.0, 3.0);
            co_await ctx.report_progress(3.0, 3.0);
            co_return NoopResult{true};
        });

    std::vector<nlohmann::json> responses;
    raw_transport->set_on_write([&responses, raw_transport](std::string_view msg) {
        responses.push_back(nlohmann::json::parse(msg));
        // init + 3 progress + tool result = 5
        if (responses.size() == 5) {
            raw_transport->close();
        }
    });

    raw_transport->enqueue_message(make_initialize_request("1").dump());

    nlohmann::json call_req;
    call_req["jsonrpc"] = "2.0";
    call_req["id"] = "2";
    call_req["method"] = "tools/call";
    call_req["params"] = nlohmann::json{{"name", "multi_progress"},
                                        {"arguments", {{"dummy", 0}}},
                                        {"_meta", {{"progressToken", "multi-tok"}}}};
    raw_transport->enqueue_message(call_req.dump());

    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> { co_await server.run(std::move(transport), io_ctx_.get_executor()); },
        boost::asio::detached);

    io_ctx_.run();

    ASSERT_EQ(responses.size(), 5);

    // Verify 3 progress notifications
    for (int i = 1; i <= 3; ++i) {
        auto& p = responses[i];
        EXPECT_EQ(p["method"], "notifications/progress");
        EXPECT_EQ(p["params"]["progressToken"], "multi-tok");
        EXPECT_DOUBLE_EQ(p["params"]["progress"].get<double>(), static_cast<double>(i));
        EXPECT_DOUBLE_EQ(p["params"]["total"].get<double>(), 3.0);
    }

    // Final tool result
    EXPECT_EQ(responses[4]["id"], "2");
    ASSERT_TRUE(responses[4].contains("result"));
}

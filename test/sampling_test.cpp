#include "mcp/server.hpp"

#include <gtest/gtest.h>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
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

struct EchoInput {
    std::string text;
};

inline void to_json(nlohmann::json& json_obj, const EchoInput& input) {
    json_obj = nlohmann::json{{"text", input.text}};
}

inline void from_json(const nlohmann::json& json_obj, EchoInput& input) {
    json_obj.at("text").get_to(input.text);
}

}  // namespace

class SamplingTest : public ::testing::Test {
   protected:
    boost::asio::io_context io_ctx_;
};

TEST_F(SamplingTest, HandlerCallsSampleLlmAndReceivesResult) {
    auto* raw_transport = new ScriptedTransport(io_ctx_.get_executor());
    auto transport = std::unique_ptr<mcp::ITransport>(raw_transport);

    mcp::Implementation server_info;
    server_info.name = "test-server";
    server_info.version = "1.0";

    mcp::ServerCapabilities caps;
    mcp::ServerCapabilities::ToolsCapability tools_cap;
    tools_cap.listChanged = true;
    caps.tools = std::move(tools_cap);

    mcp::Server server(std::move(server_info), std::move(caps));

    server.add_tool<EchoInput, mcp::CallToolResult>(
        "ask_llm", "Asks the LLM via sampling",
        nlohmann::json{{"type", "object"}, {"properties", {{"text", {{"type", "string"}}}}}},
        [](mcp::Context& ctx, EchoInput input) -> mcp::Task<mcp::CallToolResult> {
            mcp::CreateMessageRequestParams sample_req;
            mcp::SamplingMessage msg;
            msg.role = mcp::Role::User;
            mcp::TextContent tc;
            tc.text = input.text;
            msg.content = tc;
            sample_req.messages.push_back(std::move(msg));
            sample_req.maxTokens = 100;

            auto sample_result = co_await ctx.sample_llm(std::move(sample_req));

            mcp::CallToolResult tool_result;
            mcp::TextContent result_content;
            auto& content_block = std::get<mcp::SamplingMessageContentBlock>(sample_result.content);
            result_content.text = std::get<mcp::TextContent>(content_block).text;
            tool_result.content.push_back(std::move(result_content));
            co_return tool_result;
        });

    int write_count = 0;
    raw_transport->set_on_write([&write_count, raw_transport](std::string_view msg) {
        ++write_count;
        auto json_msg = nlohmann::json::parse(msg);

        if (json_msg.contains("method") && json_msg["method"] == "sampling/createMessage") {
            // This is the server's outgoing sampling request — mock client responds
            auto request_id = json_msg["id"].get<std::string>();
            mcp::CreateMessageResult sample_result;
            sample_result.role = mcp::Role::Assistant;
            mcp::TextContent tc;
            tc.text = "LLM says hello";
            sample_result.content = tc;
            sample_result.model = "test-model";

            nlohmann::json response_json;
            response_json["jsonrpc"] = "2.0";
            response_json["id"] = request_id;
            nlohmann::json result_val = std::move(sample_result);
            response_json["result"] = std::move(result_val);

            raw_transport->enqueue_message(response_json.dump());
        } else if (json_msg.contains("result") && !json_msg.contains("method")) {
            // This is the final tool call response — close transport
            raw_transport->close();
        }
    });

    nlohmann::json tools_call_request;
    tools_call_request["jsonrpc"] = "2.0";
    tools_call_request["id"] = "req-1";
    tools_call_request["method"] = "tools/call";
    tools_call_request["params"] = {{"name", "ask_llm"}, {"arguments", {{"text", "hello"}}}};

    raw_transport->enqueue_message(tools_call_request.dump());

    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> { co_await server.run(std::move(transport), io_ctx_.get_executor()); },
        boost::asio::detached);

    io_ctx_.run();

    ASSERT_GE(write_count, 2);

    auto sampling_req = nlohmann::json::parse(raw_transport->written()[0]);
    EXPECT_EQ(sampling_req["method"], "sampling/createMessage");
    ASSERT_TRUE(sampling_req.contains("params"));
    EXPECT_EQ(sampling_req["params"]["maxTokens"], 100);

    auto tool_response = nlohmann::json::parse(raw_transport->written()[1]);
    EXPECT_EQ(tool_response["id"], "req-1");
    ASSERT_TRUE(tool_response.contains("result"));
    auto content_arr = tool_response["result"]["content"];
    ASSERT_FALSE(content_arr.empty());
    EXPECT_EQ(content_arr[0]["text"], "LLM says hello");
}

TEST_F(SamplingTest, SampleLlmWithoutSenderThrows) {
    auto* raw_transport = new ScriptedTransport(io_ctx_.get_executor());

    mcp::Context ctx(*raw_transport);

    bool threw = false;
    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            try {
                mcp::CreateMessageRequestParams req;
                req.maxTokens = 10;
                co_await ctx.sample_llm(std::move(req));
            } catch (const std::runtime_error&) {
                threw = true;
            }
            raw_transport->close();
        },
        boost::asio::detached);

    io_ctx_.run();
    EXPECT_TRUE(threw);
}

TEST_F(SamplingTest, SampleLlmErrorResponseThrows) {
    auto* raw_transport = new ScriptedTransport(io_ctx_.get_executor());
    auto transport = std::unique_ptr<mcp::ITransport>(raw_transport);

    mcp::Implementation server_info;
    server_info.name = "test-server";
    server_info.version = "1.0";

    mcp::Server server(std::move(server_info), mcp::ServerCapabilities{});

    bool handler_threw = false;

    server.add_tool<EchoInput, mcp::CallToolResult>(
        "fail_tool", "Tool that gets an error from sampling", nlohmann::json{{"type", "object"}},
        [&handler_threw](mcp::Context& ctx, EchoInput /*input*/) -> mcp::Task<mcp::CallToolResult> {
            mcp::CreateMessageRequestParams sample_req;
            sample_req.maxTokens = 10;

            try {
                co_await ctx.sample_llm(std::move(sample_req));
            } catch (const std::runtime_error&) {
                handler_threw = true;
            }

            mcp::CallToolResult result;
            mcp::TextContent tc;
            tc.text = "recovered";
            result.content.push_back(std::move(tc));
            co_return result;
        });

    raw_transport->set_on_write([raw_transport](std::string_view msg) {
        auto json_msg = nlohmann::json::parse(msg);

        if (json_msg.contains("method") && json_msg["method"] == "sampling/createMessage") {
            auto request_id = json_msg["id"].get<std::string>();

            nlohmann::json error_response;
            error_response["jsonrpc"] = "2.0";
            error_response["id"] = request_id;
            error_response["error"] = {{"code", -32600}, {"message", "sampling denied"}};

            raw_transport->enqueue_message(error_response.dump());
        } else if (json_msg.contains("result") && !json_msg.contains("method")) {
            raw_transport->close();
        }
    });

    nlohmann::json tools_call_request;
    tools_call_request["jsonrpc"] = "2.0";
    tools_call_request["id"] = "req-2";
    tools_call_request["method"] = "tools/call";
    tools_call_request["params"] = {{"name", "fail_tool"}, {"arguments", {{"text", "test"}}}};

    raw_transport->enqueue_message(tools_call_request.dump());

    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> { co_await server.run(std::move(transport), io_ctx_.get_executor()); },
        boost::asio::detached);

    io_ctx_.run();

    EXPECT_TRUE(handler_threw);

    bool found_result = false;
    for (const auto& w : raw_transport->written()) {
        auto parsed = nlohmann::json::parse(w);
        if (parsed.contains("result") && parsed.value("id", "") == "req-2") {
            EXPECT_EQ(parsed["result"]["content"][0]["text"], "recovered");
            found_result = true;
        }
    }
    EXPECT_TRUE(found_result);
}

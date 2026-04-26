#include "mcp/server.hpp"

#include <gtest/gtest.h>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
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
        : strand_(boost::asio::make_strand(executor)), timer_(executor) {}

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

            timer_.expires_after(std::chrono::milliseconds(10));
            boost::system::error_code ec;
            co_await timer_.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, ec));
        }
    }

    mcp::Task<void> write_message(std::string_view message) override {
        co_await boost::asio::post(strand_, boost::asio::use_awaitable);
        if (closed_) {
            throw std::runtime_error("transport closed");
        }
        written_.emplace_back(message);
        if (on_write_) {
            on_write_(written_.back());
        }
    }

    void close() override { closed_ = true; }

    void enqueue_message(std::string msg) {
        if (!closed_) {
            incoming_.push(std::move(msg));
        }
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
    auto transport = std::make_shared<ScriptedTransport>(io_ctx_.get_executor());
    auto* raw_transport = transport.get();

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
            msg.role = mcp::Role::eUser;
            mcp::TextContent tc;
            tc.text = input.text;
            msg.content = tc;
            sample_req.messages.push_back(std::move(msg));
            sample_req.maxTokens = 100;

            auto sample_result = co_await ctx.sample_llm(std::move(sample_req));

            mcp::CallToolResult tool_result;
            mcp::TextContent result_content;
            if (sample_result.content.index() != 0) {
                throw std::runtime_error("Expected single content block");
            }
            auto& content_block = std::get<0>(sample_result.content);
            if (content_block.index() != 0) {  // TextContent is index 0 in ContentBlock
                throw std::runtime_error("Expected TextContent");
            }
            auto& result_content_block = std::get<mcp::TextContent>(content_block);
            result_content.text = result_content_block.text;
            tool_result.content.push_back(std::move(result_content));
            co_return tool_result;
        });

    int write_count = 0;
    std::vector<std::string> written_messages;
    raw_transport->set_on_write([&write_count, &written_messages, raw_transport](std::string_view msg) {
        try {
            ++write_count;
            written_messages.emplace_back(msg);
            auto json_msg = nlohmann::json::parse(msg);

            if (json_msg.contains("method") && json_msg["method"] == "sampling/createMessage") {
                auto request_id = json_msg["id"].get<std::string>();
                mcp::CreateMessageResult sample_result;
                sample_result.role = mcp::Role::eAssistant;
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
            } else if ((json_msg.contains("result") || json_msg.contains("error")) &&
                       !json_msg.contains("method")) {
                raw_transport->close();
            }
        } catch (...) {
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
        io_ctx_, [&]() -> mcp::Task<void> { co_await server.run(transport, io_ctx_.get_executor()); },
        boost::asio::detached);

    io_ctx_.run();

    ASSERT_GE(write_count, 2);

    auto sampling_req = nlohmann::json::parse(written_messages[0]);
    EXPECT_EQ(sampling_req["method"], "sampling/createMessage");
    ASSERT_TRUE(sampling_req.contains("params"));
    EXPECT_EQ(sampling_req["params"]["maxTokens"], 100);

    auto tool_response = nlohmann::json::parse(written_messages[1]);
    EXPECT_EQ(tool_response["id"], "req-1");
    ASSERT_TRUE(tool_response.contains("result"));
    auto content_arr = tool_response["result"]["content"];
    ASSERT_FALSE(content_arr.empty());
    EXPECT_EQ(content_arr[0]["text"], "LLM says hello");
}

TEST_F(SamplingTest, SampleLlmWithoutSenderThrows) {
    auto transport = std::make_shared<ScriptedTransport>(io_ctx_.get_executor());
    auto* raw_transport = transport.get();

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

TEST_F(SamplingTest, SamplingResponseWithToolUseContent) {
    auto transport = std::make_shared<ScriptedTransport>(io_ctx_.get_executor());
    auto* raw_transport = transport.get();

    mcp::Implementation server_info;
    server_info.name = "test-server";
    server_info.version = "1.0";

    mcp::ServerCapabilities caps;
    mcp::ServerCapabilities::ToolsCapability tools_cap;
    tools_cap.listChanged = true;
    caps.tools = std::move(tools_cap);

    mcp::Server server(std::move(server_info), std::move(caps));

    std::string received_tool_name;
    std::string received_tool_id;
    nlohmann::json received_tool_input;

    server.add_tool<EchoInput, mcp::CallToolResult>(
        "ask_with_tools", "Asks the LLM and expects tool use",
        nlohmann::json{{"type", "object"}, {"properties", {{"text", {{"type", "string"}}}}}},
        [&](mcp::Context& ctx, EchoInput input) -> mcp::Task<mcp::CallToolResult> {
            mcp::CreateMessageRequestParams sample_req;
            mcp::SamplingMessage msg;
            msg.role = mcp::Role::eUser;
            mcp::TextContent tc;
            tc.text = input.text;
            msg.content = tc;
            sample_req.messages.push_back(std::move(msg));
            sample_req.maxTokens = 100;

            auto sample_result = co_await ctx.sample_llm(std::move(sample_req));

            if (sample_result.content.index() != 0) {
                throw std::runtime_error("Expected single content block");
            }
            auto& content_block = std::get<0>(sample_result.content);
            if (content_block.index() != 5) {  // ToolUseContent is index 5 in ContentBlock
                throw std::runtime_error("Expected ToolUseContent");
            }
            auto& tool_use = std::get<mcp::ToolUseContent>(content_block);
            received_tool_name = tool_use.name;
            received_tool_id = tool_use.id;
            received_tool_input = tool_use.input;

            mcp::CallToolResult tool_result;
            mcp::TextContent result_content;
            result_content.text = "tool_use received: " + tool_use.name;
            tool_result.content.push_back(std::move(result_content));
            co_return tool_result;
        });

    raw_transport->set_on_write([raw_transport](std::string_view msg) {
        try {
            auto json_msg = nlohmann::json::parse(msg);

            if (json_msg.contains("method") && json_msg["method"] == "sampling/createMessage") {
                auto request_id = json_msg["id"].get<std::string>();
                mcp::CreateMessageResult sample_result;
                sample_result.role = mcp::Role::eAssistant;
                mcp::ToolUseContent tuc;
                tuc.id = "call-123";
                tuc.name = "get_weather";
                tuc.input = {{"location", "Tokyo"}};
                sample_result.content = tuc;
                sample_result.model = "test-model";

                nlohmann::json response_json;
                response_json["jsonrpc"] = "2.0";
                response_json["id"] = request_id;
                nlohmann::json result_val = std::move(sample_result);
                response_json["result"] = std::move(result_val);

                raw_transport->enqueue_message(response_json.dump());
            } else if ((json_msg.contains("result") || json_msg.contains("error")) &&
                       !json_msg.contains("method")) {
                raw_transport->close();
            }
        } catch (...) {
            raw_transport->close();
        }
    });

    nlohmann::json tools_call_request;
    tools_call_request["jsonrpc"] = "2.0";
    tools_call_request["id"] = "req-tool-use";
    tools_call_request["method"] = "tools/call";
    tools_call_request["params"] = {{"name", "ask_with_tools"}, {"arguments", {{"text", "weather"}}}};

    raw_transport->enqueue_message(tools_call_request.dump());

    boost::asio::co_spawn(
        io_ctx_, [&]() -> mcp::Task<void> { co_await server.run(transport, io_ctx_.get_executor()); },
        boost::asio::detached);

    io_ctx_.run();

    EXPECT_EQ(received_tool_name, "get_weather");
    EXPECT_EQ(received_tool_id, "call-123");
    EXPECT_EQ(received_tool_input, nlohmann::json({{"location", "Tokyo"}}));
}

TEST_F(SamplingTest, SamplingResponseWithToolResultContent) {
    auto transport = std::make_shared<ScriptedTransport>(io_ctx_.get_executor());
    auto* raw_transport = transport.get();

    mcp::Implementation server_info;
    server_info.name = "test-server";
    server_info.version = "1.0";

    mcp::ServerCapabilities caps;
    mcp::ServerCapabilities::ToolsCapability tools_cap;
    tools_cap.listChanged = true;
    caps.tools = std::move(tools_cap);

    mcp::Server server(std::move(server_info), std::move(caps));

    std::string received_tool_use_id;
    bool received_is_error = true;
    std::string received_content_text;

    server.add_tool<EchoInput, mcp::CallToolResult>(
        "ask_tool_result", "Expects tool result content",
        nlohmann::json{{"type", "object"}, {"properties", {{"text", {{"type", "string"}}}}}},
        [&](mcp::Context& ctx, EchoInput input) -> mcp::Task<mcp::CallToolResult> {
            mcp::CreateMessageRequestParams sample_req;
            mcp::SamplingMessage msg;
            msg.role = mcp::Role::eUser;
            mcp::TextContent tc;
            tc.text = input.text;
            msg.content = tc;
            sample_req.messages.push_back(std::move(msg));
            sample_req.maxTokens = 100;

            auto sample_result = co_await ctx.sample_llm(std::move(sample_req));

            if (sample_result.content.index() != 0) {
                throw std::runtime_error("Expected single content block");
            }
            auto& content_block = std::get<0>(sample_result.content);
            if (content_block.index() != 6) {  // ToolResultContent is index 6 in ContentBlock
                throw std::runtime_error("Expected ToolResultContent");
            }
            auto& tool_result = std::get<mcp::ToolResultContent>(content_block);
            received_tool_use_id = tool_result.toolUseId;
            received_is_error = tool_result.isError.value_or(true);
            if (!tool_result.content.empty()) {
                received_content_text = tool_result.content[0]["text"].get<std::string>();
            }

            mcp::CallToolResult result;
            mcp::TextContent result_content;
            result_content.text = "tool_result received";
            result.content.push_back(std::move(result_content));
            co_return result;
        });

    raw_transport->set_on_write([raw_transport](std::string_view msg) {
        try {
            auto json_msg = nlohmann::json::parse(msg);

            if (json_msg.contains("method") && json_msg["method"] == "sampling/createMessage") {
                auto request_id = json_msg["id"].get<std::string>();
                mcp::CreateMessageResult sample_result;
                sample_result.role = mcp::Role::eAssistant;
                mcp::ToolResultContent trc;
                trc.toolUseId = "call-456";
                mcp::TextContent inner_tc;
                inner_tc.text = "Weather in Tokyo: 22 degrees";
                trc.content.push_back(nlohmann::json(inner_tc));
                trc.isError = false;
                sample_result.content = trc;
                sample_result.model = "test-model";

                nlohmann::json response_json;
                response_json["jsonrpc"] = "2.0";
                response_json["id"] = request_id;
                nlohmann::json result_val = std::move(sample_result);
                response_json["result"] = std::move(result_val);

                raw_transport->enqueue_message(response_json.dump());
            } else if ((json_msg.contains("result") || json_msg.contains("error")) &&
                       !json_msg.contains("method")) {
                raw_transport->close();
            }
        } catch (...) {
            raw_transport->close();
        }
    });

    nlohmann::json tools_call_request;
    tools_call_request["jsonrpc"] = "2.0";
    tools_call_request["id"] = "req-tool-result";
    tools_call_request["method"] = "tools/call";
    tools_call_request["params"] = {{"name", "ask_tool_result"}, {"arguments", {{"text", "result"}}}};

    raw_transport->enqueue_message(tools_call_request.dump());

    boost::asio::co_spawn(
        io_ctx_, [&]() -> mcp::Task<void> { co_await server.run(transport, io_ctx_.get_executor()); },
        boost::asio::detached);

    io_ctx_.run();

    EXPECT_EQ(received_tool_use_id, "call-456");
    EXPECT_FALSE(received_is_error);
    EXPECT_EQ(received_content_text, "Weather in Tokyo: 22 degrees");
}

TEST_F(SamplingTest, ToolUseContentRoundtripSerialization) {
    mcp::ToolUseContent original;
    original.id = "call-789";
    original.name = "search";
    original.input = {{"query", "test"}, {"limit", 10}};

    nlohmann::json serialized = original;
    EXPECT_EQ(serialized["type"], "tool_use");
    EXPECT_EQ(serialized["id"], "call-789");
    EXPECT_EQ(serialized["name"], "search");
    EXPECT_EQ(serialized["input"]["query"], "test");
    EXPECT_EQ(serialized["input"]["limit"], 10);

    mcp::SamplingMessageContentBlock block = original;
    nlohmann::json block_json = block;
    EXPECT_EQ(block_json["type"], "tool_use");

    mcp::SamplingMessageContentBlock deserialized;
    from_json(block_json, deserialized);
    auto& recovered = std::get<mcp::ToolUseContent>(deserialized);
    EXPECT_EQ(recovered.id, "call-789");
    EXPECT_EQ(recovered.name, "search");
    EXPECT_EQ(recovered.input, original.input);
}

TEST_F(SamplingTest, ToolResultContentRoundtripSerialization) {
    mcp::ToolResultContent original;
    original.toolUseId = "call-789";
    mcp::TextContent inner;
    inner.text = "result text";
    original.content.push_back(nlohmann::json(inner));
    original.isError = false;

    nlohmann::json serialized = original;
    EXPECT_EQ(serialized["type"], "tool_result");
    EXPECT_EQ(serialized["toolUseId"], "call-789");
    EXPECT_FALSE(serialized["isError"].get<bool>());
    EXPECT_EQ(serialized["content"][0]["text"], "result text");

    mcp::SamplingMessageContentBlock block = original;
    nlohmann::json block_json = block;
    EXPECT_EQ(block_json["type"], "tool_result");

    mcp::SamplingMessageContentBlock deserialized;
    from_json(block_json, deserialized);
    auto& recovered = std::get<mcp::ToolResultContent>(deserialized);
    EXPECT_EQ(recovered.toolUseId, "call-789");
    EXPECT_FALSE(recovered.isError.value());
    ASSERT_EQ(recovered.content.size(), 1);
    EXPECT_EQ(recovered.content[0]["type"], "text");
    EXPECT_EQ(recovered.content[0]["text"], "result text");
}

TEST_F(SamplingTest, SampleLlmErrorResponseThrows) {
    auto transport = std::make_shared<ScriptedTransport>(io_ctx_.get_executor());
    auto* raw_transport = transport.get();

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

    std::vector<std::string> written_messages;
    raw_transport->set_on_write([&written_messages, raw_transport](std::string_view msg) {
        try {
            written_messages.emplace_back(msg);
            auto json_msg = nlohmann::json::parse(msg);

            if (json_msg.contains("method") && json_msg["method"] == "sampling/createMessage") {
                auto request_id = json_msg["id"].get<std::string>();

                nlohmann::json error_response;
                error_response["jsonrpc"] = "2.0";
                error_response["id"] = request_id;
                error_response["error"] = {{"code", mcp::g_INVALID_REQUEST},
                                           {"message", "sampling denied"}};

                raw_transport->enqueue_message(error_response.dump());
            } else if ((json_msg.contains("result") || json_msg.contains("error")) &&
                       !json_msg.contains("method")) {
                raw_transport->close();
            }
        } catch (...) {
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
        io_ctx_, [&]() -> mcp::Task<void> { co_await server.run(transport, io_ctx_.get_executor()); },
        boost::asio::detached);

    io_ctx_.run();

    EXPECT_TRUE(handler_threw);

    bool found_result = false;
    for (const auto& w : written_messages) {
        auto parsed = nlohmann::json::parse(w);
        if (parsed.contains("result") && parsed.value("id", "") == "req-2") {
            EXPECT_EQ(parsed["result"]["content"][0]["text"], "recovered");
            found_result = true;
        }
    }
    EXPECT_TRUE(found_result);
}

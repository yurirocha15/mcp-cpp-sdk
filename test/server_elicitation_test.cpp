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

struct DummyInput {
    std::string text;
};

inline void to_json(nlohmann::json& j, const DummyInput& input) {
    j = nlohmann::json{{"text", input.text}};
}

inline void from_json(const nlohmann::json& j, DummyInput& input) { j.at("text").get_to(input.text); }

}  // namespace

class ElicitationTest : public ::testing::Test {
   protected:
    boost::asio::io_context io_ctx_;
};

TEST_F(ElicitationTest, FormElicitationRoundtrip) {
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

    server.add_tool<DummyInput, mcp::CallToolResult>(
        "ask_user", "Asks the user via elicitation",
        nlohmann::json{{"type", "object"}, {"properties", {{"text", {{"type", "string"}}}}}},
        [](mcp::Context& ctx, DummyInput /*input*/) -> mcp::Task<mcp::CallToolResult> {
            mcp::ElicitRequestFormParams form_params;
            form_params.message = "Please enter your name";
            form_params.requestedSchema =
                nlohmann::json{{"type", "object"}, {"properties", {{"name", {{"type", "string"}}}}}};

            auto result = co_await ctx.elicit(std::move(form_params));

            mcp::CallToolResult tool_result;
            mcp::TextContent tc;
            if (result.action == mcp::ElicitAction::eAccept && result.content) {
                tc.text = result.content->dump();
            } else {
                tc.text = "declined";
            }
            tool_result.content.push_back(std::move(tc));
            co_return tool_result;
        });

    std::vector<std::string> written_messages;
    raw_transport->set_on_write([&written_messages, raw_transport](std::string_view msg) {
        written_messages.emplace_back(msg);
        auto json_msg = nlohmann::json::parse(msg);

        if (json_msg.contains("method") && json_msg["method"] == "elicitation/create") {
            auto request_id = json_msg["id"].get<std::string>();

            EXPECT_EQ(json_msg["params"]["mode"], "form");
            EXPECT_EQ(json_msg["params"]["message"], "Please enter your name");

            mcp::ElicitResult elicit_result;
            elicit_result.action = mcp::ElicitAction::eAccept;
            elicit_result.content = nlohmann::json{{"name", "Alice"}};

            nlohmann::json response_json;
            response_json["jsonrpc"] = "2.0";
            response_json["id"] = request_id;
            nlohmann::json result_val = std::move(elicit_result);
            response_json["result"] = std::move(result_val);

            raw_transport->enqueue_message(response_json.dump());
        } else if (json_msg.contains("result") && !json_msg.contains("method")) {
            raw_transport->close();
        }
    });

    nlohmann::json tool_call_request;
    tool_call_request["jsonrpc"] = "2.0";
    tool_call_request["id"] = "req-1";
    tool_call_request["method"] = "tools/call";
    tool_call_request["params"] = {{"name", "ask_user"}, {"arguments", {{"text", "hello"}}}};

    raw_transport->enqueue_message(tool_call_request.dump());

    boost::asio::co_spawn(
        io_ctx_, [&]() -> mcp::Task<void> { co_await server.run(transport, io_ctx_.get_executor()); },
        boost::asio::detached);

    io_ctx_.run();

    ASSERT_GE(written_messages.size(), 2u);

    auto elicit_req = nlohmann::json::parse(written_messages[0]);
    EXPECT_EQ(elicit_req["method"], "elicitation/create");
    EXPECT_EQ(elicit_req["params"]["mode"], "form");

    auto tool_response = nlohmann::json::parse(written_messages[1]);
    EXPECT_EQ(tool_response["id"], "req-1");
    ASSERT_TRUE(tool_response.contains("result"));
    auto content_arr = tool_response["result"]["content"];
    ASSERT_FALSE(content_arr.empty());
    EXPECT_EQ(content_arr[0]["text"], "{\"name\":\"Alice\"}");
}

TEST_F(ElicitationTest, URLElicitationRoundtrip) {
    auto transport = std::make_shared<ScriptedTransport>(io_ctx_.get_executor());
    auto* raw_transport = transport.get();

    mcp::Implementation server_info;
    server_info.name = "test-server";
    server_info.version = "1.0";

    mcp::Server server(std::move(server_info), mcp::ServerCapabilities{});

    server.add_tool<DummyInput, mcp::CallToolResult>(
        "oauth_tool", "Requires user auth via URL",
        nlohmann::json{{"type", "object"}, {"properties", {{"text", {{"type", "string"}}}}}},
        [](mcp::Context& ctx, DummyInput /*input*/) -> mcp::Task<mcp::CallToolResult> {
            mcp::ElicitRequestURLParams url_params;
            url_params.message = "Please authenticate";
            url_params.url = "https://example.com/auth";
            url_params.elicitationId = "elicit-123";

            auto result = co_await ctx.elicit(std::move(url_params));

            mcp::CallToolResult tool_result;
            mcp::TextContent tc;
            tc.text =
                (result.action == mcp::ElicitAction::eAccept) ? "authenticated" : "not_authenticated";
            tool_result.content.push_back(std::move(tc));
            co_return tool_result;
        });

    std::vector<std::string> written_messages;
    raw_transport->set_on_write([&written_messages, raw_transport](std::string_view msg) {
        written_messages.emplace_back(msg);
        auto json_msg = nlohmann::json::parse(msg);

        if (json_msg.contains("method") && json_msg["method"] == "elicitation/create") {
            auto request_id = json_msg["id"].get<std::string>();

            EXPECT_EQ(json_msg["params"]["mode"], "url");
            EXPECT_EQ(json_msg["params"]["url"], "https://example.com/auth");
            EXPECT_EQ(json_msg["params"]["elicitationId"], "elicit-123");

            mcp::ElicitResult elicit_result;
            elicit_result.action = mcp::ElicitAction::eAccept;

            nlohmann::json response_json;
            response_json["jsonrpc"] = "2.0";
            response_json["id"] = request_id;
            nlohmann::json result_val = std::move(elicit_result);
            response_json["result"] = std::move(result_val);

            raw_transport->enqueue_message(response_json.dump());
        } else if (json_msg.contains("result") && !json_msg.contains("method")) {
            raw_transport->close();
        }
    });

    nlohmann::json tool_call_request;
    tool_call_request["jsonrpc"] = "2.0";
    tool_call_request["id"] = "req-2";
    tool_call_request["method"] = "tools/call";
    tool_call_request["params"] = {{"name", "oauth_tool"}, {"arguments", {{"text", "go"}}}};

    raw_transport->enqueue_message(tool_call_request.dump());

    boost::asio::co_spawn(
        io_ctx_, [&]() -> mcp::Task<void> { co_await server.run(transport, io_ctx_.get_executor()); },
        boost::asio::detached);

    io_ctx_.run();

    ASSERT_GE(written_messages.size(), 2u);

    auto elicit_req = nlohmann::json::parse(written_messages[0]);
    EXPECT_EQ(elicit_req["method"], "elicitation/create");
    EXPECT_EQ(elicit_req["params"]["mode"], "url");

    auto tool_response = nlohmann::json::parse(written_messages[1]);
    EXPECT_EQ(tool_response["id"], "req-2");
    auto content_arr = tool_response["result"]["content"];
    ASSERT_FALSE(content_arr.empty());
    EXPECT_EQ(content_arr[0]["text"], "authenticated");
}

TEST_F(ElicitationTest, ClientDeclinesElicitation) {
    auto transport = std::make_shared<ScriptedTransport>(io_ctx_.get_executor());
    auto* raw_transport = transport.get();

    mcp::Implementation server_info;
    server_info.name = "test-server";
    server_info.version = "1.0";

    mcp::Server server(std::move(server_info), mcp::ServerCapabilities{});

    server.add_tool<DummyInput, mcp::CallToolResult>(
        "needs_input", "Elicits form input",
        nlohmann::json{{"type", "object"}, {"properties", {{"text", {{"type", "string"}}}}}},
        [](mcp::Context& ctx, DummyInput /*input*/) -> mcp::Task<mcp::CallToolResult> {
            mcp::ElicitRequestFormParams form_params;
            form_params.message = "Enter something";
            form_params.requestedSchema = nlohmann::json{{"type", "object"}};

            auto result = co_await ctx.elicit(std::move(form_params));

            mcp::CallToolResult tool_result;
            mcp::TextContent tc;
            if (result.action == mcp::ElicitAction::eDecline) {
                tc.text = "user_declined";
            } else if (result.action == mcp::ElicitAction::eCancel) {
                tc.text = "user_cancelled";
            } else {
                tc.text = "unexpected";
            }
            tool_result.content.push_back(std::move(tc));
            co_return tool_result;
        });

    std::vector<std::string> written_messages;
    raw_transport->set_on_write([&written_messages, raw_transport](std::string_view msg) {
        written_messages.emplace_back(msg);
        auto json_msg = nlohmann::json::parse(msg);

        if (json_msg.contains("method") && json_msg["method"] == "elicitation/create") {
            auto request_id = json_msg["id"].get<std::string>();

            mcp::ElicitResult elicit_result;
            elicit_result.action = mcp::ElicitAction::eDecline;

            nlohmann::json response_json;
            response_json["jsonrpc"] = "2.0";
            response_json["id"] = request_id;
            nlohmann::json result_val = std::move(elicit_result);
            response_json["result"] = std::move(result_val);

            raw_transport->enqueue_message(response_json.dump());
        } else if (json_msg.contains("result") && !json_msg.contains("method")) {
            raw_transport->close();
        }
    });

    nlohmann::json tool_call_request;
    tool_call_request["jsonrpc"] = "2.0";
    tool_call_request["id"] = "req-3";
    tool_call_request["method"] = "tools/call";
    tool_call_request["params"] = {{"name", "needs_input"}, {"arguments", {{"text", "test"}}}};

    raw_transport->enqueue_message(tool_call_request.dump());

    boost::asio::co_spawn(
        io_ctx_, [&]() -> mcp::Task<void> { co_await server.run(transport, io_ctx_.get_executor()); },
        boost::asio::detached);

    io_ctx_.run();

    ASSERT_GE(written_messages.size(), 2u);

    auto tool_response = nlohmann::json::parse(written_messages[1]);
    EXPECT_EQ(tool_response["id"], "req-3");
    auto content_arr = tool_response["result"]["content"];
    ASSERT_FALSE(content_arr.empty());
    EXPECT_EQ(content_arr[0]["text"], "user_declined");
}

TEST_F(ElicitationTest, ElicitWithoutSenderThrows) {
    auto* raw_transport = new ScriptedTransport(io_ctx_.get_executor());

    mcp::Context ctx(*raw_transport);

    bool threw = false;
    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            try {
                mcp::ElicitRequestFormParams form_params;
                form_params.message = "test";
                form_params.requestedSchema = nlohmann::json{{"type", "object"}};
                co_await ctx.elicit(std::move(form_params));
            } catch (const std::runtime_error&) {
                threw = true;
            }
            raw_transport->close();
        },
        boost::asio::detached);

    io_ctx_.run();
    EXPECT_TRUE(threw);
}

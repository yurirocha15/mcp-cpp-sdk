#include "mcp/client.hpp"
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

class RootsTest : public ::testing::Test {
   protected:
    boost::asio::io_context io_ctx_;
};

TEST_F(RootsTest, ServerRequestsRootsFromClient) {
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
        "get_roots", "Gets roots from client",
        nlohmann::json{{"type", "object"}, {"properties", {{"text", {{"type", "string"}}}}}},
        [](mcp::Context& ctx, DummyInput /*input*/) -> mcp::Task<mcp::CallToolResult> {
            auto roots_result = co_await ctx.request_roots();

            mcp::CallToolResult tool_result;
            mcp::TextContent tc;
            std::string text;
            for (const auto& root : roots_result.roots) {
                if (!text.empty()) {
                    text += ",";
                }
                text += root.uri;
                if (root.name) {
                    text += "(" + *root.name + ")";
                }
            }
            tc.text = text;
            tool_result.content.push_back(std::move(tc));
            co_return tool_result;
        });

    std::vector<std::string> written_messages;
    raw_transport->set_on_write([&written_messages, raw_transport](std::string_view msg) {
        written_messages.emplace_back(msg);
        auto json_msg = nlohmann::json::parse(msg);

        if (json_msg.contains("method") && json_msg["method"] == "roots/list") {
            auto request_id = json_msg["id"].get<std::string>();

            mcp::ListRootsResult roots_result;
            mcp::Root root1;
            root1.uri = "file:///home/user/project";
            root1.name = "project";
            mcp::Root root2;
            root2.uri = "file:///home/user/data";
            roots_result.roots = {root1, root2};

            nlohmann::json response_json;
            response_json["jsonrpc"] = "2.0";
            response_json["id"] = request_id;
            nlohmann::json result_val = std::move(roots_result);
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
    tool_call_request["params"] = {{"name", "get_roots"}, {"arguments", {{"text", "go"}}}};

    raw_transport->enqueue_message(tool_call_request.dump());

    boost::asio::co_spawn(
        io_ctx_, [&]() -> mcp::Task<void> { co_await server.run(transport, io_ctx_.get_executor()); },
        boost::asio::detached);

    io_ctx_.run();

    ASSERT_GE(written_messages.size(), 2u);

    auto roots_req = nlohmann::json::parse(written_messages[0]);
    EXPECT_EQ(roots_req["method"], "roots/list");

    auto tool_response = nlohmann::json::parse(written_messages[1]);
    EXPECT_EQ(tool_response["id"], "req-1");
    ASSERT_TRUE(tool_response.contains("result"));
    auto content_arr = tool_response["result"]["content"];
    ASSERT_FALSE(content_arr.empty());
    EXPECT_EQ(content_arr[0]["text"], "file:///home/user/project(project),file:///home/user/data");
}

TEST_F(RootsTest, RequestRootsWithoutSenderThrows) {
    auto* raw_transport = new ScriptedTransport(io_ctx_.get_executor());

    mcp::Context ctx(*raw_transport);

    bool threw = false;
    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            try {
                co_await ctx.request_roots();
            } catch (const std::runtime_error&) {
                threw = true;
            }
            raw_transport->close();
        },
        boost::asio::detached);

    io_ctx_.run();
    EXPECT_TRUE(threw);
}

TEST_F(RootsTest, ClientSetRootsServesRootsList) {
    auto client_transport_ptr = std::make_shared<ScriptedTransport>(io_ctx_.get_executor());
    auto* client_transport = client_transport_ptr.get();

    mcp::Client client(client_transport_ptr, io_ctx_.get_executor());

    mcp::Root root1;
    root1.uri = "file:///workspace/src";
    root1.name = "source";
    mcp::Root root2;
    root2.uri = "file:///workspace/tests";
    root2.name = "tests";

    client.set_roots({root1, root2});

    int write_count = 0;
    std::vector<std::string> written_messages;
    client_transport->set_on_write([&write_count, &written_messages,
                                    client_transport](std::string_view msg) {
        ++write_count;
        written_messages.emplace_back(msg);
        auto json_msg = nlohmann::json::parse(msg);

        if (write_count == 1) {
            // First write is the initialize request from connect(); respond to it
            auto request_id = json_msg["id"].get<std::string>();

            mcp::InitializeResult init_result;
            init_result.protocolVersion = std::string(mcp::g_LATEST_PROTOCOL_VERSION);
            init_result.serverInfo.name = "test-server";
            init_result.serverInfo.version = "1.0";

            nlohmann::json response_json;
            response_json["jsonrpc"] = "2.0";
            response_json["id"] = request_id;
            nlohmann::json result_val = std::move(init_result);
            response_json["result"] = std::move(result_val);

            client_transport->enqueue_message(response_json.dump());
        } else if (json_msg.contains("method") && json_msg["method"] == "notifications/initialized") {
            nlohmann::json roots_request;
            roots_request["jsonrpc"] = "2.0";
            roots_request["id"] = "srv-1";
            roots_request["method"] = "roots/list";
            roots_request["params"] = nlohmann::json::object();

            client_transport->enqueue_message(roots_request.dump());
        } else if (json_msg.contains("result") && json_msg.value("id", "") == "srv-1") {
            client_transport->close();
        }
    });

    mcp::Implementation client_info;
    client_info.name = "test-client";
    client_info.version = "1.0";

    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            co_await client.connect(std::move(client_info), mcp::ClientCapabilities{});
        },
        boost::asio::detached);

    io_ctx_.run();

    bool found_roots_response = false;
    for (const auto& msg : written_messages) {
        auto parsed = nlohmann::json::parse(msg);
        if (parsed.contains("result") && parsed.value("id", "") == "srv-1") {
            found_roots_response = true;
            auto roots_arr = parsed["result"]["roots"];
            ASSERT_EQ(roots_arr.size(), 2u);
            EXPECT_EQ(roots_arr[0]["uri"], "file:///workspace/src");
            EXPECT_EQ(roots_arr[0]["name"], "source");
            EXPECT_EQ(roots_arr[1]["uri"], "file:///workspace/tests");
            EXPECT_EQ(roots_arr[1]["name"], "tests");
        }
    }
    EXPECT_TRUE(found_roots_response);
}

TEST_F(RootsTest, ClientSetRootsWithNotifySendsNotification) {
    auto client_transport_ptr = std::make_shared<ScriptedTransport>(io_ctx_.get_executor());
    auto* client_transport = client_transport_ptr.get();

    mcp::Client client(client_transport_ptr, io_ctx_.get_executor());

    int write_count = 0;
    std::vector<std::string> written_messages;
    client_transport->set_on_write(
        [&write_count, &written_messages, client_transport](std::string_view msg) {
            ++write_count;
            written_messages.emplace_back(msg);
            auto json_msg = nlohmann::json::parse(msg);

            if (write_count == 1) {
                auto request_id = json_msg["id"].get<std::string>();

                mcp::InitializeResult init_result;
                init_result.protocolVersion = std::string(mcp::g_LATEST_PROTOCOL_VERSION);
                init_result.serverInfo.name = "test-server";
                init_result.serverInfo.version = "1.0";
                mcp::ServerCapabilities::ResourcesCapability res_cap;
                res_cap.listChanged = true;
                init_result.capabilities.resources = std::move(res_cap);

                nlohmann::json response_json;
                response_json["jsonrpc"] = "2.0";
                response_json["id"] = request_id;
                nlohmann::json result_val = std::move(init_result);
                response_json["result"] = std::move(result_val);

                client_transport->enqueue_message(response_json.dump());
            } else if (json_msg.contains("method") &&
                       json_msg["method"] == "notifications/roots/list_changed") {
                client_transport->close();
            }
        });

    mcp::Implementation client_info;
    client_info.name = "test-client";
    client_info.version = "1.0";

    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            co_await client.connect(std::move(client_info), mcp::ClientCapabilities{});
            mcp::Root root;
            root.uri = "file:///updated/path";
            root.name = "updated";
            client.set_roots({root}, true);
        },
        boost::asio::detached);

    io_ctx_.run();

    bool found_notification = false;
    for (const auto& msg : written_messages) {
        auto parsed = nlohmann::json::parse(msg);
        if (parsed.contains("method") && parsed["method"] == "notifications/roots/list_changed") {
            found_notification = true;
            EXPECT_EQ(parsed["jsonrpc"], "2.0");
            EXPECT_FALSE(parsed.contains("id"));
        }
    }
    EXPECT_TRUE(found_notification);
}

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

   private:
    boost::asio::strand<boost::asio::any_io_executor> strand_;
    boost::asio::steady_timer timer_;
    std::queue<std::string> incoming_;
    std::vector<std::string> written_;
    std::function<void(std::string_view)> on_write_;
    bool closed_ = false;
};

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

class ServerToolTest : public ::testing::Test {
   protected:
    boost::asio::io_context io_ctx_;

    struct ServerSetup {
        ScriptedTransport* raw_transport;
        std::unique_ptr<mcp::ITransport> transport;
        mcp::Server server;

        ServerSetup(boost::asio::io_context& io_ctx, mcp::ServerCapabilities caps)
            : raw_transport(new ScriptedTransport(io_ctx.get_executor())),
              transport(raw_transport),
              server(
                  [] {
                      mcp::Implementation info;
                      info.name = "test-server";
                      info.version = "1.0";
                      return info;
                  }(),
                  std::move(caps)) {}
    };
};

TEST_F(ServerToolTest, NonTemplateSyncHandlerReturnsResult) {
    mcp::ServerCapabilities caps;
    caps.tools = mcp::ServerCapabilities::ToolsCapability{};
    ServerSetup setup(io_ctx_, std::move(caps));

    setup.server.add_tool(
        "greet", "Greets a user",
        nlohmann::json{{"type", "object"}, {"properties", {{"name", {{"type", "string"}}}}}},
        [](const nlohmann::json& params) -> nlohmann::json {
            return {{"message", "Hello, " + params.at("name").get<std::string>() + "!"}};
        });

    std::vector<nlohmann::json> responses;
    setup.raw_transport->set_on_write([&responses, &setup](std::string_view msg) {
        responses.push_back(nlohmann::json::parse(msg));
        if (responses.size() == 2) {
            setup.raw_transport->close();
        }
    });

    setup.raw_transport->enqueue_message(make_initialize_request("1").dump());

    nlohmann::json call_req;
    call_req["jsonrpc"] = "2.0";
    call_req["id"] = "2";
    call_req["method"] = "tools/call";
    call_req["params"] = {{"name", "greet"}, {"arguments", {{"name", "Alice"}}}};
    setup.raw_transport->enqueue_message(call_req.dump());

    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            co_await setup.server.run(std::move(setup.transport), io_ctx_.get_executor());
        },
        boost::asio::detached);

    io_ctx_.run();

    ASSERT_EQ(responses.size(), 2);
    EXPECT_EQ(responses[1]["id"], "2");
    ASSERT_TRUE(responses[1].contains("result"));
    EXPECT_EQ(responses[1]["result"]["message"], "Hello, Alice!");
}

TEST_F(ServerToolTest, NonTemplateHandlerExceptionBecomesJsonRpcError) {
    mcp::ServerCapabilities caps;
    caps.tools = mcp::ServerCapabilities::ToolsCapability{};
    ServerSetup setup(io_ctx_, std::move(caps));

    setup.server.add_tool(
        "fail", "Always fails", nlohmann::json{{"type", "object"}},
        [](const nlohmann::json&) -> nlohmann::json { throw std::runtime_error("something broke"); });

    std::vector<nlohmann::json> responses;
    setup.raw_transport->set_on_write([&responses, &setup](std::string_view msg) {
        responses.push_back(nlohmann::json::parse(msg));
        if (responses.size() == 2) {
            setup.raw_transport->close();
        }
    });

    setup.raw_transport->enqueue_message(make_initialize_request("1").dump());

    nlohmann::json call_req;
    call_req["jsonrpc"] = "2.0";
    call_req["id"] = "2";
    call_req["method"] = "tools/call";
    call_req["params"] = {{"name", "fail"}, {"arguments", nlohmann::json::object()}};
    setup.raw_transport->enqueue_message(call_req.dump());

    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            co_await setup.server.run(std::move(setup.transport), io_ctx_.get_executor());
        },
        boost::asio::detached);

    io_ctx_.run();

    ASSERT_EQ(responses.size(), 2);
    EXPECT_EQ(responses[1]["id"], "2");
    ASSERT_TRUE(responses[1].contains("result"));
    EXPECT_TRUE(responses[1]["result"]["isError"].get<bool>());
    auto content_arr = responses[1]["result"]["content"];
    ASSERT_FALSE(content_arr.empty());
    auto err_text = content_arr[0]["text"].get<std::string>();
    EXPECT_NE(err_text.find("something broke"), std::string::npos);
}

TEST_F(ServerToolTest, NonTemplateToolAppearsInToolsList) {
    mcp::ServerCapabilities caps;
    caps.tools = mcp::ServerCapabilities::ToolsCapability{};
    ServerSetup setup(io_ctx_, std::move(caps));

    nlohmann::json schema = {{"type", "object"}, {"properties", {{"x", {{"type", "integer"}}}}}};
    setup.server.add_tool("echo", "Echoes input", schema,
                          [](const nlohmann::json& params) -> nlohmann::json { return params; });

    std::vector<nlohmann::json> responses;
    setup.raw_transport->set_on_write([&responses, &setup](std::string_view msg) {
        responses.push_back(nlohmann::json::parse(msg));
        if (responses.size() == 2) {
            setup.raw_transport->close();
        }
    });

    setup.raw_transport->enqueue_message(make_initialize_request("1").dump());

    nlohmann::json list_req;
    list_req["jsonrpc"] = "2.0";
    list_req["id"] = "2";
    list_req["method"] = "tools/list";
    list_req["params"] = nlohmann::json::object();
    setup.raw_transport->enqueue_message(list_req.dump());

    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            co_await setup.server.run(std::move(setup.transport), io_ctx_.get_executor());
        },
        boost::asio::detached);

    io_ctx_.run();

    ASSERT_EQ(responses.size(), 2);
    auto tools_arr = responses[1]["result"]["tools"];
    ASSERT_EQ(tools_arr.size(), 1);
    EXPECT_EQ(tools_arr[0]["name"], "echo");
    EXPECT_EQ(tools_arr[0]["description"], "Echoes input");
    EXPECT_EQ(tools_arr[0]["inputSchema"], schema);
}

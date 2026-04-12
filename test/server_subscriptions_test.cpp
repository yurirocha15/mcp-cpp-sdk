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

class ServerSubscriptionsTest : public ::testing::Test {
   protected:
    boost::asio::io_context io_ctx_;
};

TEST_F(ServerSubscriptionsTest, SubscribeReturnsEmptyResult) {
    auto* raw_transport = new ScriptedTransport(io_ctx_.get_executor());
    auto transport = std::unique_ptr<mcp::ITransport>(raw_transport);

    mcp::Implementation info;
    info.name = "test-server";
    info.version = "1.0";

    mcp::ServerCapabilities caps;
    mcp::ServerCapabilities::ResourcesCapability res_cap;
    res_cap.subscribe = true;
    caps.resources = std::move(res_cap);

    mcp::Server server(std::move(info), std::move(caps));

    std::vector<nlohmann::json> responses;
    raw_transport->set_on_write([&responses, raw_transport](std::string_view msg) {
        responses.push_back(nlohmann::json::parse(msg));
        if (responses.size() == 2) {
            raw_transport->close();
        }
    });

    raw_transport->enqueue_message(make_initialize_request("1").dump());

    nlohmann::json sub_req;
    sub_req["jsonrpc"] = "2.0";
    sub_req["id"] = "2";
    sub_req["method"] = "resources/subscribe";
    sub_req["params"] = nlohmann::json{{"uri", "file:///test.txt"}};
    raw_transport->enqueue_message(sub_req.dump());

    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> { co_await server.run(std::move(transport), io_ctx_.get_executor()); },
        boost::asio::detached);

    io_ctx_.run();

    ASSERT_EQ(responses.size(), 2);
    EXPECT_EQ(responses[1]["id"], "2");
    ASSERT_TRUE(responses[1].contains("result"));
    EXPECT_EQ(responses[1]["result"], nlohmann::json::object());
}

TEST_F(ServerSubscriptionsTest, UnsubscribeReturnsEmptyResult) {
    auto* raw_transport = new ScriptedTransport(io_ctx_.get_executor());
    auto transport = std::unique_ptr<mcp::ITransport>(raw_transport);

    mcp::Implementation info;
    info.name = "test-server";
    info.version = "1.0";

    mcp::ServerCapabilities caps;
    mcp::ServerCapabilities::ResourcesCapability res_cap;
    res_cap.subscribe = true;
    caps.resources = std::move(res_cap);

    mcp::Server server(std::move(info), std::move(caps));

    std::vector<nlohmann::json> responses;
    raw_transport->set_on_write([&responses, raw_transport](std::string_view msg) {
        responses.push_back(nlohmann::json::parse(msg));
        // init + subscribe + unsubscribe = 3
        if (responses.size() == 3) {
            raw_transport->close();
        }
    });

    raw_transport->enqueue_message(make_initialize_request("1").dump());

    nlohmann::json sub_req;
    sub_req["jsonrpc"] = "2.0";
    sub_req["id"] = "2";
    sub_req["method"] = "resources/subscribe";
    sub_req["params"] = nlohmann::json{{"uri", "file:///test.txt"}};
    raw_transport->enqueue_message(sub_req.dump());

    nlohmann::json unsub_req;
    unsub_req["jsonrpc"] = "2.0";
    unsub_req["id"] = "3";
    unsub_req["method"] = "resources/unsubscribe";
    unsub_req["params"] = nlohmann::json{{"uri", "file:///test.txt"}};
    raw_transport->enqueue_message(unsub_req.dump());

    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> { co_await server.run(std::move(transport), io_ctx_.get_executor()); },
        boost::asio::detached);

    io_ctx_.run();

    ASSERT_EQ(responses.size(), 3);
    EXPECT_EQ(responses[1]["id"], "2");
    EXPECT_EQ(responses[1]["result"], nlohmann::json::object());
    EXPECT_EQ(responses[2]["id"], "3");
    EXPECT_EQ(responses[2]["result"], nlohmann::json::object());
}

TEST_F(ServerSubscriptionsTest, NotifyResourceUpdatedSendsToSubscribers) {
    auto* raw_transport = new ScriptedTransport(io_ctx_.get_executor());
    auto transport = std::unique_ptr<mcp::ITransport>(raw_transport);

    mcp::Implementation info;
    info.name = "test-server";
    info.version = "1.0";

    mcp::ServerCapabilities caps;
    mcp::ServerCapabilities::ResourcesCapability res_cap;
    res_cap.subscribe = true;
    caps.resources = std::move(res_cap);
    mcp::ServerCapabilities::ToolsCapability tools_cap;
    caps.tools = std::move(tools_cap);

    mcp::Server server(std::move(info), std::move(caps));

    // Register a tool that triggers resource update notification
    struct TriggerParams {
        std::string uri;
    };
    struct TriggerResult {
        bool triggered = true;
    };

    server.add_tool<nlohmann::json, nlohmann::json>(
        "trigger_update", "Triggers a resource update", nlohmann::json{{"type", "object"}},
        [&server](mcp::Context& /*ctx*/, nlohmann::json params) -> mcp::Task<nlohmann::json> {
            auto uri = params.at("uri").get<std::string>();
            co_await server.notify_resource_updated(uri);
            co_return nlohmann::json{{"triggered", true}};
        });

    std::vector<nlohmann::json> responses;
    raw_transport->set_on_write([&responses, raw_transport](std::string_view msg) {
        responses.push_back(nlohmann::json::parse(msg));
        // init + subscribe result + resource updated notification + tool result = 4
        if (responses.size() == 4) {
            raw_transport->close();
        }
    });

    raw_transport->enqueue_message(make_initialize_request("1").dump());

    // Subscribe to a resource
    nlohmann::json sub_req;
    sub_req["jsonrpc"] = "2.0";
    sub_req["id"] = "2";
    sub_req["method"] = "resources/subscribe";
    sub_req["params"] = nlohmann::json{{"uri", "file:///watched.txt"}};
    raw_transport->enqueue_message(sub_req.dump());

    // Call tool that triggers notification
    nlohmann::json call_req;
    call_req["jsonrpc"] = "2.0";
    call_req["id"] = "3";
    call_req["method"] = "tools/call";
    call_req["params"] =
        nlohmann::json{{"name", "trigger_update"}, {"arguments", {{"uri", "file:///watched.txt"}}}};
    raw_transport->enqueue_message(call_req.dump());

    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> { co_await server.run(std::move(transport), io_ctx_.get_executor()); },
        boost::asio::detached);

    io_ctx_.run();

    ASSERT_EQ(responses.size(), 4);

    // Subscribe result
    EXPECT_EQ(responses[1]["id"], "2");
    EXPECT_EQ(responses[1]["result"], nlohmann::json::object());

    // Resource updated notification
    auto& notif = responses[2];
    EXPECT_EQ(notif["method"], "notifications/resources/updated");
    EXPECT_FALSE(notif.contains("id"));
    EXPECT_EQ(notif["params"]["uri"], "file:///watched.txt");

    // Tool result
    EXPECT_EQ(responses[3]["id"], "3");
    ASSERT_TRUE(responses[3].contains("result"));
}

TEST_F(ServerSubscriptionsTest, NotifyResourceUpdatedNotSentForUnsubscribedUri) {
    auto* raw_transport = new ScriptedTransport(io_ctx_.get_executor());
    auto transport = std::unique_ptr<mcp::ITransport>(raw_transport);

    mcp::Implementation info;
    info.name = "test-server";
    info.version = "1.0";

    mcp::ServerCapabilities caps;
    mcp::ServerCapabilities::ToolsCapability tools_cap;
    caps.tools = std::move(tools_cap);

    mcp::Server server(std::move(info), std::move(caps));

    server.add_tool<nlohmann::json, nlohmann::json>(
        "trigger_update", "Triggers update for unsubscribed", nlohmann::json{{"type", "object"}},
        [&server](mcp::Context& /*ctx*/, nlohmann::json params) -> mcp::Task<nlohmann::json> {
            auto uri = params.at("uri").get<std::string>();
            co_await server.notify_resource_updated(uri);
            co_return nlohmann::json{{"triggered", true}};
        });

    std::vector<nlohmann::json> responses;
    raw_transport->set_on_write([&responses, raw_transport](std::string_view msg) {
        responses.push_back(nlohmann::json::parse(msg));
        // init + tool result = 2 (no notification because not subscribed)
        if (responses.size() == 2) {
            raw_transport->close();
        }
    });

    raw_transport->enqueue_message(make_initialize_request("1").dump());

    nlohmann::json call_req;
    call_req["jsonrpc"] = "2.0";
    call_req["id"] = "2";
    call_req["method"] = "tools/call";
    call_req["params"] = nlohmann::json{{"name", "trigger_update"},
                                        {"arguments", {{"uri", "file:///not-subscribed.txt"}}}};
    raw_transport->enqueue_message(call_req.dump());

    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> { co_await server.run(std::move(transport), io_ctx_.get_executor()); },
        boost::asio::detached);

    io_ctx_.run();

    ASSERT_EQ(responses.size(), 2);
    EXPECT_EQ(responses[0]["id"], "1");
    EXPECT_EQ(responses[1]["id"], "2");
    // No notification was sent
}

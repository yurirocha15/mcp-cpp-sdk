#include "mcp/client.hpp"

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
        std::string msg(message);
        co_await boost::asio::post(strand_, boost::asio::use_awaitable);
        written_.emplace_back(msg);
        if (on_write_) {
            auto cb = on_write_;
            cb(written_.back());
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
    [[nodiscard]] const boost::asio::strand<boost::asio::any_io_executor>& strand() const {
        return strand_;
    }

   private:
    boost::asio::strand<boost::asio::any_io_executor> strand_;
    boost::asio::steady_timer timer_;
    std::queue<std::string> incoming_;
    std::vector<std::string> written_;
    std::function<void(std::string_view)> on_write_;
    bool closed_ = false;
};

nlohmann::json make_initialize_result() {
    return {{"protocolVersion", mcp::LATEST_PROTOCOL_VERSION},
            {"capabilities", nlohmann::json::object()},
            {"serverInfo", {{"name", "test-server"}, {"version", "1.0"}}}};
}

}  // namespace

class ClientNotificationsTest : public ::testing::Test {
   protected:
    boost::asio::io_context io_ctx_;
};

TEST_F(ClientNotificationsTest, NotificationCallbackFires) {
    auto* raw = new ScriptedTransport(io_ctx_.get_executor());
    auto transport = std::unique_ptr<mcp::ITransport>(raw);

    mcp::Client client(std::move(transport), io_ctx_.get_executor());

    bool callback_fired = false;
    nlohmann::json received_params;
    client.on_notification("notifications/progress", [&](const nlohmann::json& params) {
        callback_fired = true;
        received_params = params;
    });

    int write_count = 0;
    raw->set_on_write([&](std::string_view msg) {
        ++write_count;
        auto json_msg = nlohmann::json::parse(msg);

        if (write_count == 1) {
            auto id = json_msg["id"].get<std::string>();
            raw->enqueue_message(
                nlohmann::json{{"jsonrpc", "2.0"}, {"id", id}, {"result", make_initialize_result()}}
                    .dump());
        } else if (write_count == 2) {
            nlohmann::json progress_notif;
            progress_notif["jsonrpc"] = "2.0";
            progress_notif["method"] = "notifications/progress";
            progress_notif["params"] = {{"progressToken", "tok-1"}, {"progress", 0.5}};
            raw->enqueue_message(progress_notif.dump());
            raw->close();
        }
    });

    boost::asio::co_spawn(
        io_ctx_, [&]() -> mcp::Task<void> { co_await client.connect({"test-client", "1.0"}, {}); },
        boost::asio::detached);

    io_ctx_.run();

    EXPECT_TRUE(callback_fired);
    EXPECT_EQ(received_params["progressToken"], "tok-1");
    EXPECT_DOUBLE_EQ(received_params["progress"].get<double>(), 0.5);
}

TEST_F(ClientNotificationsTest, UnhandledNotificationSilentlyDropped) {
    auto* raw = new ScriptedTransport(io_ctx_.get_executor());
    auto transport = std::unique_ptr<mcp::ITransport>(raw);

    mcp::Client client(std::move(transport), io_ctx_.get_executor());

    int write_count = 0;
    raw->set_on_write([&](std::string_view msg) {
        ++write_count;
        auto json_msg = nlohmann::json::parse(msg);

        if (write_count == 1) {
            auto id = json_msg["id"].get<std::string>();
            raw->enqueue_message(
                nlohmann::json{{"jsonrpc", "2.0"}, {"id", id}, {"result", make_initialize_result()}}
                    .dump());
        } else if (write_count == 2) {
            nlohmann::json notif;
            notif["jsonrpc"] = "2.0";
            notif["method"] = "notifications/unknown";
            notif["params"] = {{"data", "test"}};
            raw->enqueue_message(notif.dump());
            raw->close();
        }
    });

    bool completed = false;
    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            co_await client.connect({"test-client", "1.0"}, {});
            completed = true;
        },
        boost::asio::detached);

    io_ctx_.run();
    EXPECT_TRUE(completed);
}

TEST_F(ClientNotificationsTest, PingRequestHandledAutomatically) {
    auto* raw = new ScriptedTransport(io_ctx_.get_executor());
    auto transport = std::unique_ptr<mcp::ITransport>(raw);

    mcp::Client client(std::move(transport), io_ctx_.get_executor());

    std::vector<std::string> written_messages;
    int write_count = 0;
    raw->set_on_write([&](std::string_view msg) {
        ++write_count;
        written_messages.emplace_back(msg);
        auto json_msg = nlohmann::json::parse(msg);

        if (write_count == 1) {
            auto id = json_msg["id"].get<std::string>();
            raw->enqueue_message(
                nlohmann::json{{"jsonrpc", "2.0"}, {"id", id}, {"result", make_initialize_result()}}
                    .dump());
        } else if (write_count == 2) {
            nlohmann::json ping;
            ping["jsonrpc"] = "2.0";
            ping["id"] = "server-ping-1";
            ping["method"] = "ping";
            raw->enqueue_message(ping.dump());
        } else if (write_count == 3) {
            raw->close();
        }
    });

    boost::asio::co_spawn(
        io_ctx_, [&]() -> mcp::Task<void> { co_await client.connect({"test-client", "1.0"}, {}); },
        boost::asio::detached);

    io_ctx_.run();

    ASSERT_GE(written_messages.size(), 3u);
    auto ping_response = nlohmann::json::parse(written_messages[2]);
    EXPECT_EQ(ping_response["id"], "server-ping-1");
    EXPECT_TRUE(ping_response.contains("result"));
    EXPECT_EQ(ping_response["result"], nlohmann::json::object());
}

TEST_F(ClientNotificationsTest, CustomRequestHandlerDispatch) {
    auto* raw = new ScriptedTransport(io_ctx_.get_executor());
    auto transport = std::unique_ptr<mcp::ITransport>(raw);

    mcp::Client client(std::move(transport), io_ctx_.get_executor());

    bool handler_called = false;
    nlohmann::json handler_params;
    client.on_request("roots/list", [&](const nlohmann::json& params) -> mcp::Task<nlohmann::json> {
        handler_called = true;
        handler_params = params;
        mcp::ListRootsResult result;
        mcp::Root r;
        r.uri = "file:///workspace";
        r.name = "workspace";
        result.roots.push_back(std::move(r));
        nlohmann::json j = std::move(result);
        co_return j;
    });

    std::vector<std::string> written_messages;
    int write_count = 0;
    raw->set_on_write([&](std::string_view msg) {
        ++write_count;
        written_messages.emplace_back(msg);
        auto json_msg = nlohmann::json::parse(msg);

        if (write_count == 1) {
            auto id = json_msg["id"].get<std::string>();
            raw->enqueue_message(
                nlohmann::json{{"jsonrpc", "2.0"}, {"id", id}, {"result", make_initialize_result()}}
                    .dump());
        } else if (write_count == 2) {
            nlohmann::json roots_req;
            roots_req["jsonrpc"] = "2.0";
            roots_req["id"] = "server-req-1";
            roots_req["method"] = "roots/list";
            roots_req["params"] = nlohmann::json::object();
            raw->enqueue_message(roots_req.dump());
        } else if (write_count == 3) {
            raw->close();
        }
    });

    boost::asio::co_spawn(
        io_ctx_, [&]() -> mcp::Task<void> { co_await client.connect({"test-client", "1.0"}, {}); },
        boost::asio::detached);

    io_ctx_.run();

    EXPECT_TRUE(handler_called);
    ASSERT_GE(written_messages.size(), 3u);
    auto response = nlohmann::json::parse(written_messages[2]);
    EXPECT_EQ(response["id"], "server-req-1");
    ASSERT_TRUE(response.contains("result"));
    ASSERT_TRUE(response["result"].contains("roots"));
    EXPECT_EQ(response["result"]["roots"][0]["uri"], "file:///workspace");
}

TEST_F(ClientNotificationsTest, UnknownRequestReturnsMethodNotFound) {
    auto* raw = new ScriptedTransport(io_ctx_.get_executor());
    auto transport = std::unique_ptr<mcp::ITransport>(raw);

    mcp::Client client(std::move(transport), io_ctx_.get_executor());

    std::vector<std::string> written_messages;
    int write_count = 0;
    raw->set_on_write([&](std::string_view msg) {
        ++write_count;
        written_messages.emplace_back(msg);
        auto json_msg = nlohmann::json::parse(msg);

        if (write_count == 1) {
            auto id = json_msg["id"].get<std::string>();
            raw->enqueue_message(
                nlohmann::json{{"jsonrpc", "2.0"}, {"id", id}, {"result", make_initialize_result()}}
                    .dump());
        } else if (write_count == 2) {
            nlohmann::json unknown_req;
            unknown_req["jsonrpc"] = "2.0";
            unknown_req["id"] = "server-req-unknown";
            unknown_req["method"] = "nonexistent/method";
            raw->enqueue_message(unknown_req.dump());
        } else if (write_count == 3) {
            raw->close();
        }
    });

    boost::asio::co_spawn(
        io_ctx_, [&]() -> mcp::Task<void> { co_await client.connect({"test-client", "1.0"}, {}); },
        boost::asio::detached);

    io_ctx_.run();

    ASSERT_GE(written_messages.size(), 3u);
    auto response = nlohmann::json::parse(written_messages[2]);
    EXPECT_EQ(response["id"], "server-req-unknown");
    ASSERT_TRUE(response.contains("error"));
    EXPECT_EQ(response["error"]["code"], -32601);
}

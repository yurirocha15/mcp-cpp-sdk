#include "../test_utils.hpp"

#include "mcp/client/client.hpp"

#include <gtest/gtest.h>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

// Returns just the result object (not a full JSON-RPC response) —
// differs from the shared helper in test_utils.hpp which returns a
// complete response envelope.  Callers here wrap it inline.
nlohmann::json make_initialize_result() {
    return {{"protocolVersion", mcp::g_LATEST_PROTOCOL_VERSION},
            {"capabilities", nlohmann::json::object()},
            {"serverInfo", {{"name", "test-server"}, {"version", "1.0"}}}};
}

}  // namespace

class ClientNotificationsTest : public ::testing::Test {
   protected:
    boost::asio::io_context io_ctx_;
};

TEST_F(ClientNotificationsTest, NotificationCallbackFires) {
    auto transport = std::make_shared<ScriptedTransport>(io_ctx_.get_executor());
    auto* raw = transport.get();

    mcp::Client client(transport, io_ctx_.get_executor());

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

            boost::asio::post(io_ctx_,
                              [raw]() { boost::asio::post(raw->strand(), [raw]() { raw->close(); }); });
        }
    });

    boost::asio::co_spawn(
        io_ctx_, [&]() -> mcp::Task<void> { co_await client.connect("test-client", "1.0"); },
        boost::asio::detached);

    io_ctx_.run();

    EXPECT_TRUE(callback_fired);
    EXPECT_EQ(received_params["progressToken"], "tok-1");
    EXPECT_DOUBLE_EQ(received_params["progress"].get<double>(), 0.5);
}

TEST_F(ClientNotificationsTest, UnhandledNotificationSilentlyDropped) {
    auto transport = std::make_shared<ScriptedTransport>(io_ctx_.get_executor());
    auto* raw = transport.get();

    mcp::Client client(transport, io_ctx_.get_executor());

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

            boost::asio::post(io_ctx_,
                              [raw]() { boost::asio::post(raw->strand(), [raw]() { raw->close(); }); });
        }
    });

    bool completed = false;
    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            co_await client.connect("test-client", "1.0");
            completed = true;
        },
        boost::asio::detached);

    io_ctx_.run();
    EXPECT_TRUE(completed);
}

TEST_F(ClientNotificationsTest, PingRequestHandledAutomatically) {
    auto transport = std::make_shared<ScriptedTransport>(io_ctx_.get_executor());
    auto* raw = transport.get();

    mcp::Client client(transport, io_ctx_.get_executor());

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
        io_ctx_, [&]() -> mcp::Task<void> { co_await client.connect("test-client", "1.0"); },
        boost::asio::detached);

    io_ctx_.run();

    ASSERT_GE(written_messages.size(), 3u);
    auto ping_response = nlohmann::json::parse(written_messages[2]);
    EXPECT_EQ(ping_response["id"], "server-ping-1");
    EXPECT_TRUE(ping_response.contains("result"));
    EXPECT_EQ(ping_response["result"], nlohmann::json::object());
}

TEST_F(ClientNotificationsTest, CustomRequestHandlerDispatch) {
    auto transport = std::make_shared<ScriptedTransport>(io_ctx_.get_executor());
    auto* raw = transport.get();

    mcp::Client client(transport, io_ctx_.get_executor());

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
        io_ctx_, [&]() -> mcp::Task<void> { co_await client.connect("test-client", "1.0"); },
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
    auto transport = std::make_shared<ScriptedTransport>(io_ctx_.get_executor());
    auto* raw = transport.get();

    mcp::Client client(transport, io_ctx_.get_executor());

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
        io_ctx_, [&]() -> mcp::Task<void> { co_await client.connect("test-client", "1.0"); },
        boost::asio::detached);

    io_ctx_.run();

    ASSERT_GE(written_messages.size(), 3u);
    auto response = nlohmann::json::parse(written_messages[2]);
    EXPECT_EQ(response["id"], "server-req-unknown");
    ASSERT_TRUE(response.contains("error"));
    EXPECT_EQ(response["error"]["code"], mcp::g_METHOD_NOT_FOUND);
}

namespace {

struct NotificationSessionOutcome {
    bool survived{false};  ///< A request issued after the notifications still completed.
    std::string failure;
    std::vector<mcp::Error> reported;  ///< What ClientOptions::on_protocol_error saw.
};

// Delivers `notifications` to a connected client, then issues one request over the same session.
// That request is the probe: it completes only if the notifications left the session intact. A
// notification the client mishandles by ending the read loop fails it instead.
NotificationSessionOutcome deliver_notifications(
    boost::asio::io_context& io_ctx, const std::vector<nlohmann::json>& notifications,
    const std::function<void(mcp::Client&)>& register_handlers) {
    auto transport = std::make_shared<ScriptedTransport>(io_ctx.get_executor());
    auto* raw = transport.get();

    NotificationSessionOutcome outcome;

    mcp::ClientOptions options;
    // Short enough that a swallowed response surfaces as a test failure instead of a stalled run.
    options.request_timeout = std::chrono::seconds(5);
    options.on_protocol_error = [&](const mcp::Error& error) { outcome.reported.push_back(error); };
    mcp::Client client(transport, io_ctx.get_executor(), options);
    register_handlers(client);

    bool delivered = false;
    raw->set_on_write([&](std::string_view msg) {
        const auto json_msg = nlohmann::json::parse(msg);
        if (!json_msg.contains("id")) {
            return;
        }
        const auto id = json_msg.at("id").get<std::string>();
        if (json_msg.value("method", "") == "initialize") {
            raw->enqueue_message(
                nlohmann::json{{"jsonrpc", "2.0"}, {"id", id}, {"result", make_initialize_result()}}
                    .dump());
            return;
        }
        if (!delivered) {
            delivered = true;
            for (const auto& notification : notifications) {
                raw->enqueue_message(notification.dump());
            }
        }
        raw->enqueue_message(
            nlohmann::json{{"jsonrpc", "2.0"}, {"id", id}, {"result", nlohmann::json::object()}}
                .dump());
    });

    boost::asio::co_spawn(
        io_ctx,
        [&]() -> mcp::Task<void> {
            static_cast<void>(co_await client.connect("test-client", "1.0"));
            try {
                co_await client.ping();
                outcome.survived = true;
            } catch (const mcp::McpError& error) {
                outcome.failure = error.what();
            }
            raw->close();
        },
        boost::asio::detached);

    io_ctx.run();
    return outcome;
}

}  // namespace

// A cancelled notification names a request id, and the spec constrains that to a string or an
// integer. A peer that sends anything else has said nothing the client can act on -- and nothing
// that should cost the application its connection.
TEST_F(ClientNotificationsTest, MalformedCancelledNotificationDoesNotStopReadLoop) {
    const auto outcome =
        deliver_notifications(io_ctx_,
                              {{{"jsonrpc", "2.0"},
                                {"method", "notifications/cancelled"},
                                {"params", {{"requestId", nlohmann::json::object()}}}}},
                              [](mcp::Client&) {});

    EXPECT_TRUE(outcome.survived) << "the session died on one malformed notification: "
                                  << outcome.failure;
    ASSERT_EQ(outcome.reported.size(), 1U) << "the drop was silent";
    EXPECT_EQ(outcome.reported.front().code, mcp::g_PARSE_ERROR);
}

// `total` and `message` are optional. A peer that serializes absent optionals as explicit null --
// the default for Go's encoding/json without omitempty -- still means "not provided", and the
// notification must arrive with those fields empty.
TEST_F(ClientNotificationsTest, ProgressNotificationWithNullOptionalFieldsIsDelivered) {
    std::optional<mcp::ProgressNotificationParams> seen;

    const auto outcome = deliver_notifications(
        io_ctx_,
        {{{"jsonrpc", "2.0"},
          {"method", "notifications/progress"},
          {"params",
           {{"progressToken", "tok-1"}, {"progress", 0.5}, {"total", nullptr}, {"message", nullptr}}}}},
        [&](mcp::Client& client) {
            client.on_progress([&](const mcp::ProgressNotificationParams& params) { seen = params; });
        });

    EXPECT_TRUE(outcome.survived) << "the session died on one progress notification: "
                                  << outcome.failure;
    ASSERT_TRUE(seen.has_value()) << "the progress callback never ran";
    EXPECT_DOUBLE_EQ(seen->progress, 0.5);
    EXPECT_FALSE(seen->total.has_value());
    EXPECT_FALSE(seen->message.has_value());
    EXPECT_TRUE(outcome.reported.empty()) << "a well-formed notification was reported as an error";
}

// Progress params that are genuinely undecodable are reported against their own cause, so an
// application can tell a peer that sent the notification wrong from a bug in its own callback.
TEST_F(ClientNotificationsTest, MalformedProgressNotificationIsReportedNotFatal) {
    bool callback_ran = false;

    const auto outcome = deliver_notifications(
        io_ctx_,
        {{{"jsonrpc", "2.0"},
          {"method", "notifications/progress"},
          {"params", {{"progressToken", "tok-1"}, {"progress", nullptr}}}}},
        [&](mcp::Client& client) {
            client.on_progress([&](const mcp::ProgressNotificationParams&) { callback_ran = true; });
        });

    EXPECT_TRUE(outcome.survived) << "the session died on one progress notification: "
                                  << outcome.failure;
    EXPECT_FALSE(callback_ran) << "an undecodable notification reached the application";
    ASSERT_EQ(outcome.reported.size(), 1U) << "the drop was silent";
    EXPECT_NE(outcome.reported.front().message.find("Malformed progress notification params"),
              std::string::npos)
        << "actual: " << outcome.reported.front().message;
}

// The notification callback is application code and it runs on the read loop. A bug in it is the
// application's to fix, not grounds for the SDK to tear down the connection -- but it must not
// vanish either, or the application cannot find the bug.
TEST_F(ClientNotificationsTest, ThrowingNotificationCallbackDoesNotStopReadLoop) {
    const auto outcome = deliver_notifications(
        io_ctx_,
        {{{"jsonrpc", "2.0"},
          {"method", "notifications/message"},
          {"params", nlohmann::json::object()}}},
        [](mcp::Client& client) {
            client.on_notification("notifications/message", [](const nlohmann::json&) {
                throw std::runtime_error("callback blew up");
            });
        });

    EXPECT_TRUE(outcome.survived) << "the session died on a throwing application callback: "
                                  << outcome.failure;
    ASSERT_EQ(outcome.reported.size(), 1U) << "the throw was silent";
    EXPECT_EQ(outcome.reported.front().code, mcp::g_INTERNAL_ERROR);
    EXPECT_NE(outcome.reported.front().message.find("notifications/message"), std::string::npos)
        << "actual: " << outcome.reported.front().message;
    EXPECT_NE(outcome.reported.front().message.find("callback blew up"), std::string::npos)
        << "actual: " << outcome.reported.front().message;
}

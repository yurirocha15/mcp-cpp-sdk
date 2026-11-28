#include "support/hang_guard.hpp"
#include "support/test_utils.hpp"

#include "mcp/client/client.hpp"
#include "mcp/core/async.hpp"
#include "mcp/detail/text/diagnostic_text.hpp"
#include "support/spawn.hpp"

#include <gtest/gtest.h>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <chrono>
#include <functional>
#include <map>
#include <mcp/core/constants.hpp>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace client_notifications_test {

namespace {

// Returns just the result object (not a full JSON-RPC response) —
// differs from the shared helper in test_utils.hpp which returns a
// complete response envelope.  Callers here wrap it inline.
nlohmann::json make_initialize_result() {
    return {{"protocolVersion", mcp::constants::protocol::g_LATEST_LEGACY_PROTOCOL_VERSION},
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

    mcp::Client client(transport, io_ctx_.get_executor(), legacy_client_options());

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

    mcp_test::spawn_detached(
        io_ctx_, [&]() -> mcp::Task<void> { co_await client.connect("test-client", "1.0"); });

    io_ctx_.run();

    EXPECT_TRUE(callback_fired);
    EXPECT_EQ(received_params["progressToken"], "tok-1");
    EXPECT_DOUBLE_EQ(received_params["progress"].get<double>(), 0.5);
}

TEST_F(ClientNotificationsTest, UnhandledNotificationSilentlyDropped) {
    auto transport = std::make_shared<ScriptedTransport>(io_ctx_.get_executor());
    auto* raw = transport.get();

    mcp::Client client(transport, io_ctx_.get_executor(), legacy_client_options());

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
    mcp_test::spawn_detached(io_ctx_, [&]() -> mcp::Task<void> {
        co_await client.connect("test-client", "1.0");
        completed = true;
    });

    io_ctx_.run();
    EXPECT_TRUE(completed);
}

TEST_F(ClientNotificationsTest, PingRequestHandledAutomatically) {
    auto transport = std::make_shared<ScriptedTransport>(io_ctx_.get_executor());
    auto* raw = transport.get();

    mcp::Client client(transport, io_ctx_.get_executor(), legacy_client_options());

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

    mcp_test::spawn_detached(
        io_ctx_, [&]() -> mcp::Task<void> { co_await client.connect("test-client", "1.0"); });

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

    mcp::Client client(transport, io_ctx_.get_executor(), legacy_client_options());

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

    mcp_test::spawn_detached(
        io_ctx_, [&]() -> mcp::Task<void> { co_await client.connect("test-client", "1.0"); });

    io_ctx_.run();

    EXPECT_TRUE(handler_called);
    ASSERT_GE(written_messages.size(), 3u);
    auto response = nlohmann::json::parse(written_messages[2]);
    EXPECT_EQ(response["id"], "server-req-1");
    ASSERT_TRUE(response.contains("result"));
    ASSERT_TRUE(response["result"].contains("roots"));
    EXPECT_EQ(response["result"]["roots"][0]["uri"], "file:///workspace");
}

// A handler whose result, or whose exception's text, is not UTF-8 still has the server answered,
// with an error, instead of leaving the server's request to wait out its timeout.
TEST_F(ClientNotificationsTest, AnswerThatCannotBeEncodedIsAnError) {
    auto transport = std::make_shared<ScriptedTransport>(io_ctx_.get_executor());
    auto* raw = transport.get();

    mcp::Client client(transport, io_ctx_.get_executor(), legacy_client_options());
    const std::string not_utf8 = "\xff\xfe";
    client.on_request("test/result", [&](const nlohmann::json&) -> mcp::Task<nlohmann::json> {
        co_return nlohmann::json{{"path", not_utf8}};
    });
    client.on_request("test/throw", [&](const nlohmann::json&) -> mcp::Task<nlohmann::json> {
        throw std::runtime_error("cannot open " + not_utf8);
        co_return nlohmann::json::object();
    });

    // Bounds a missing answer, which would otherwise leave the exchange open.
    boost::asio::steady_timer guard(io_ctx_, g_hang_guard);
    guard.async_wait([raw](const boost::system::error_code& error) {
        if (!error) {
            raw->close();
        }
    });
    std::vector<nlohmann::json> written;
    raw->set_on_write([&](std::string_view msg) {
        written.push_back(nlohmann::json::parse(msg));
        if (written.size() == 1) {
            raw->enqueue_message(nlohmann::json{
                {"jsonrpc", "2.0"}, {"id", written[0]["id"]}, {"result", make_initialize_result()}}
                                     .dump());
        } else if (written.size() == 2) {
            raw->enqueue_message(
                nlohmann::json{{"jsonrpc", "2.0"}, {"id", "r"}, {"method", "test/result"}}.dump());
            raw->enqueue_message(
                nlohmann::json{{"jsonrpc", "2.0"}, {"id", "t"}, {"method", "test/throw"}}.dump());
        } else if (written.size() == 4) {
            guard.cancel();
            raw->close();
        }
    });

    mcp_test::spawn_detached(
        io_ctx_, [&]() -> mcp::Task<void> { co_await client.connect("test-client", "1.0"); });
    io_ctx_.run();

    std::map<std::string, nlohmann::json> answers;
    for (const auto& message : written) {
        if (message.contains("id") && message["id"].is_string()) {
            answers[message["id"].get<std::string>()] = message;
        }
    }
    for (const auto* id : {"r", "t"}) {
        ASSERT_TRUE(answers.count(id) == 1) << id;
        EXPECT_EQ(answers[id]["error"]["code"], mcp::constants::jsonrpc::g_INTERNAL_ERROR)
            << answers[id].dump();
        EXPECT_EQ(answers[id]["error"]["message"], "The answer could not be encoded as JSON");
    }
}

// The handlers registered before connect() decide what a legacy initialize declares, as they do
// for every request of a modern session.
TEST_F(ClientNotificationsTest, InitializeDeclaresWhatTheRegisteredHandlersAnswer) {
    auto transport = std::make_shared<ScriptedTransport>(io_ctx_.get_executor());
    auto* raw = transport.get();
    mcp::Client client(transport, io_ctx_.get_executor(), legacy_client_options());
    client.on_sampling(
        [](const mcp::CreateMessageRequestParams&) -> mcp::Task<mcp::CreateMessageResult> {
            co_return mcp::CreateMessageResult{};
        });
    client.on_elicitation([](const mcp::ElicitRequestParams&) -> mcp::Task<mcp::ElicitResult> {
        co_return mcp::ElicitResult{};
    });
    client.on_roots_list([](const nlohmann::json&) -> mcp::Task<mcp::ListRootsResult> {
        co_return mcp::ListRootsResult{};
    });

    boost::asio::steady_timer guard(io_ctx_, g_hang_guard);
    guard.async_wait([raw](const boost::system::error_code& error) {
        if (!error) {
            raw->close();
        }
    });
    nlohmann::json initialize;
    raw->set_on_write([&](std::string_view msg) {
        auto message = nlohmann::json::parse(msg);
        if (message.value("method", "") == "initialize") {
            initialize = message;
            raw->enqueue_message(nlohmann::json{
                {"jsonrpc", "2.0"}, {"id", message["id"]}, {"result", make_initialize_result()}}
                                     .dump());
        } else if (message.value("method", "") == "notifications/initialized") {
            guard.cancel();
            raw->close();
        }
    });
    mcp_test::spawn_detached(
        io_ctx_, [&]() -> mcp::Task<void> { co_await client.connect("test-client", "1.0"); });
    io_ctx_.run();

    ASSERT_FALSE(initialize.is_null()) << "no initialize was sent";
    const auto& capabilities = initialize.at("params").at("capabilities");
    EXPECT_TRUE(capabilities.contains("sampling")) << capabilities.dump();
    EXPECT_TRUE(capabilities.contains("elicitation")) << capabilities.dump();
    EXPECT_TRUE(capabilities.contains("roots")) << capabilities.dump();
}

TEST_F(ClientNotificationsTest, SamplingHandlerAnswersTheServersRequest) {
    auto transport = std::make_shared<ScriptedTransport>(io_ctx_.get_executor());
    auto* raw = transport.get();
    mcp::Client client(transport, io_ctx_.get_executor(), legacy_client_options());
    std::optional<mcp::CreateMessageRequestParams> asked;
    client.on_sampling(
        [&](const mcp::CreateMessageRequestParams& params) -> mcp::Task<mcp::CreateMessageResult> {
            asked = params;
            mcp::CreateMessageResult result;
            result.role = mcp::Role::eAssistant;
            mcp::TextContent text;
            text.text = "four";
            result.content = std::move(text);
            result.model = "test-model";
            co_return result;
        });

    boost::asio::steady_timer guard(io_ctx_, g_hang_guard);
    guard.async_wait([raw](const boost::system::error_code& error) {
        if (!error) {
            raw->close();
        }
    });
    nlohmann::json answer;
    raw->set_on_write([&](std::string_view msg) {
        auto message = nlohmann::json::parse(msg);
        if (message.value("method", "") == "initialize") {
            raw->enqueue_message(nlohmann::json{
                {"jsonrpc", "2.0"}, {"id", message["id"]}, {"result", make_initialize_result()}}
                                     .dump());
        } else if (message.value("method", "") == "notifications/initialized") {
            raw->enqueue_message(nlohmann::json{
                {"jsonrpc", "2.0"},
                {"id", "sample-1"},
                {"method", "sampling/createMessage"},
                {"params",
                 {{"messages",
                   {{{"role", "user"}, {"content", {{"type", "text"}, {"text", "2 + 2?"}}}}}},
                  {"maxTokens",
                   10}}}}.dump());
        } else if (message.value("id", nlohmann::json()) == "sample-1") {
            answer = message;
            guard.cancel();
            raw->close();
        }
    });
    mcp_test::spawn_detached(
        io_ctx_, [&]() -> mcp::Task<void> { co_await client.connect("test-client", "1.0"); });
    io_ctx_.run();

    ASSERT_TRUE(asked.has_value()) << "the sampling handler never ran";
    EXPECT_EQ(asked->maxTokens, 10);
    ASSERT_EQ(asked->messages.size(), 1U);
    ASSERT_TRUE(answer.contains("result")) << answer.dump();
    EXPECT_EQ(answer["result"]["model"], "test-model");
    EXPECT_EQ(answer["result"]["role"], "assistant");
    EXPECT_EQ(answer["result"]["content"]["text"], "four");
}

TEST_F(ClientNotificationsTest, UnknownRequestReturnsMethodNotFound) {
    auto transport = std::make_shared<ScriptedTransport>(io_ctx_.get_executor());
    auto* raw = transport.get();

    mcp::Client client(transport, io_ctx_.get_executor(), legacy_client_options());

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

    mcp_test::spawn_detached(
        io_ctx_, [&]() -> mcp::Task<void> { co_await client.connect("test-client", "1.0"); });

    io_ctx_.run();

    ASSERT_GE(written_messages.size(), 3u);
    auto response = nlohmann::json::parse(written_messages[2]);
    EXPECT_EQ(response["id"], "server-req-unknown");
    ASSERT_TRUE(response.contains("error"));
    EXPECT_EQ(response["error"]["code"], mcp::constants::jsonrpc::g_METHOD_NOT_FOUND);
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
NotificationSessionOutcome deliver_raw_messages(
    boost::asio::io_context& io_ctx, const std::vector<std::string>& notifications,
    const std::function<void(mcp::Client&)>& register_handlers) {
    auto transport = std::make_shared<ScriptedTransport>(io_ctx.get_executor());
    auto* raw = transport.get();

    NotificationSessionOutcome outcome;

    mcp::ClientOptions options;
    // Bounded, so that a swallowed response surfaces as a test failure instead of a stalled run.
    options.request_timeout = g_hang_guard;
    options.on_protocol_error = [&](const mcp::Error& error) { outcome.reported.push_back(error); };
    options.protocol = mcp::ClientProtocol::legacy();  // the scripted peer answers initialize
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
                raw->enqueue_message(notification);
            }
        }
        raw->enqueue_message(
            nlohmann::json{{"jsonrpc", "2.0"}, {"id", id}, {"result", nlohmann::json::object()}}
                .dump());
    });

    mcp_test::spawn_detached(io_ctx, [&]() -> mcp::Task<void> {
        static_cast<void>(co_await client.connect("test-client", "1.0"));
        try {
            co_await client.ping();
            outcome.survived = true;
        } catch (const mcp::McpError& error) {
            outcome.failure = error.what();
        }
        raw->close();
    });

    io_ctx.run();
    return outcome;
}

NotificationSessionOutcome deliver_notifications(
    boost::asio::io_context& io_ctx, const std::vector<nlohmann::json>& notifications,
    const std::function<void(mcp::Client&)>& register_handlers) {
    std::vector<std::string> texts;
    texts.reserve(notifications.size());
    for (const auto& notification : notifications) {
        texts.push_back(notification.dump());
    }
    return deliver_raw_messages(io_ctx, texts, register_handlers);
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
    EXPECT_EQ(outcome.reported.front().code, mcp::constants::jsonrpc::g_PARSE_ERROR);
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

// Copying a parsed document recurses once per level, so a message made of nothing but brackets
// could exhaust the stack. One nested past the limit is dropped and reported like any other
// undecodable message; one at the limit is delivered.
TEST_F(ClientNotificationsTest, MessageNestedBeyondTheDepthLimitIsDroppedNotFatal) {
    const auto log_nested_to = [](std::size_t depth) {
        // The envelope and params are two of the levels.
        nlohmann::json data = nlohmann::json::array();
        for (std::size_t level = 3; level < depth; ++level) {
            data = nlohmann::json::array({std::move(data)});
        }
        return nlohmann::json{{"jsonrpc", "2.0"},
                              {"method", "notifications/message"},
                              {"params", {{"level", "info"}, {"data", std::move(data)}}}};
    };

    const auto outcome =
        deliver_notifications(io_ctx_, {log_nested_to(128), log_nested_to(129)}, [](mcp::Client&) {});

    EXPECT_TRUE(outcome.survived) << "the session died on a deeply nested message: " << outcome.failure;
    ASSERT_EQ(outcome.reported.size(), 1U) << "expected only the message over the limit to be dropped";
    EXPECT_EQ(outcome.reported.front().code, mcp::constants::jsonrpc::g_PARSE_ERROR);
    EXPECT_NE(outcome.reported.front().message.find("nests deeper than 128 levels"), std::string::npos)
        << "actual: " << outcome.reported.front().message;
}

// The text nlohmann::json reports for @p text, which the client passes on as its own report.
std::string nlohmann_parse_failure(const std::string& text) {
    try {
        const auto parsed = nlohmann::json::parse(text);
        static_cast<void>(parsed);
    } catch (const std::exception& error) {
        return error.what();
    }
    return {};
}

// Text that is not JSON is dropped and reported with the parse error nlohmann::json gives for it,
// and the session goes on.
TEST_F(ClientNotificationsTest, MalformedJsonIsReportedWithTheParseError) {
    const std::vector<std::string> malformed = {
        R"({"jsonrpc":"2.0","method":"notifications/custom")",
        R"({"jsonrpc":"2.0","method":"notifications/custom",})",
        R"({"jsonrpc":"2.0","method":"notifications/custom"} trailing)",
        R"({"jsonrpc":"2.0","method":"notifications/custom","params":{"a":01}})",
        R"({"jsonrpc":"2.0","method":"notifications/custom","params":{"a":"\x"}})",
        "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/custom\",\"params\":{\"a\":\"\xff\"}}",
        "",
        "   ",
        "nul",
    };
    const auto outcome = deliver_raw_messages(io_ctx_, malformed, [](mcp::Client&) {});

    EXPECT_TRUE(outcome.survived) << "the session died on malformed text: " << outcome.failure;
    ASSERT_EQ(outcome.reported.size(), malformed.size());
    for (std::size_t index = 0; index < malformed.size(); ++index) {
        EXPECT_EQ(outcome.reported[index].code, mcp::constants::jsonrpc::g_PARSE_ERROR);
        const auto failure = nlohmann_parse_failure(malformed[index]);
        ASSERT_FALSE(failure.empty()) << "case " << index << " parses";
        EXPECT_EQ(outcome.reported[index].message, "Dropped an undecodable message from the peer: " +
                                                       mcp::detail::sanitize_for_diagnostics(failure))
            << "case " << index;
    }
}

// A message nested past the limit is reported with the depth error, whatever else is wrong with
// its text, since the depth is checked before the text is parsed.
TEST_F(ClientNotificationsTest, OverDeepMessageIsReportedWithTheDepthError) {
    const std::string too_deep = std::string(129, '[') + std::string(129, ']');
    const std::string too_deep_and_truncated = std::string(129, '[');
    const auto outcome =
        deliver_raw_messages(io_ctx_, {too_deep, too_deep_and_truncated}, [](mcp::Client&) {});

    EXPECT_TRUE(outcome.survived) << "the session died on a deep message: " << outcome.failure;
    ASSERT_EQ(outcome.reported.size(), 2U);
    for (const auto& error : outcome.reported) {
        EXPECT_EQ(error.code, mcp::constants::jsonrpc::g_PARSE_ERROR);
        EXPECT_EQ(error.message,
                  "Dropped an undecodable message from the peer: JSON-RPC error -32700: JSON-RPC "
                  "message nests deeper than 128 levels");
    }
}

// Valid text is read as nlohmann::json reads it, including the inputs a fast parser leaves to it:
// a byte order mark, a repeated member (the last one counts), an integer past 64 bits, a
// surrogate pair and a negative zero.
TEST_F(ClientNotificationsTest, ValidTextIsDeliveredAsNlohmannReadsIt) {
    const std::string method_and_params_prefix =
        R"({"jsonrpc":"2.0","method":"notifications/custom","params":)";
    const std::vector<std::string> messages = {
        "\xEF\xBB\xBF" + method_and_params_prefix + R"({"n":1}})",
        method_and_params_prefix + R"({"n":1,"n":2}})",
        method_and_params_prefix + R"({"n":123456789012345678901234567890}})",
        method_and_params_prefix + R"({"n":"\ud83d\ude00"}})",
        method_and_params_prefix + R"({"n":-0.0}})",
        method_and_params_prefix + R"({"n":1e2}})",
    };
    std::vector<nlohmann::json> received;
    const auto outcome = deliver_raw_messages(io_ctx_, messages, [&](mcp::Client& client) {
        client.on_notification("notifications/custom",
                               [&](const nlohmann::json& params) { received.push_back(params); });
    });

    EXPECT_TRUE(outcome.survived) << outcome.failure;
    EXPECT_TRUE(outcome.reported.empty());
    ASSERT_EQ(received.size(), messages.size());
    for (std::size_t index = 0; index < messages.size(); ++index) {
        EXPECT_EQ(received[index].dump(), nlohmann::json::parse(messages[index]).at("params").dump())
            << "message " << index;
    }
    EXPECT_EQ(received[1].at("n"), 2);
    EXPECT_EQ(received[3].at("n"), "\xF0\x9F\x98\x80");
}

// The notification callback is application code and it runs on the read loop. A bug in it is the
// application's to fix, not grounds for the SDK to tear down the connection -- but it must not
// vanish either, or the application cannot find the bug. A callback that throws something other
// than an exception is reported by its method alone.
TEST_F(ClientNotificationsTest, ThrowingNotificationCallbackDoesNotStopReadLoop) {
    const auto outcome = deliver_notifications(
        io_ctx_,
        {{{"jsonrpc", "2.0"},
          {"method", "notifications/message"},
          {"params", nlohmann::json::object()}},
         {{"jsonrpc", "2.0"}, {"method", "notifications/tools/list_changed"}}},
        [](mcp::Client& client) {
            client.on_notification("notifications/message", [](const nlohmann::json&) {
                throw std::runtime_error("callback blew up");
            });
            client.on_notification("notifications/tools/list_changed",
                                   [](const nlohmann::json&) { throw 42; });
        });

    EXPECT_TRUE(outcome.survived) << "the session died on a throwing application callback: "
                                  << outcome.failure;
    ASSERT_EQ(outcome.reported.size(), 2U) << "a throw was silent";
    EXPECT_EQ(outcome.reported[0].code, mcp::constants::jsonrpc::g_INTERNAL_ERROR);
    EXPECT_NE(outcome.reported[0].message.find("notifications/message"), std::string::npos)
        << "actual: " << outcome.reported[0].message;
    EXPECT_NE(outcome.reported[0].message.find("callback blew up"), std::string::npos)
        << "actual: " << outcome.reported[0].message;
    EXPECT_EQ(outcome.reported[1].code, mcp::constants::jsonrpc::g_INTERNAL_ERROR);
    EXPECT_EQ(outcome.reported[1].message,
              "Notification callback for notifications/tools/list_changed threw");
}

namespace {

// Connects a legacy client to a scripted peer that answers initialize itself and every other
// request with the messages @p answer returns for it, runs @p body on the connected client, and
// returns what ClientOptions::on_protocol_error saw. @p written receives every message the client
// sent.
std::vector<mcp::Error> run_with_scripted_peer(
    boost::asio::io_context& io_ctx,
    const std::function<std::vector<nlohmann::json>(const nlohmann::json& request)>& answer,
    const std::function<mcp::Task<void>(mcp::Client&)>& body,
    std::vector<nlohmann::json>* written = nullptr) {
    auto transport = std::make_shared<ScriptedTransport>(io_ctx.get_executor());
    auto* raw = transport.get();

    std::vector<mcp::Error> reported;
    mcp::ClientOptions options;
    // Bounded, so that a swallowed response surfaces as a test failure instead of a stalled run.
    options.request_timeout = g_hang_guard;
    options.on_protocol_error = [&reported](const mcp::Error& error) { reported.push_back(error); };
    options.protocol = mcp::ClientProtocol::legacy();  // the scripted peer answers initialize
    mcp::Client client(transport, io_ctx.get_executor(), options);

    raw->set_on_write([&](std::string_view msg) {
        const auto request = nlohmann::json::parse(msg);
        if (written != nullptr) {
            written->push_back(request);
        }
        if (!request.contains("id")) {
            return;
        }
        if (request.value("method", "") == "initialize") {
            raw->enqueue_message(nlohmann::json{
                {"jsonrpc", "2.0"}, {"id", request.at("id")}, {"result", make_initialize_result()}}
                                     .dump());
            return;
        }
        for (const auto& message : answer(request)) {
            raw->enqueue_message(message.dump());
        }
    });

    mcp_test::spawn_detached(io_ctx, [&]() -> mcp::Task<void> {
        static_cast<void>(co_await client.connect("test-client", "1.0"));
        co_await body(client);
        raw->close();
    });

    io_ctx.run();
    return reported;
}

nlohmann::json empty_result_for(const nlohmann::json& request) {
    return {{"jsonrpc", "2.0"}, {"id", request.at("id")}, {"result", nlohmann::json::object()}};
}

}  // namespace

// RequestOptions::on_progress receives the progress of its own request, under the token the
// application put in `_meta`. Progress params that cannot be decoded, and a callback that throws,
// are reported; neither costs the request its result, and none of it reaches Client::on_progress.
TEST_F(ClientNotificationsTest, RequestProgressCallbackSurvivesBadProgressAndItsOwnThrow) {
    std::vector<double> progress_seen;
    int unrouted = 0;
    nlohmann::json sent_token;
    bool answered = false;
    std::string failure;

    const auto reported = run_with_scripted_peer(
        io_ctx_,
        [&sent_token](const nlohmann::json& request) {
            sent_token = request.at("params").at("_meta").at("progressToken");
            const auto progress = [&request](nlohmann::json value) {
                return nlohmann::json{
                    {"jsonrpc", "2.0"},
                    {"method", "notifications/progress"},
                    {"params",
                     {{"progressToken", request.at("params").at("_meta").at("progressToken")},
                      {"progress", std::move(value)}}}};
            };
            return std::vector<nlohmann::json>{progress(1),
                                               progress("half"),
                                               progress(2),
                                               {{"jsonrpc", "2.0"},
                                                {"id", request.at("id")},
                                                {"result", {{"content", nlohmann::json::array()}}}}};
        },
        [&](mcp::Client& client) -> mcp::Task<void> {
            client.on_progress([&unrouted](const mcp::ProgressNotificationParams&) { ++unrouted; });
            mcp::RequestOptions options;
            options.on_progress = [&progress_seen](const mcp::ProgressNotificationParams& params) {
                if (params.progress == 1) {
                    throw std::runtime_error("progress bar is gone");
                }
                progress_seen.push_back(params.progress);
            };
            nlohmann::json params = {{"name", "slow"},
                                     {"arguments", nlohmann::json::object()},
                                     {"_meta", {{"progressToken", 7}}}};
            try {
                static_cast<void>(co_await client.send_request("tools/call", params, options));
                answered = true;
            } catch (const std::exception& error) {
                failure = error.what();
            }
        });

    EXPECT_TRUE(answered) << "the request failed: " << failure;
    EXPECT_EQ(sent_token, nlohmann::json(7)) << "the application's token was replaced";
    EXPECT_EQ(progress_seen, std::vector<double>{2});
    EXPECT_EQ(unrouted, 0) << "progress of a request with a callback reached Client::on_progress";
    ASSERT_EQ(reported.size(), 2U);
    EXPECT_EQ(reported[0].code, mcp::constants::jsonrpc::g_INTERNAL_ERROR);
    EXPECT_NE(reported[0].message.find("Progress callback threw"), std::string::npos)
        << "actual: " << reported[0].message;
    EXPECT_NE(reported[0].message.find("progress bar is gone"), std::string::npos)
        << "actual: " << reported[0].message;
    EXPECT_EQ(reported[1].code, mcp::constants::jsonrpc::g_INVALID_PARAMS);
    EXPECT_NE(reported[1].message.find("Malformed progress notification params"), std::string::npos)
        << "actual: " << reported[1].message;
}

// Options the client cannot honour fail the call before anything is sent: a timeout that is not
// positive, and, with an on_progress callback, params that are not an object, a `_meta` that is not
// an object or a progress token that is neither a string nor an integer. The session goes on.
TEST_F(ClientNotificationsTest, RequestOptionsThatCannotBeHonouredFailBeforeTheRequestIsSent) {
    std::vector<nlohmann::json> written;
    std::vector<std::string> outcomes;
    bool pinged = false;

    const auto reported = run_with_scripted_peer(
        io_ctx_,
        [](const nlohmann::json& request) {
            return std::vector<nlohmann::json>{empty_result_for(request)};
        },
        [&](mcp::Client& client) -> mcp::Task<void> {
            const std::vector<std::chrono::milliseconds> unusable_timeouts = {
                std::chrono::milliseconds(0), std::chrono::milliseconds(-1)};
            for (const auto timeout : unusable_timeouts) {
                mcp::RequestOptions options;
                options.timeout = timeout;
                try {
                    static_cast<void>(co_await client.send_request("ping", std::nullopt, options));
                    outcomes.emplace_back("sent");
                } catch (const std::invalid_argument&) {
                    outcomes.emplace_back("invalid_argument");
                }
            }
            const std::vector<nlohmann::json> unusable_params = {
                nlohmann::json::array({1}),
                {{"_meta", "not an object"}},
                {{"_meta", {{"progressToken", 1.5}}}},
            };
            for (const auto& params : unusable_params) {
                mcp::RequestOptions options;
                options.on_progress = [](const mcp::ProgressNotificationParams&) {};
                const std::optional<nlohmann::json> request_params(std::in_place, params);
                try {
                    static_cast<void>(co_await client.send_request("ping", request_params, options));
                    outcomes.emplace_back("sent");
                } catch (const mcp::McpError& error) {
                    outcomes.emplace_back(error.code() == mcp::constants::jsonrpc::g_INVALID_PARAMS
                                              ? "invalid_params"
                                              : error.what());
                }
            }
            co_await client.ping();
            pinged = true;
        },
        &written);

    EXPECT_EQ(outcomes,
              (std::vector<std::string>{"invalid_argument", "invalid_argument", "invalid_params",
                                        "invalid_params", "invalid_params"}));
    EXPECT_TRUE(pinged) << "the session did not survive the refused calls";
    int pings = 0;
    for (const auto& message : written) {
        pings += message.value("method", "") == "ping" ? 1 : 0;
    }
    EXPECT_EQ(pings, 1) << "a refused call reached the wire";
    EXPECT_TRUE(reported.empty());
}

}  // namespace client_notifications_test

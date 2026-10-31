#include "../pipe_stream.hpp"
#include "../test_utils.hpp"

#include "mcp/client/client.hpp"
#include "mcp/server/server.hpp"
#include "mcp/transport/http_client.hpp"
#include "mcp/transport/http_session_manager.hpp"
#include "mcp/transport/memory.hpp"
#include "mcp/transport/stdio.hpp"
#include "mcp/transport/websocket.hpp"

#include <gtest/gtest.h>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <istream>
#include <memory>
#include <mutex>
#include <optional>
#include <ostream>
#include <streambuf>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using nlohmann::json;

/// Only a guard against a hang: no test asserts that anything finishes within it.
constexpr auto g_hang_guard = std::chrono::seconds(30);

constexpr std::string_view g_subscription_key = "io.modelcontextprotocol/subscriptionId";

json response_to(const json& request, json result) {
    return {{"jsonrpc", "2.0"}, {"id", request.at("id")}, {"result", std::move(result)}};
}

json discover_result() {
    return {{"resultType", "complete"},
            {"supportedVersions", json::array({"2026-07-28"})},
            {"capabilities", {{"tools", {{"listChanged", true}}}}},
            {"_meta", {{"io.modelcontextprotocol/serverInfo", {{"name", "peer"}, {"version", "1"}}}}}};
}

/// A notification of subscription @p id, as a server sends it.
json notification_of(const json& id, std::string method, json params = json::object()) {
    params["_meta"] = {{std::string(g_subscription_key), id}};
    return {{"jsonrpc", "2.0"}, {"method", std::move(method)}, {"params", std::move(params)}};
}

/// The empty result a server ends a stream with.
json closing_result(const json& id) {
    return {
        {"jsonrpc", "2.0"},
        {"id", id},
        {"result", {{"resultType", "complete"}, {"_meta", {{std::string(g_subscription_key), id}}}}}};
}

/// A client on a scripted transport whose peer is modern and acknowledges every listen with
/// `honor(requested)`, or with nothing when `acknowledge` is off.
class Harness {
   public:
    explicit Harness(mcp::ClientOptions options = {})
        : transport(std::make_shared<ScriptedTransport>(io.get_executor())), guard(io) {
        options.protocol = mcp::ClientProtocol::Version("2026-07-28");
        client = std::make_unique<mcp::Client>(transport, io.get_executor(), std::move(options));
        transport->set_on_write([this](std::string_view text) { on_write(json::parse(text)); });
    }

    void run(std::function<mcp::Task<void>()> body) {
        guard.expires_after(g_hang_guard);
        guard.async_wait([this](const boost::system::error_code& error) {
            if (!error) {
                guard_fired = true;
                io.stop();
            }
        });
        boost::asio::co_spawn(
            io,
            [this, body = std::move(body)]() -> mcp::Task<void> {
                try {
                    co_await body();
                } catch (const std::exception& error) {
                    ADD_FAILURE() << "unexpected exception: " << error.what();
                }
                client->close();
                guard.cancel();
            },
            boost::asio::detached);
        io.run();
        EXPECT_FALSE(guard_fired) << "the test did not finish";
    }

    void deliver(const json& message) { transport->enqueue_message(message.dump()); }

    [[nodiscard]] std::vector<json> with_method(std::string_view method) const {
        std::vector<json> matching;
        for (const auto& message : seen) {
            if (message.value("method", "") == method) {
                matching.push_back(message);
            }
        }
        return matching;
    }

    /// What the peer honors of a requested filter; everything unless set.
    std::function<json(const json&)> honor = [](const json& requested) { return requested; };
    bool acknowledge{true};
    /// Answers a listen request with an error instead of acknowledging it.
    std::optional<mcp::Error> refuse;
    std::vector<json> seen;

    boost::asio::io_context io;
    std::shared_ptr<ScriptedTransport> transport;
    std::unique_ptr<mcp::Client> client;
    boost::asio::steady_timer guard;
    bool guard_fired{false};

   private:
    void on_write(const json& message) {
        seen.push_back(message);
        if (!message.contains("method") || !message.contains("id")) {
            return;
        }
        const auto method = message.at("method").get<std::string>();
        if (method == "server/discover") {
            deliver(response_to(message, discover_result()));
        } else if (method == "subscriptions/listen") {
            if (refuse) {
                deliver({{"jsonrpc", "2.0"}, {"id", message.at("id")}, {"error", json(*refuse)}});
            } else if (acknowledge) {
                json params = {{"notifications", honor(message.at("params").at("notifications"))}};
                deliver(notification_of(message.at("id"), "notifications/subscriptions/acknowledged",
                                        std::move(params)));
            }
        }
    }
};

/// Waits, a millisecond at a time, until @p condition holds or the hang guard would fire.
mcp::Task<bool> wait_until(std::function<bool()> condition) {
    auto executor = co_await boost::asio::this_coro::executor;
    const auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!condition() && std::chrono::steady_clock::now() < give_up) {
        boost::asio::steady_timer timer(executor, std::chrono::milliseconds(1));
        co_await timer.async_wait(boost::asio::use_awaitable);
    }
    co_return condition();
}

/// Reads events until the stream has ended; the methods in arrival order.
mcp::Task<std::vector<std::string>> drain(mcp::Subscription subscription) {
    std::vector<std::string> methods;
    for (;;) {
        auto event = co_await subscription.next();
        if (!event) {
            co_return methods;
        }
        methods.push_back(event->method);
    }
}

}  // namespace

// "The client SHOULD check the acknowledged filter against what it requested": the handle keeps
// both, and says what the server left out.
TEST(ClientListen, AckSubsetDetected) {
    Harness harness;
    harness.honor = [](const json&) {
        return json{{"toolsListChanged", true}, {"resourceSubscriptions", json::array({"file:///a"})}};
    };
    mcp::SubscriptionFilter requested;
    requested.toolsListChanged = true;
    requested.promptsListChanged = true;
    requested.resourceSubscriptions = {"file:///a", "file:///b"};

    mcp::Subscription subscription;
    harness.run([&]() -> mcp::Task<void> {
        co_await harness.client->connect("listen-client", "1.0");
        subscription = co_await harness.client->listen(requested);
    });

    EXPECT_EQ(subscription.requested(), requested);
    EXPECT_TRUE(subscription.acknowledged().toolsListChanged);
    EXPECT_FALSE(subscription.acknowledged().promptsListChanged);
    EXPECT_EQ(subscription.acknowledged().resourceSubscriptions, std::vector<std::string>{"file:///a"});
    const auto missing = subscription.unhonored();
    EXPECT_FALSE(missing.toolsListChanged);
    EXPECT_TRUE(missing.promptsListChanged);
    EXPECT_EQ(missing.resourceSubscriptions, std::vector<std::string>{"file:///b"});

    // The request carries the per-request fields and the filter exactly as asked.
    const auto requests = harness.with_method("subscriptions/listen");
    ASSERT_EQ(requests.size(), 1u);
    const auto& params = requests[0].at("params");
    EXPECT_EQ(params.at("_meta").at("io.modelcontextprotocol/protocolVersion"), "2026-07-28");
    EXPECT_EQ(params.at("notifications"),
              (json{{"toolsListChanged", true},
                    {"promptsListChanged", true},
                    {"resourceSubscriptions", json::array({"file:///a", "file:///b"})}}));
}

// "Each subscription is identified by the JSON-RPC request ID of its subscriptions/listen request,
// and every notification on the stream carries that ID ... so clients can demultiplex them."
TEST(ClientListen, TwoListensDemux) {
    Harness harness;
    std::vector<std::string> handler_methods;
    harness.client->on_notification("notifications/tools/list_changed",
                                    [&](const json&) { handler_methods.push_back("handler"); });
    mcp::SubscriptionFilter tools;
    tools.toolsListChanged = true;
    mcp::SubscriptionFilter resources;
    resources.resourceSubscriptions = {"file:///a"};

    std::vector<std::string> first_methods;
    std::vector<std::string> second_methods;
    json first_params;
    harness.run([&]() -> mcp::Task<void> {
        co_await harness.client->connect("listen-client", "1.0");
        auto first = co_await harness.client->listen(tools);
        auto second = co_await harness.client->listen(resources);
        EXPECT_NE(first.id(), second.id());
        const json first_id = first.id();
        const json second_id = second.id();

        // Interleaved on the one connection.
        harness.deliver(
            notification_of(second_id, "notifications/resources/updated", json{{"uri", "file:///a"}}));
        harness.deliver(notification_of(first_id, "notifications/tools/list_changed"));
        harness.deliver(notification_of(first_id, "notifications/tools/list_changed"));
        harness.deliver(
            notification_of(second_id, "notifications/resources/updated", json{{"uri", "file:///a"}}));
        harness.deliver(notification_of(json("999"), "notifications/tools/list_changed"));
        harness.deliver(closing_result(first_id));
        harness.deliver(closing_result(second_id));

        first_methods = co_await drain(first);
        second_methods = co_await drain(second);
    });

    EXPECT_EQ(first_methods, (std::vector<std::string>(2, "notifications/tools/list_changed")));
    EXPECT_EQ(second_methods, (std::vector<std::string>(2, "notifications/resources/updated")));
    // The registered handler still sees every notification of its method, including the one of
    // a stream this client does not know.
    EXPECT_EQ(handler_methods.size(), 3u);
}

// "A client that receives this response knows the subscription closed cleanly; a transport that
// closes without it indicates an unexpected disconnect."
// On a transport without per-request streams the server closes a stream with notifications/cancelled
// naming the listen request, after the closing result or on its own. Either way the stream ends as
// closed by the server, and the cancellation is not one of its notifications.
TEST(ClientListen, ServerCancellationEndsTheStream) {
    std::vector<mcp::Error> reported;
    mcp::ClientOptions options;
    options.on_protocol_error = [&reported](const mcp::Error& error) { reported.push_back(error); };
    Harness harness(options);
    bool handler_saw_cancel = false;
    harness.client->on_notification("notifications/cancelled",
                                    [&](const json&) { handler_saw_cancel = true; });
    mcp::SubscriptionFilter tools;
    tools.toolsListChanged = true;
    const auto cancellation = [](const json& id) {
        auto message = notification_of(id, "notifications/cancelled", {{"requestId", id}});
        message["params"]["reason"] = "The server closed the subscription";
        return message;
    };

    std::optional<mcp::SubscriptionEnd> after_result;
    std::optional<mcp::SubscriptionEnd> alone;
    std::vector<std::string> after_result_methods;
    std::vector<std::string> alone_methods;
    harness.run([&]() -> mcp::Task<void> {
        co_await harness.client->connect("listen-client", "1.0");
        auto first = co_await harness.client->listen(tools);
        auto second = co_await harness.client->listen(tools);
        harness.deliver(closing_result(json(first.id())));
        harness.deliver(cancellation(json(first.id())));
        harness.deliver(cancellation(json(second.id())));
        after_result_methods = co_await drain(first);
        alone_methods = co_await drain(second);
        after_result = first.end_reason();
        alone = second.end_reason();
    });

    EXPECT_EQ(after_result, mcp::SubscriptionEnd::eServerClosed);
    EXPECT_EQ(alone, mcp::SubscriptionEnd::eServerClosed);
    EXPECT_TRUE(after_result_methods.empty());
    EXPECT_TRUE(alone_methods.empty());
    EXPECT_TRUE(reported.empty());
    EXPECT_FALSE(handler_saw_cancel);
}

TEST(ClientListen, GracefulCloseVsDrop) {
    Harness harness;
    mcp::SubscriptionFilter tools;
    tools.toolsListChanged = true;

    std::optional<mcp::SubscriptionEnd> graceful;
    std::optional<mcp::SubscriptionEnd> failed;
    std::optional<mcp::SubscriptionEnd> dropped;
    std::optional<mcp::Error> failed_error;
    std::optional<mcp::Error> dropped_error;
    std::optional<mcp::Error> graceful_error;
    std::vector<std::string> graceful_methods;
    harness.run([&]() -> mcp::Task<void> {
        co_await harness.client->connect("listen-client", "1.0");
        auto closed = co_await harness.client->listen(tools);
        auto refused = co_await harness.client->listen(tools);
        auto cut = co_await harness.client->listen(tools);
        EXPECT_FALSE(closed.end_reason().has_value());

        harness.deliver(notification_of(json(closed.id()), "notifications/tools/list_changed"));
        harness.deliver(closing_result(json(closed.id())));
        harness.deliver({{"jsonrpc", "2.0"},
                         {"id", json(refused.id())},
                         {"error", {{"code", mcp::g_INTERNAL_ERROR}, {"message", "shutting down"}}}});
        graceful_methods = co_await drain(closed);
        co_await drain(refused);
        graceful = closed.end_reason();
        graceful_error = closed.end_error();
        failed = refused.end_reason();
        failed_error = refused.end_error();

        // No closing result: the connection just ends.
        harness.transport->close();
        co_await drain(cut);
        dropped = cut.end_reason();
        dropped_error = cut.end_error();
    });

    EXPECT_EQ(graceful_methods, std::vector<std::string>{"notifications/tools/list_changed"});
    EXPECT_EQ(graceful, mcp::SubscriptionEnd::eServerClosed);
    EXPECT_FALSE(graceful_error.has_value());
    EXPECT_EQ(failed, mcp::SubscriptionEnd::eFailed);
    ASSERT_TRUE(failed_error.has_value());
    EXPECT_EQ(failed_error->code, mcp::g_INTERNAL_ERROR);
    EXPECT_EQ(dropped, mcp::SubscriptionEnd::eDropped);
    ASSERT_TRUE(dropped_error.has_value());
    EXPECT_EQ(dropped_error->code, mcp::g_CONNECTION_CLOSED);
}

// "close the SSE stream (HTTP) or send notifications/cancelled referencing the
// subscriptions/listen request ID (stdio)."
TEST(ClientListen, CloseSendsCancelledForTheListenId) {
    Harness harness;
    mcp::SubscriptionFilter tools;
    tools.toolsListChanged = true;

    json id;
    std::vector<std::string> methods;
    std::optional<mcp::SubscriptionEnd> reason;
    std::size_t other_open = 0;
    harness.run([&]() -> mcp::Task<void> {
        co_await harness.client->connect("listen-client", "1.0");
        auto stream = co_await harness.client->listen(tools);
        auto other = co_await harness.client->listen(tools);
        id = json(stream.id());
        co_await stream.close();
        // A second close and a late notification of the closed stream change nothing.
        co_await stream.close();
        harness.deliver(notification_of(id, "notifications/tools/list_changed"));
        methods = co_await drain(stream);
        reason = stream.end_reason();
        other_open = other.end_reason().has_value() ? 0 : 1;
    });

    const auto cancels = harness.with_method("notifications/cancelled");
    ASSERT_EQ(cancels.size(), 1u);
    EXPECT_EQ(cancels[0].at("params").at("requestId"), id);
    EXPECT_TRUE(methods.empty());
    EXPECT_EQ(reason, mcp::SubscriptionEnd::eCancelled);
    EXPECT_EQ(other_open, 1u);
}

// A next() that is waiting when the stream is closed resolves to nothing.
TEST(ClientListen, CloseWakesAWaitingNext) {
    Harness harness;
    mcp::SubscriptionFilter tools;
    tools.toolsListChanged = true;

    bool got_nothing = false;
    harness.run([&]() -> mcp::Task<void> {
        co_await harness.client->connect("listen-client", "1.0");
        auto stream = co_await harness.client->listen(tools);
        boost::asio::co_spawn(
            harness.io, [stream]() mutable -> mcp::Task<void> { co_await stream.close(); },
            boost::asio::detached);
        auto event = co_await stream.next();
        got_nothing = !event.has_value();
    });
    EXPECT_TRUE(got_nothing);
}

// The 2026-07-28 request does not exist for an earlier revision: nothing is sent.
TEST(ClientListen, LegacySessionIsRefused) {
    boost::asio::io_context io;
    auto transport = std::make_shared<ScriptedTransport>(io.get_executor());
    std::vector<json> seen;
    transport->set_on_write([&](std::string_view text) {
        auto message = json::parse(text);
        seen.push_back(message);
        if (message.contains("id") && message.at("method") == "initialize") {
            transport->enqueue_message(
                response_to(message, {{"protocolVersion", "2025-11-25"},
                                      {"capabilities", json::object()},
                                      {"serverInfo", {{"name", "legacy"}, {"version", "1"}}}})
                    .dump());
        }
    });
    mcp::Client client(transport, io.get_executor(), legacy_client_options());

    int code = 0;
    boost::asio::co_spawn(
        io,
        [&]() -> mcp::Task<void> {
            co_await client.connect("listen-client", "1.0");
            try {
                mcp::SubscriptionFilter tools;
                tools.toolsListChanged = true;
                co_await client.listen(tools);
            } catch (const mcp::McpError& error) {
                code = error.code();
            }
            client.close();
        },
        boost::asio::detached);
    io.run();

    EXPECT_EQ(code, mcp::g_METHOD_NOT_FOUND);
    for (const auto& message : seen) {
        EXPECT_NE(message.value("method", ""), "subscriptions/listen");
    }
}

// An error answer before the acknowledgment (for example a malformed filter) fails listen().
TEST(ClientListen, ErrorBeforeAcknowledgmentFailsListen) {
    Harness harness;
    harness.refuse = mcp::Error{mcp::g_INVALID_PARAMS, "bad filter", std::nullopt};
    int code = 0;
    harness.run([&]() -> mcp::Task<void> {
        co_await harness.client->connect("listen-client", "1.0");
        const mcp::SubscriptionFilter nothing;
        try {
            co_await harness.client->listen(nothing);
        } catch (const mcp::McpError& error) {
            code = error.code();
        }
    });
    EXPECT_EQ(code, mcp::g_INVALID_PARAMS);
}

// No acknowledgment: listen() times out, and the half-open stream is cancelled at the server.
TEST(ClientListen, MissingAcknowledgmentTimesOutAndCancels) {
    Harness harness;
    harness.acknowledge = false;
    int code = 0;
    harness.run([&]() -> mcp::Task<void> {
        co_await harness.client->connect("listen-client", "1.0");
        mcp::RequestOptions options;
        options.timeout = std::chrono::milliseconds(20);
        try {
            mcp::SubscriptionFilter tools;
            tools.toolsListChanged = true;
            co_await harness.client->listen(tools, options);
        } catch (const mcp::McpError& error) {
            code = error.code();
        }
    });
    EXPECT_EQ(code, mcp::g_REQUEST_TIMEOUT);
    const auto listens = harness.with_method("subscriptions/listen");
    const auto cancels = harness.with_method("notifications/cancelled");
    ASSERT_EQ(listens.size(), 1u);
    ASSERT_EQ(cancels.size(), 1u);
    EXPECT_EQ(cancels[0].at("params").at("requestId"), listens[0].at("id"));
}

// A stream nobody reads keeps the newest notifications and counts the rest.
TEST(ClientListen, FullQueueDropsTheOldest) {
    mcp::ClientOptions options;
    options.subscription_queue_limit = 2;
    Harness harness(options);
    mcp::SubscriptionFilter tools;
    tools.toolsListChanged = true;

    std::vector<json> kept;
    std::uint64_t dropped = 0;
    harness.run([&]() -> mcp::Task<void> {
        co_await harness.client->connect("listen-client", "1.0");
        auto stream = co_await harness.client->listen(tools);
        for (int index = 0; index < 4; ++index) {
            harness.deliver(notification_of(json(stream.id()), "notifications/tools/list_changed",
                                            json{{"n", index}}));
        }
        harness.deliver(closing_result(json(stream.id())));
        // Everything has arrived once the closing result has; only then does anything read.
        const bool ended = co_await wait_until([&stream] { return stream.end_reason().has_value(); });
        EXPECT_TRUE(ended);
        for (;;) {
            auto event = co_await stream.next();
            if (!event) {
                break;
            }
            kept.push_back(event->params.at("n"));
        }
        dropped = stream.dropped_events();
    });
    EXPECT_EQ(kept, (std::vector<json>{2, 3}));
    EXPECT_EQ(dropped, 2u);
}

TEST(ClientListen, ListenBeforeConnectFails) {
    boost::asio::io_context io;
    auto transport = std::make_shared<ScriptedTransport>(io.get_executor());
    mcp::Client client(transport, io.get_executor());
    EXPECT_THROW(
        {
            auto pending = client.listen(mcp::SubscriptionFilter{});
            (void)pending;
        },
        mcp::McpError);
}

// ---------------------------------------------------------------------------
// The SDK client against the SDK server
// ---------------------------------------------------------------------------

namespace {

constexpr std::string_view g_uri = "file:///project/config.json";

/// A server that can emit tools and resources change notifications but has no prompts.
std::unique_ptr<mcp::Server> make_listen_server() {
    mcp::ServerCapabilities capabilities;
    capabilities.tools = mcp::ServerCapabilities::ToolsCapability{true};
    capabilities.resources = mcp::ServerCapabilities::ResourcesCapability{true, true};
    return std::make_unique<mcp::Server>(mcp::Implementation{"listen-server", "1.0"}, capabilities);
}

/// What the cross-transport scenario observed.
struct ListenOutcome {
    std::string error;
    mcp::SubscriptionFilter unhonored;
    std::vector<std::string> tools_stream;
    std::vector<std::string> resources_stream;
    std::string uri_seen;
    bool cancelled_stream_released{false};
    std::optional<mcp::SubscriptionEnd> graceful_end;
    std::optional<mcp::SubscriptionEnd> cancelled_end;
};

/// Two streams on one connection, notifications of both kinds, a client cancel and a server close.
mcp::Task<void> listen_scenario(mcp::Client& client, mcp::Server& server,
                                std::shared_ptr<ListenOutcome> outcome) {
    try {
        const auto connected = co_await client.connect("listen-client", "1.0");
        if (connected.era != mcp::ProtocolEra::eModern) {
            outcome->error = "not a modern session";
            co_return;
        }
        mcp::SubscriptionFilter tools_request;
        tools_request.toolsListChanged = true;
        tools_request.promptsListChanged = true;  // the server has no prompts
        tools_request.resourceSubscriptions = {std::string(g_uri)};
        auto tools = co_await client.listen(tools_request);
        outcome->unhonored = tools.unhonored();

        mcp::SubscriptionFilter resources_request;
        resources_request.resourcesListChanged = true;
        auto resources = co_await client.listen(resources_request);
        EXPECT_NE(tools.id(), resources.id());

        co_await server.notify_tools_list_changed();
        co_await server.notify_resources_list_changed();
        co_await server.notify_resource_updated(std::string(g_uri));

        auto first = co_await tools.next();
        auto second = co_await tools.next();
        auto third = co_await resources.next();
        if (!first || !second || !third) {
            outcome->error = "a notification is missing";
            co_return;
        }
        outcome->tools_stream = {first->method, second->method};
        outcome->resources_stream = {third->method};
        outcome->uri_seen = second->params.value("uri", "");

        // The client ends one stream; the server notices and keeps the other.
        co_await resources.close();
        outcome->cancelled_end = resources.end_reason();
        outcome->cancelled_stream_released =
            co_await wait_until([&server] { return server.subscription_count() == 1; });

        server.close_subscriptions();
        auto after = co_await tools.next();
        if (after) {
            outcome->error = "unexpected notification: " + after->method;
        }
        outcome->graceful_end = tools.end_reason();
    } catch (const std::exception& error) {
        outcome->error = error.what();
    }
}

void expect_listen_outcome(const ListenOutcome& outcome) {
    EXPECT_EQ(outcome.error, "");
    EXPECT_TRUE(outcome.unhonored.promptsListChanged);
    EXPECT_FALSE(outcome.unhonored.toolsListChanged);
    EXPECT_TRUE(outcome.unhonored.resourceSubscriptions.empty());
    EXPECT_EQ(outcome.tools_stream, (std::vector<std::string>{"notifications/tools/list_changed",
                                                              "notifications/resources/updated"}));
    EXPECT_EQ(outcome.resources_stream,
              std::vector<std::string>{"notifications/resources/list_changed"});
    EXPECT_EQ(outcome.uri_seen, g_uri);
    EXPECT_TRUE(outcome.cancelled_stream_released);
    EXPECT_EQ(outcome.cancelled_end, mcp::SubscriptionEnd::eCancelled);
    EXPECT_EQ(outcome.graceful_end, mcp::SubscriptionEnd::eServerClosed);
}

mcp::ClientOptions modern_options() {
    mcp::ClientOptions options;
    options.protocol = mcp::ClientProtocol::Version("2026-07-28");
    return options;
}

void run_guarded(boost::asio::io_context& io, boost::asio::steady_timer& guard, bool& fired,
                 std::function<mcp::Task<void>()> body) {
    guard.expires_after(g_hang_guard);
    guard.async_wait([&](const boost::system::error_code& error) {
        if (!error) {
            fired = true;
            io.stop();
        }
    });
    boost::asio::co_spawn(
        io,
        [&guard, body = std::move(body)]() -> mcp::Task<void> {
            co_await body();
            guard.cancel();
        },
        boost::asio::detached);
    io.run();
    EXPECT_FALSE(fired) << "the test did not finish";
}

}  // namespace

TEST(ClientListen, AgainstSdkServerOverMemory) {
    boost::asio::io_context io;
    boost::asio::steady_timer guard(io);
    bool fired = false;
    auto [client_transport, server_transport] = mcp::create_memory_transport_pair(io.get_executor());
    auto server = make_listen_server();
    mcp::Client client(client_transport, io.get_executor(), modern_options());
    auto outcome = std::make_shared<ListenOutcome>();

    boost::asio::co_spawn(
        io,
        [&]() -> mcp::Task<void> {
            try {
                co_await server->run(server_transport, io.get_executor());
            } catch (...) {
            }
        },
        boost::asio::detached);
    run_guarded(io, guard, fired, [&]() -> mcp::Task<void> {
        co_await listen_scenario(client, *server, outcome);
        client.close();
        server_transport->close();
    });
    expect_listen_outcome(*outcome);
}

// A real stdio pair: the client reads and writes streams, the server runs run_stdio() on a thread.
TEST(ClientListen, AgainstSdkServerOverStdio) {
    PipeBuffer to_server;
    PipeBuffer to_client;
    PipeWriter server_out(to_client);
    PipeWriter client_out(to_server);
    std::istream server_in(&to_server);
    std::ostream server_output(&server_out);
    std::istream client_in(&to_client);
    std::ostream client_output(&client_out);

    auto server = make_listen_server();
    std::thread server_thread([&] { server->run_stdio(server_in, server_output); });

    boost::asio::io_context io;
    boost::asio::steady_timer guard(io);
    bool fired = false;
    auto transport = std::make_shared<mcp::StdioTransport>(io.get_executor(), client_in, client_output);
    mcp::Client client(transport, io.get_executor(), modern_options());
    auto outcome = std::make_shared<ListenOutcome>();

    run_guarded(io, guard, fired, [&]() -> mcp::Task<void> {
        co_await listen_scenario(client, *server, outcome);
        client.close();
    });

    // Let the server see end of input, then the client's reader see end of input.
    to_server.close();
    server_thread.join();
    to_client.close();
    expect_listen_outcome(*outcome);
}

// Over Streamable HTTP each subscription is its own POST with an open event stream, served by the
// session manager's server for 2026-07-28 requests.
TEST(ClientListen, AgainstSdkServerOverHttp) {
    constexpr unsigned short port = 20580;
    boost::asio::io_context io;
    boost::asio::steady_timer guard(io);
    bool fired = false;
    std::shared_ptr<mcp::Server> server = make_listen_server();
    mcp::StreamableHttpSessionManager manager(
        io.get_executor(), "127.0.0.1", port,
        [](const boost::asio::any_io_executor&) { return make_listen_server(); });
    manager.set_modern_server(server);
    auto transport = std::make_shared<mcp::HttpClientTransport>(
        io.get_executor(), "http://127.0.0.1:" + std::to_string(port) + "/mcp");
    mcp::Client client(transport, io.get_executor(), modern_options());
    auto outcome = std::make_shared<ListenOutcome>();

    boost::asio::co_spawn(io, manager.listen(), boost::asio::detached);
    run_guarded(io, guard, fired, [&]() -> mcp::Task<void> {
        co_await listen_scenario(client, *server, outcome);
        client.close();
        manager.close();
    });
    expect_listen_outcome(*outcome);
}

// Without set_modern_server() the manager builds that server from its factory on first use; the
// application reaches it through modern_server() to send notifications.
TEST(ClientListen, WorksWithoutSetModernServer) {
    constexpr unsigned short port = 20581;
    boost::asio::io_context io;
    boost::asio::steady_timer guard(io);
    bool fired = false;
    mcp::StreamableHttpSessionManager manager(
        io.get_executor(), "127.0.0.1", port,
        [](const boost::asio::any_io_executor&) { return make_listen_server(); });
    auto server = manager.modern_server();
    auto transport = std::make_shared<mcp::HttpClientTransport>(
        io.get_executor(), "http://127.0.0.1:" + std::to_string(port) + "/mcp");
    mcp::Client client(transport, io.get_executor(), modern_options());
    auto outcome = std::make_shared<ListenOutcome>();

    boost::asio::co_spawn(io, manager.listen(), boost::asio::detached);
    run_guarded(io, guard, fired, [&]() -> mcp::Task<void> {
        co_await listen_scenario(client, *server, outcome);
        client.close();
        manager.close();
    });
    expect_listen_outcome(*outcome);
}

TEST(ClientListen, AgainstSdkServerOverWebSocket) {
    boost::asio::io_context io;
    boost::asio::steady_timer guard(io);
    bool fired = false;
    auto server = make_listen_server();
    boost::asio::ip::tcp::acceptor acceptor(io, {boost::asio::ip::tcp::v4(), 0});
    const auto port = acceptor.local_endpoint().port();
    auto outcome = std::make_shared<ListenOutcome>();

    boost::asio::co_spawn(
        io,
        [&]() -> mcp::Task<void> {
            try {
                auto socket = co_await acceptor.async_accept(boost::asio::use_awaitable);
                auto transport = std::make_shared<mcp::WebSocketServerTransport>(std::move(socket));
                co_await server->run(transport, io.get_executor());
            } catch (...) {
            }
        },
        boost::asio::detached);
    auto transport = std::make_shared<mcp::WebSocketClientTransport>(io.get_executor(), "127.0.0.1",
                                                                     std::to_string(port));
    mcp::Client client(transport, io.get_executor(), modern_options());
    run_guarded(io, guard, fired, [&]() -> mcp::Task<void> {
        co_await listen_scenario(client, *server, outcome);
        client.close();
    });
    expect_listen_outcome(*outcome);
}

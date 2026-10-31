#include "../test_utils.hpp"

#include "mcp/server/server.hpp"
#include "mcp/transport/memory.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace asio = boost::asio;
using nlohmann::json;

/// The value at JSON pointer @p pointer, or null when the message lacks it.
json field(const json& message, const char* pointer) {
    return message.value(json::json_pointer(pointer), json());
}

/// The subscription id a notification names, or null.
json subscription_of(const json& message) {
    return field(message, "/params/_meta/io.modelcontextprotocol~1subscriptionId");
}

/// The subscription id a result names, or null.
json result_subscription_of(const json& message) {
    return field(message, "/result/_meta/io.modelcontextprotocol~1subscriptionId");
}

/// Waits, a millisecond at a time, until @p condition holds. Gives up after 30 s so a broken
/// subscription fails the test instead of hanging the run.
mcp::Task<bool> wait_until(std::function<bool()> condition) {
    auto executor = co_await asio::this_coro::executor;
    const auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!condition() && std::chrono::steady_clock::now() < give_up) {
        asio::steady_timer timer(executor, std::chrono::milliseconds(1));
        co_await timer.async_wait(asio::use_awaitable);
    }
    co_return condition();
}

json listen_request(const json& id, json filter) {
    return make_modern_request(id, "subscriptions/listen", {{"notifications", std::move(filter)}});
}

json cancel_notification(const json& request_id) {
    return {{"jsonrpc", "2.0"},
            {"method", "notifications/cancelled"},
            {"params", {{"requestId", request_id}}}};
}

/// A server that advertises every change notification a subscription can ask for.
std::unique_ptr<mcp::Server> make_server(bool with_prompts = true) {
    mcp::ServerCapabilities caps;
    caps.tools = mcp::ServerCapabilities::ToolsCapability{true};
    caps.resources = mcp::ServerCapabilities::ResourcesCapability{true, true};
    if (with_prompts) {
        caps.prompts = mcp::ServerCapabilities::PromptsCapability{true};
    }
    auto server = std::make_unique<mcp::Server>(mcp::Implementation{"listen-server", "1.0"}, caps);
    server->add_tool<json, json>("echo", "Echoes", json{{"type", "object"}},
                                 [](json args) -> mcp::Task<json> { co_return args; });
    server->add_tool<json, json>("log", "Logs one error", json{{"type", "object"}},
                                 [](mcp::Context& ctx, json /*args*/) -> mcp::Task<json> {
                                     co_await ctx.log(mcp::LoggingLevel::eError, "logged");
                                     co_return json{{"logged", true}};
                                 });
    return server;
}

/// A server running on a scripted session: the test feeds it messages and sees what it writes.
class Listen : public ::testing::Test {
   protected:
    asio::io_context io_ctx_;
    std::shared_ptr<ScriptedTransport> transport_;
    std::vector<json> written_;
    std::atomic<bool> run_done_{false};

    void start(mcp::Server& server) {
        transport_ = std::make_shared<ScriptedTransport>(io_ctx_.get_executor());
        transport_->set_on_write(
            [this](std::string_view message) { written_.push_back(json::parse(message)); });
        asio::co_spawn(
            io_ctx_,
            [this, &server]() -> mcp::Task<void> {
                try {
                    co_await server.run(transport_, io_ctx_.get_executor());
                } catch (const std::exception&) {
                }
                run_done_.store(true);
            },
            asio::detached);
    }

    void send(const json& message) { transport_->enqueue_message(message.dump()); }

    mcp::Task<bool> wait_written(std::size_t count) {
        co_return co_await wait_until([this, count] { return written_.size() >= count; });
    }

    /// Ends the session and waits for the server to finish with it.
    mcp::Task<void> finish() {
        transport_->close();
        const bool done = co_await wait_until([this] { return run_done_.load(); });
        EXPECT_TRUE(done);
    }

    void run(std::function<mcp::Task<void>()> scenario) {
        io_ctx_.restart();
        asio::co_spawn(io_ctx_, std::move(scenario), asio::detached);
        io_ctx_.run();
    }

    static bool is_notification(const json& message, std::string_view method) {
        return message.value("method", "") == method;
    }
};

// "The server MUST send notifications/subscriptions/acknowledged as the first message carrying the
// subscription's ID" and "The notifications field in the acknowledgment reflects the subset the
// server agreed to honor. Notification types the server does not support are omitted."
TEST_F(Listen, AckFirstWithSubset) {
    auto server = make_server(false);
    start(*server);
    run([&]() -> mcp::Task<void> {
        send(listen_request(1, {{"toolsListChanged", true},
                                {"promptsListChanged", true},
                                {"resourcesListChanged", false},
                                {"resourceSubscriptions", {"file:///a", "file:///b"}}}));
        const bool acknowledged = co_await wait_written(1);
        EXPECT_TRUE(acknowledged);
        EXPECT_EQ(server->subscription_count(), 1u);
        co_await finish();
    });

    ASSERT_EQ(written_.size(), 1u);
    const auto& ack = written_[0];
    EXPECT_EQ(field(ack, "/jsonrpc"), "2.0");
    EXPECT_EQ(field(ack, "/method"), "notifications/subscriptions/acknowledged");
    EXPECT_FALSE(ack.contains("id"));
    EXPECT_EQ(subscription_of(ack), 1);
    EXPECT_EQ(
        field(ack, "/params/notifications"),
        (json{{"toolsListChanged", true}, {"resourceSubscriptions", {"file:///a", "file:///b"}}}));
}

// "notifications/message is request-scoped: the server MUST NOT deliver it on a
// subscriptions/listen stream".
TEST_F(Listen, LogMessagesOfARequestStayOffTheSubscription) {
    auto server = make_server();
    start(*server);
    auto call = make_modern_request(2, "tools/call", {{"name", "log"}, {"arguments", json::object()}});
    call["params"]["_meta"]["io.modelcontextprotocol/logLevel"] = "debug";
    run([&]() -> mcp::Task<void> {
        send(listen_request(1, {{"toolsListChanged", true}, {"resourcesListChanged", true}}));
        co_await wait_written(1);
        send(call);
        co_await wait_written(3);
        co_await finish();
    });

    ASSERT_EQ(written_.size(), 3u);
    EXPECT_TRUE(is_notification(written_[0], "notifications/subscriptions/acknowledged"));
    EXPECT_TRUE(is_notification(written_[1], "notifications/message")) << written_[1];
    EXPECT_TRUE(subscription_of(written_[1]).is_null()) << written_[1];
    EXPECT_EQ(field(written_[2], "/id"), 2) << written_[2];
    EXPECT_TRUE(result_subscription_of(written_[2]).is_null()) << written_[2];
}

TEST_F(Listen, AckOmitsTypesTheServerDoesNotAdvertise) {
    mcp::ServerCapabilities caps;
    caps.tools = mcp::ServerCapabilities::ToolsCapability{};
    caps.resources = mcp::ServerCapabilities::ResourcesCapability{true, std::nullopt};
    mcp::Server server(mcp::Implementation{"quiet", "1.0"}, caps);
    start(server);
    run([&]() -> mcp::Task<void> {
        send(listen_request("s", {{"toolsListChanged", true},
                                  {"resourcesListChanged", true},
                                  {"resourceSubscriptions", {"file:///a"}}}));
        co_await wait_written(1);
        co_await finish();
    });

    ASSERT_EQ(written_.size(), 1u);
    EXPECT_EQ(field(written_[0], "/params/notifications"), (json{{"resourcesListChanged", true}}));
    EXPECT_EQ(subscription_of(written_[0]), "s");
}

TEST_F(Listen, NotificationsCarrySubscriptionId) {
    auto server = make_server();
    start(*server);
    run([&]() -> mcp::Task<void> {
        send(listen_request("sub-A", {{"toolsListChanged", true},
                                      {"promptsListChanged", true},
                                      {"resourcesListChanged", true},
                                      {"resourceSubscriptions", {"file:///a"}}}));
        co_await wait_written(1);
        co_await server->notify_tools_list_changed();
        co_await server->notify_prompts_list_changed();
        co_await server->notify_resources_list_changed();
        co_await server->notify_resource_updated("file:///a");
        co_await wait_written(5);
        co_await finish();
    });

    ASSERT_EQ(written_.size(), 5u);
    EXPECT_TRUE(is_notification(written_[0], "notifications/subscriptions/acknowledged"));
    const std::vector<std::string> methods = {
        "notifications/tools/list_changed", "notifications/prompts/list_changed",
        "notifications/resources/list_changed", "notifications/resources/updated"};
    for (std::size_t index = 0; index < methods.size(); ++index) {
        const auto& message = written_[index + 1];
        EXPECT_EQ(field(message, "/method"), methods[index]) << message;
        EXPECT_FALSE(message.contains("id")) << message;
        EXPECT_EQ(subscription_of(message), "sub-A") << message;
    }
    EXPECT_EQ(field(written_[4], "/params/uri"), "file:///a");
}

/// A stream sink that takes longer to write the acknowledgment than any other message.
mcp::Task<void> deliver_slowly(asio::any_io_executor executor, std::vector<json>* out,
                               std::string message) {
    if (message.find("notifications/subscriptions/acknowledged") != std::string::npos) {
        for (int turn = 0; turn < 3; ++turn) {
            co_await asio::post(executor, asio::use_awaitable);
        }
    }
    out->push_back(json::parse(message));
}

// A change that happens before the acknowledgment has been written is sent after it: "The server
// MUST NOT send any notification on the subscription before it."
TEST_F(Listen, NotificationBeforeTheAckIsWrittenFollowsIt) {
    auto server = make_server();
    std::vector<json> sent;
    std::string wire;
    mcp::RequestContext context;
    context.cancelled = std::make_shared<std::atomic<bool>>(false);
    context.notifications = [this, &sent](std::string_view message) {
        return deliver_slowly(io_ctx_.get_executor(), &sent, std::string(message));
    };
    // Called once the subscription is registered and before its acknowledgment is written.
    context.watch_cancel = [this, &server](std::function<void()> /*wake*/) {
        asio::co_spawn(io_ctx_, server->notify_tools_list_changed(), asio::detached);
    };
    asio::co_spawn(io_ctx_,
                   server->dispatch_request_direct(listen_request(1, {{"toolsListChanged", true}}),
                                                   std::move(context)),
                   [&wire](std::exception_ptr error, std::string response) {
                       EXPECT_EQ(error, nullptr);
                       wire = std::move(response);
                   });
    run([&]() -> mcp::Task<void> {
        const bool both = co_await wait_until([&sent] { return sent.size() >= 2; });
        EXPECT_TRUE(both);
        server->close_subscriptions();
        const bool answered = co_await wait_until([&wire] { return !wire.empty(); });
        EXPECT_TRUE(answered);
    });

    ASSERT_EQ(sent.size(), 2u);
    EXPECT_TRUE(is_notification(sent[0], "notifications/subscriptions/acknowledged"));
    EXPECT_TRUE(is_notification(sent[1], "notifications/tools/list_changed"));
    EXPECT_EQ(subscription_of(sent[1]), 1);
}

// "Each subscription is identified by the JSON-RPC request ID of its subscriptions/listen
// request, and every notification on the stream carries that ID".
TEST_F(Listen, TwoListensDemux) {
    auto server = make_server();
    start(*server);
    run([&]() -> mcp::Task<void> {
        send(listen_request(1, {{"toolsListChanged", true}}));
        send(listen_request(2, {{"toolsListChanged", true}, {"resourceSubscriptions", {"file:///a"}}}));
        co_await wait_written(2);
        EXPECT_EQ(server->subscription_count(), 2u);

        co_await server->notify_tools_list_changed();
        co_await wait_written(4);
        co_await server->notify_resource_updated("file:///a");
        co_await wait_written(5);
        // Nobody asked for this URI, so nothing is sent for it; the next change shows that
        // nothing was written in between.
        co_await server->notify_resource_updated("file:///other");
        co_await server->notify_tools_list_changed();
        co_await wait_written(7);
        co_await finish();
    });

    ASSERT_EQ(written_.size(), 7u);
    std::set<int> acknowledged;
    for (std::size_t index = 0; index < 2; ++index) {
        EXPECT_TRUE(is_notification(written_[index], "notifications/subscriptions/acknowledged"));
        acknowledged.insert(subscription_of(written_[index]).get<int>());
    }
    EXPECT_EQ(acknowledged, (std::set<int>{1, 2}));

    std::set<int> first_change;
    for (std::size_t index = 2; index < 4; ++index) {
        EXPECT_TRUE(is_notification(written_[index], "notifications/tools/list_changed"));
        first_change.insert(subscription_of(written_[index]).get<int>());
    }
    EXPECT_EQ(first_change, (std::set<int>{1, 2}));

    EXPECT_TRUE(is_notification(written_[4], "notifications/resources/updated"));
    EXPECT_EQ(subscription_of(written_[4]), 2);
    EXPECT_EQ(field(written_[4], "/params/uri"), "file:///a");

    std::set<int> last_change;
    for (std::size_t index = 5; index < 7; ++index) {
        EXPECT_TRUE(is_notification(written_[index], "notifications/tools/list_changed"));
        last_change.insert(subscription_of(written_[index]).get<int>());
    }
    EXPECT_EQ(last_change, (std::set<int>{1, 2}));
}

// "The server MUST NOT send notification types the client has not explicitly requested." A
// session that only serves 2026-07-28 requests gets no plain list-changed broadcast either.
TEST_F(Listen, UnrequestedTypesNeverSent) {
    auto server = make_server();
    start(*server);
    run([&]() -> mcp::Task<void> {
        send(listen_request(1, {{"toolsListChanged", true}, {"resourceSubscriptions", {"file:///a"}}}));
        co_await wait_written(1);
        co_await server->notify_prompts_list_changed();
        co_await server->notify_resources_list_changed();
        co_await server->notify_resource_updated("file:///b");
        co_await server->notify_tools_list_changed();
        co_await wait_written(2);
        co_await finish();
    });

    ASSERT_EQ(written_.size(), 2u);
    EXPECT_TRUE(is_notification(written_[0], "notifications/subscriptions/acknowledged"));
    EXPECT_TRUE(is_notification(written_[1], "notifications/tools/list_changed"));
}

// "it SHOULD respond to the original subscriptions/listen request with an empty result before
// closing the stream ... the response carries io.modelcontextprotocol/subscriptionId in _meta".
// On a transport without per-request streams the stream then closes with the one cancellation a
// server sends: "A server MUST send notifications/cancelled referencing a subscriptions/listen
// request ID when it tears down that subscription stream" (cancellation page).
TEST_F(Listen, GracefulCloseResponse) {
    auto server = make_server();
    start(*server);
    run([&]() -> mcp::Task<void> {
        send(listen_request("g", {{"toolsListChanged", true}}));
        co_await wait_written(1);
        server->close_subscriptions();
        co_await wait_written(3);
        const bool removed = co_await wait_until([&] { return server->subscription_count() == 0; });
        EXPECT_TRUE(removed);
        co_await finish();
    });

    ASSERT_EQ(written_.size(), 3u);
    const auto& response = written_[1];
    EXPECT_EQ(field(response, "/jsonrpc"), "2.0");
    EXPECT_EQ(field(response, "/id"), "g");
    ASSERT_TRUE(response.contains("result")) << response;
    EXPECT_EQ(field(response, "/result/resultType"), "complete");
    EXPECT_EQ(result_subscription_of(response), "g");
    const auto& cancellation = written_[2];
    EXPECT_TRUE(is_notification(cancellation, "notifications/cancelled")) << cancellation;
    EXPECT_EQ(field(cancellation, "/params/requestId"), "g");
    EXPECT_FALSE(cancellation.contains("id")) << cancellation;
}

// A client ends a subscription with notifications/cancelled, or by going away. Either way nothing
// more is sent for it, not even its response.
TEST_F(Listen, ListenerRemovedOnCancelAndDisconnect) {
    auto server = make_server();
    start(*server);
    run([&]() -> mcp::Task<void> {
        send(listen_request(1, {{"toolsListChanged", true}}));
        co_await wait_written(1);
        EXPECT_EQ(server->subscription_count(), 1u);

        send(cancel_notification(1));
        const bool removed = co_await wait_until([&] { return server->subscription_count() == 0; });
        EXPECT_TRUE(removed);

        send(make_modern_request(2, "tools/list"));
        co_await wait_written(2);
        co_await finish();
    });

    ASSERT_EQ(written_.size(), 2u);
    EXPECT_TRUE(is_notification(written_[0], "notifications/subscriptions/acknowledged"));
    EXPECT_EQ(written_[1]["id"], 2) << "no notification or response after the cancel: " << written_[1];

    // The transport closing while a subscription is open ends it, and the session with it.
    written_.clear();
    run_done_.store(false);
    start(*server);
    run([&]() -> mcp::Task<void> {
        send(listen_request(1, {{"toolsListChanged", true}}));
        send(listen_request(2, {{"toolsListChanged", true}}));
        co_await wait_written(2);
        EXPECT_EQ(server->subscription_count(), 2u);
        co_await finish();
    });
    EXPECT_EQ(server->subscription_count(), 0u);
    EXPECT_EQ(written_.size(), 2u);
}

TEST_F(Listen, CancelOfAnotherRequestKeepsTheSubscription) {
    auto server = make_server();
    start(*server);
    run([&]() -> mcp::Task<void> {
        send(listen_request(1, {{"toolsListChanged", true}}));
        co_await wait_written(1);
        send(cancel_notification(99));
        send(make_modern_request(2, "tools/list"));
        co_await wait_written(2);
        EXPECT_EQ(server->subscription_count(), 1u);
        co_await server->notify_tools_list_changed();
        co_await wait_written(3);
        co_await finish();
    });

    ASSERT_EQ(written_.size(), 3u);
    EXPECT_EQ(written_[1]["id"], 2);
    EXPECT_TRUE(is_notification(written_[2], "notifications/tools/list_changed"));
}

TEST_F(Listen, OtherRequestsAreAnsweredWhileAListenIsOpen) {
    auto server = make_server();
    start(*server);
    run([&]() -> mcp::Task<void> {
        send(listen_request(1, {{"toolsListChanged", true}}));
        co_await wait_written(1);
        send(make_modern_request(2, "tools/list"));
        co_await wait_written(2);
        send(make_modern_request(3, "tools/list"));
        co_await wait_written(3);
        co_await finish();
    });

    ASSERT_EQ(written_.size(), 3u);
    EXPECT_EQ(written_[1]["id"], 2);
    EXPECT_EQ(written_[2]["id"], 3);
}

TEST_F(Listen, InvalidFilterIsInvalidParams) {
    auto server = make_server();
    start(*server);
    run([&]() -> mcp::Task<void> {
        send(listen_request(1, {{"toolsListChanged", "yes"}}));
        send(listen_request(2, {{"resourceSubscriptions", {1, 2}}}));
        send(make_modern_request(3, "subscriptions/listen", {{"notifications", "all"}}));
        send(make_modern_request(4, "subscriptions/listen"));
        co_await wait_written(4);
        EXPECT_EQ(server->subscription_count(), 0u);
        co_await finish();
    });

    ASSERT_EQ(written_.size(), 4u);
    for (const auto& response : written_) {
        ASSERT_TRUE(response.contains("error")) << response;
        EXPECT_EQ(field(response, "/error/code"), mcp::g_INVALID_PARAMS) << response;
    }
}

// Without a stream to keep open there is nowhere to send the subscription's notifications.
TEST_F(Listen, DirectDispatchWithoutAStreamIsRejected) {
    auto server = make_server();
    json response;
    asio::co_spawn(io_ctx_,
                   server->dispatch_request_direct(listen_request(1, {{"toolsListChanged", true}})),
                   [&response](std::exception_ptr error, std::string wire) {
                       EXPECT_EQ(error, nullptr);
                       response = json::parse(wire);
                   });
    io_ctx_.run();

    ASSERT_TRUE(response.contains("error")) << response;
    EXPECT_EQ(field(response, "/error/code"), mcp::g_INVALID_REQUEST);
    EXPECT_EQ(server->subscription_count(), 0u);
}

// Legacy requests are untouched: subscriptions/listen does not exist for them, the plain
// notification still reaches an initialized session, and resources/subscribe still works.
TEST_F(Listen, LegacySessionSeesNoChange) {
    auto server = make_server();
    start(*server);
    run([&]() -> mcp::Task<void> {
        send(make_initialize_request("i"));
        send(make_initialized_notification());
        send({{"jsonrpc", "2.0"},
              {"id", "l"},
              {"method", "subscriptions/listen"},
              {"params", {{"notifications", {{"toolsListChanged", true}}}}}});
        send({{"jsonrpc", "2.0"},
              {"id", "s"},
              {"method", "resources/subscribe"},
              {"params", {{"uri", "file:///a"}}}});
        co_await wait_written(3);
        co_await server->notify_tools_list_changed();
        co_await server->notify_resource_updated("file:///a");
        co_await wait_written(5);
        EXPECT_EQ(server->subscription_count(), 0u);
        co_await finish();
    });

    ASSERT_EQ(written_.size(), 5u);
    std::set<std::string> answered;
    for (std::size_t index = 0; index < 3; ++index) {
        answered.insert(written_[index]["id"].get<std::string>());
        if (written_[index]["id"] == "l") {
            EXPECT_EQ(field(written_[index], "/error/code"), mcp::g_METHOD_NOT_FOUND)
                << written_[index];
        } else {
            EXPECT_TRUE(written_[index].contains("result")) << written_[index];
        }
    }
    EXPECT_EQ(answered, (std::set<std::string>{"i", "l", "s"}));
    EXPECT_EQ(written_[3], (json{{"jsonrpc", "2.0"}, {"method", "notifications/tools/list_changed"}}));
    EXPECT_EQ(written_[4], (json{{"jsonrpc", "2.0"},
                                 {"method", "notifications/resources/updated"},
                                 {"params", {{"uri", "file:///a"}}}}));
}

/// Collects what @p transport delivers until it closes.
mcp::Task<void> collect(std::shared_ptr<mcp::ITransport> transport, std::vector<json>* out,
                        std::atomic<std::size_t>* count) {
    try {
        for (;;) {
            auto message = co_await transport->read_message();
            out->push_back(json::parse(message));
            count->fetch_add(1);
        }
    } catch (const std::exception&) {
    }
}

mcp::Task<void> serve(mcp::Server* server, std::shared_ptr<mcp::ITransport> transport,
                      asio::any_io_executor executor, std::atomic<bool>* done) {
    try {
        co_await server->run(std::move(transport), std::move(executor));
    } catch (const std::exception&) {
    }
    done->store(true);
}

mcp::Task<void> write_to(std::shared_ptr<mcp::ITransport> transport, std::string message) {
    co_await transport->write_message(message);
}

// A listener is registered and removed from request handlers while other threads send
// notifications; every notification reaches the open listener exactly once.
TEST(ListenConcurrencyTest, NotificationsFromManyThreadsReachTheListener) {
    constexpr int g_threads = 4;
    constexpr int g_per_thread = 100;
    asio::thread_pool pool(g_threads);
    auto server = make_server();
    auto [client, server_end] = mcp::create_memory_transport_pair(pool.get_executor());
    std::vector<json> received;
    std::atomic<std::size_t> received_count{0};
    std::atomic<bool> run_done{false};
    asio::co_spawn(pool, collect(client, &received, &received_count), asio::detached);
    asio::co_spawn(pool, serve(server.get(), server_end, pool.get_executor(), &run_done),
                   asio::detached);

    const auto request = listen_request(1, {{"toolsListChanged", true}}).dump();
    asio::co_spawn(pool, write_to(client, request), asio::detached);
    const auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (received_count.load() < 1 && std::chrono::steady_clock::now() < give_up) {
        std::this_thread::yield();
    }
    ASSERT_GE(received_count.load(), 1u);

    std::vector<std::thread> threads;
    for (int thread = 0; thread < g_threads; ++thread) {
        threads.emplace_back([&] {
            for (int index = 0; index < g_per_thread; ++index) {
                std::promise<void> done;
                asio::co_spawn(pool, server->notify_tools_list_changed(),
                               [&done](std::exception_ptr) { done.set_value(); });
                done.get_future().wait();
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    const std::size_t expected = 1 + static_cast<std::size_t>(g_threads * g_per_thread);
    while (received_count.load() < expected && std::chrono::steady_clock::now() < give_up) {
        std::this_thread::yield();
    }
    client->close();
    while (!run_done.load() && std::chrono::steady_clock::now() < give_up) {
        std::this_thread::yield();
    }
    pool.join();

    ASSERT_EQ(received.size(), expected);
    for (std::size_t index = 1; index < received.size(); ++index) {
        EXPECT_EQ(field(received[index], "/method"), "notifications/tools/list_changed");
        EXPECT_EQ(subscription_of(received[index]), 1);
    }
    EXPECT_EQ(server->subscription_count(), 0u);
}

}  // namespace

#include "mcp/client/client.hpp"
#include "mcp/core/async.hpp"
#include "mcp/core/constants.hpp"
#include "mcp/transport/memory.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <boost/asio/io_context.hpp>
#include <cstdint>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#ifndef MCP_TEST_STDIO_SERVER
#error "MCP_TEST_STDIO_SERVER must name the stdio test server executable"
#endif
#ifndef MCP_SOURCE_DIR
#error "MCP_SOURCE_DIR must name the source tree"
#endif

namespace client_sync_test {

using nlohmann::json;

mcp::Endpoint test_server() {
    mcp::StdioProcessOptions process;
    process.command = MCP_TEST_STDIO_SERVER;
    process.arguments = {"serve", "marker"};
    process.stderr_mode = mcp::StderrMode::eDiscard;
    return mcp::Endpoint::stdio(std::move(process));
}

bool mentions(const mcp::CallToolResult& result, const std::string& text) {
    return result.structuredContent && result.structuredContent->dump().find(text) != std::string::npos;
}

TEST(ClientSyncTest, BlockingCallsReachTheServer) {
    mcp::Client client(test_server());
    const auto connected = client.sync().connect("sync-test", "1.0");
    EXPECT_FALSE(connected.protocolVersion.empty());

    const auto tools = client.sync().list_tools();
    std::set<std::string> names;
    for (const auto& tool : tools.tools) {
        names.insert(tool.name);
    }
    EXPECT_TRUE(names.contains("args"));

    const auto result = client.sync().call_tool("args", json::object());
    EXPECT_FALSE(result.isError.value_or(false));
    EXPECT_TRUE(mentions(result, "marker"));
}

TEST(ClientSyncTest, RunAwaitsACoroutineThatUsesTheClient) {
    mcp::Client client(test_server());
    mcp::CallToolResult result;
    std::string tool_name = "args";
    client.run([&]() -> mcp::Task<void> {
        co_await client.connect("sync-test", "1.0");
        // The lambda's captures are read after the coroutine has suspended above.
        result = co_await client.call_tool(tool_name, json::object());
    });
    EXPECT_TRUE(mentions(result, "marker"));
}

TEST(ClientSyncTest, RunRethrowsTheCoroutinesException) {
    mcp::Client client(test_server());
    EXPECT_THROW(client.run([&]() -> mcp::Task<void> {
        co_await client.connect("sync-test", "1.0");
        throw std::runtime_error("from the coroutine");
    }),
                 std::runtime_error);
}

TEST(ClientSyncTest, RunRefusesAnEmptyTask) {
    mcp::Client client(test_server());
    EXPECT_THROW(client.run([]() { return mcp::Task<void>(); }), std::invalid_argument);
}

TEST(ClientSyncTest, BlockingCallsRunAndCoroutinesMix) {
    mcp::Client client(test_server());
    client.sync().connect("sync-test", "1.0");
    bool awaited = false;
    client.run([&]() -> mcp::Task<void> {
        const auto result = co_await client.call_tool("args", json::object());
        awaited = mentions(result, "marker");
    });
    EXPECT_TRUE(awaited);
    EXPECT_TRUE(mentions(client.sync().call_tool("args", json::object()), "marker"));
}

TEST(ClientSyncTest, ABlockingCallOnTheLoopThreadThrows) {
    mcp::Client client(test_server());
    client.sync().connect("sync-test", "1.0");
    bool refused = false;
    bool run_refused = false;
    client.run([&]() -> mcp::Task<void> {
        try {
            static_cast<void>(client.sync().list_tools());
        } catch (const std::logic_error&) {
            refused = true;
        }
        try {
            client.run([]() -> mcp::Task<void> { co_return; });
        } catch (const std::logic_error&) {
            run_refused = true;
        }
        static_cast<void>(co_await client.list_tools());
    });
    EXPECT_TRUE(refused);
    EXPECT_TRUE(run_refused);
}

TEST(ClientSyncTest, AClientOnTheApplicationsExecutorRefusesBlockingCalls) {
    boost::asio::io_context io_ctx;
    auto [client_end, server_end] = mcp::make_memory_transport_pair(io_ctx.get_executor());
    mcp::Client client(client_end, io_ctx.get_executor());
    EXPECT_THROW(static_cast<void>(client.sync()), std::logic_error);
    EXPECT_THROW(client.run([]() -> mcp::Task<void> { co_return; }), std::logic_error);
}

TEST(ClientSyncTest, ConcurrentCallersEachGetTheirResult) {
    mcp::Client client(test_server());
    client.sync().connect("sync-test", "1.0");
    std::atomic<int> answered{0};
    std::vector<std::thread> callers;
    for (int i = 0; i < 8; ++i) {
        callers.emplace_back([&] {
            if (mentions(client.sync().call_tool("args", json::object()), "marker")) {
                ++answered;
            }
        });
    }
    for (auto& caller : callers) {
        caller.join();
    }
    EXPECT_EQ(answered.load(), 8);
}

TEST(ClientSyncTest, ATimedOutCallThrowsMcpError) {
    mcp::Client client(test_server());
    client.sync().connect("sync-test", "1.0");
    mcp::RequestOptions options;
    options.timeout = std::chrono::milliseconds(200);
    try {
        static_cast<void>(client.sync().call_tool("hold", json::object(), options));
        ADD_FAILURE() << "the call did not time out";
    } catch (const mcp::McpError& error) {
        EXPECT_EQ(error.code(), mcp::constants::jsonrpc::g_REQUEST_TIMEOUT);
    }
}

// A blocking call made on a handle while its Client is being destroyed returns, with the error of
// a destroyed client, instead of waiting for a loop that no longer runs.
TEST(ClientSyncTest, ABlockingCallDuringTheClientsDestructionReturns) {
    for (int round = 0; round < 3; ++round) {
        auto client = std::make_unique<mcp::Client>(test_server());
        client->sync().connect("sync-test", "1.0");
        mcp::SubscriptionFilter filter;
        filter.toolsListChanged = true;
        mcp::Subscription stream = client->sync().listen(filter);
        auto caller = std::async(std::launch::async, [&stream] {
            for (;;) {
                try {
                    stream.sync().close();
                } catch (const std::exception&) {
                    return;
                }
            }
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(2 + round));
        client.reset();
        EXPECT_EQ(caller.wait_for(std::chrono::seconds(20)), std::future_status::ready)
            << "round " << round;
    }
}

TEST(ClientSyncTest, SubscriptionAndTaskHandleViews) {
    mcp::Client client(test_server());
    client.sync().connect("sync-test", "1.0");
    mcp::SubscriptionFilter filter;
    filter.toolsListChanged = true;
    auto subscription = client.sync().listen(filter);
    subscription.sync().close();
    EXPECT_TRUE(subscription.end_reason().has_value());

    EXPECT_THROW(static_cast<void>(mcp::TaskHandle().sync()), mcp::McpError);
}

/// The names declared in the public part of `class MCP_API <name> {` in @p header, followed by
/// `(`, for members whose declaration begins with a Task or `auto` return (coroutine members), or
/// for every member when @p coroutines_only is false.
std::set<std::string> member_names(const std::string& header, const std::string& name,
                                   bool coroutines_only) {
    const auto start = header.find("class MCP_API " + name + " {");
    EXPECT_NE(start, std::string::npos) << name;
    const auto end = header.find("\n};", start);
    auto body = header.substr(start, end - start);
    const auto private_part = body.find("\n   private:");
    if (private_part != std::string::npos) {
        body = body.substr(0, private_part);
    }
    std::set<std::string> names;
    const std::regex coroutine(R"((?:Task<[^;{]*?>|auto)\s+(\w+)\()");
    const std::regex any(R"((\w+)\()");
    const auto& pattern = coroutines_only ? coroutine : any;
    for (std::sregex_iterator it(body.begin(), body.end(), pattern), last; it != last; ++it) {
        names.insert((*it)[1].str());
    }
    return names;
}

TEST(ClientSyncTest, RequestProgressReachesTheRequestsCallback) {
    for (const bool legacy : {false, true}) {
        SCOPED_TRACE(legacy ? "legacy session" : "modern session");
        mcp::ClientOptions options;
        if (legacy) {
            options.protocol = mcp::ClientProtocol::legacy();
        }
        mcp::Client client(test_server(), options);
        client.sync().connect("sync-test", "1.0");
        int unrouted = 0;
        client.on_progress([&](const mcp::ProgressNotificationParams&) { ++unrouted; });

        std::vector<double> seen;
        std::optional<double> total;
        mcp::RequestOptions request;
        request.on_progress = [&](const mcp::ProgressNotificationParams& progress) {
            seen.push_back(progress.progress);
            total = progress.total;
        };
        const auto result = client.sync().call_tool("progress", json{{"steps", 3}}, request);
        EXPECT_FALSE(result.isError.value_or(false));
        EXPECT_EQ(seen, (std::vector<double>{1, 2, 3}));
        EXPECT_EQ(total, 3.0);
        EXPECT_EQ(unrouted, 0) << "progress of a request with a callback reached Client::on_progress";
    }
}

TEST(ClientSyncTest, AProgressTokenTheParamsNameIsKept) {
    mcp::Client client(test_server());
    client.sync().connect("sync-test", "1.0");
    std::vector<std::string> routed;
    std::vector<std::string> unrouted;
    client.on_progress([&](const mcp::ProgressNotificationParams& progress) {
        unrouted.push_back(progress.progressToken.correlation_key());
    });
    mcp::RequestOptions request;
    request.on_progress = [&](const mcp::ProgressNotificationParams& progress) {
        routed.push_back(progress.progressToken.correlation_key());
    };
    const std::optional<json> call{json{
        {"name", "progress"}, {"arguments", {{"steps", 2}}}, {"_meta", {{"progressToken", "mine"}}}}};
    static_cast<void>(client.sync().send_request("tools/call", call, request));
    EXPECT_EQ(routed, (std::vector<std::string>(2, mcp::ProgressToken{"mine"}.correlation_key())));
    EXPECT_TRUE(unrouted.empty());

    // Without a callback the same notifications go to Client::on_progress, as before.
    static_cast<void>(client.sync().send_request("tools/call", call));
    EXPECT_EQ(unrouted, (std::vector<std::string>(2, mcp::ProgressToken{"mine"}.correlation_key())));
    EXPECT_EQ(routed.size(), 2U);
}

TEST(ClientSyncTest, AProgressTokenInUseIsRefused) {
    mcp::Client client(test_server());
    client.sync().connect("sync-test", "1.0");
    const std::optional<json> call{json{{"name", "progress"},
                                        {"arguments", {{"steps", 1}, {"wait_ms", 600000}}},
                                        {"_meta", {{"progressToken", "shared"}}}}};
    std::promise<void> reported;
    auto first_report = reported.get_future();
    std::once_flag once;
    mcp::CancellationToken cancel;
    mcp::RequestOptions first;
    first.cancellation = cancel;
    first.on_progress = [&](const mcp::ProgressNotificationParams&) {
        std::call_once(once, [&] { reported.set_value(); });
    };
    bool first_cancelled = false;
    std::thread caller([&] {
        try {
            static_cast<void>(client.sync().send_request("tools/call", call, first));
        } catch (const mcp::McpError& error) {
            first_cancelled = error.code() == mcp::constants::jsonrpc::g_REQUEST_CANCELLED;
        }
    });
    first_report.wait();

    mcp::RequestOptions second;
    second.on_progress = [](const mcp::ProgressNotificationParams&) {};
    EXPECT_THROW(static_cast<void>(client.sync().send_request("tools/call", call, second)),
                 std::invalid_argument);
    cancel.cancel();
    caller.join();
    EXPECT_TRUE(first_cancelled);

    // The first call's end released the token.
    json short_call = *call;
    short_call["arguments"]["wait_ms"] = 0;
    int reports = 0;
    second.on_progress = [&](const mcp::ProgressNotificationParams&) { ++reports; };
    static_cast<void>(client.sync().send_request("tools/call", short_call, second));
    EXPECT_EQ(reports, 1);
}

// A token the client makes differs from one an application writes into another request, so the
// progress of each request reaches its own receiver.
TEST(ClientSyncTest, AMadeProgressTokenDiffersFromAWrittenOne) {
    mcp::Client client(test_server());
    client.sync().connect("sync-test", "1.0");
    std::mutex seen_mutex;
    std::vector<std::string> routed;
    std::vector<std::string> unrouted;
    client.on_progress([&](const mcp::ProgressNotificationParams& progress) {
        std::lock_guard lock(seen_mutex);
        unrouted.push_back(progress.progressToken.correlation_key());
    });

    std::promise<void> reported;
    auto first_report = reported.get_future();
    std::once_flag once;
    mcp::CancellationToken cancel;
    mcp::RequestOptions made;
    made.cancellation = cancel;
    made.on_progress = [&](const mcp::ProgressNotificationParams& progress) {
        {
            std::lock_guard lock(seen_mutex);
            routed.push_back(progress.progressToken.correlation_key());
        }
        std::call_once(once, [&] { reported.set_value(); });
    };
    std::thread caller([&] {
        try {
            static_cast<void>(
                client.sync().call_tool("progress", json{{"steps", 1}, {"wait_ms", 600000}}, made));
        } catch (const mcp::McpError&) {
        }
    });
    first_report.wait();

    for (const std::int64_t written : {1, 2}) {
        const std::optional<json> call{json{{"name", "progress"},
                                            {"arguments", {{"steps", 1}}},
                                            {"_meta", {{"progressToken", written}}}}};
        static_cast<void>(client.sync().send_request("tools/call", call));
    }
    cancel.cancel();
    caller.join();

    std::lock_guard lock(seen_mutex);
    ASSERT_EQ(routed.size(), 1U);
    EXPECT_EQ(routed.front().rfind("s:", 0), 0U) << routed.front();
    EXPECT_EQ(unrouted,
              (std::vector<std::string>{mcp::ProgressToken{std::int64_t{1}}.correlation_key(),
                                        mcp::ProgressToken{std::int64_t{2}}.correlation_key()}));
}

// `{}` makes null arguments, which are sent as an empty object, by call_tool() and start_tool().
TEST(ClientSyncTest, NullToolArgumentsAreSentAsAnEmptyObject) {
    mcp::Client client(test_server());
    client.sync().connect("sync-test", "1.0");
    EXPECT_TRUE(mentions(client.sync().call_tool("args", {}), "marker"));
    const auto started = client.sync().start_tool("args", {});
    ASSERT_TRUE(std::holds_alternative<mcp::CallToolResult>(started));
    EXPECT_TRUE(mentions(std::get<mcp::CallToolResult>(started), "marker"));

    // The coroutine start_tool() takes JSON arguments as call_tool() does.
    std::optional<mcp::CallToolResult> from_coroutine;
    client.run([&]() -> mcp::Task<void> {
        const json arguments = json::object();
        auto result = co_await client.start_tool("args", arguments);
        if (auto* answered = std::get_if<mcp::CallToolResult>(&result)) {
            from_coroutine = std::move(*answered);
        }
    });
    ASSERT_TRUE(from_coroutine.has_value());
    EXPECT_TRUE(mentions(*from_coroutine, "marker"));
}

TEST(ClientSyncTest, EveryRequestOptionsOverloadSendsItsRequest) {
    mcp::Client client(test_server());
    client.sync().connect("sync-test", "1.0");
    auto sync = client.sync();
    mcp::RequestOptions request;
    request.timeout = std::chrono::seconds(30);

    EXPECT_FALSE(sync.list_tools(std::nullopt, request).tools.empty());
    EXPECT_EQ(sync.list_resources(std::nullopt, request).resources.size(), 1U);
    EXPECT_TRUE(sync.list_resource_templates(std::nullopt, request).resourceTemplates.empty());
    EXPECT_EQ(sync.list_prompts(std::nullopt, request).prompts.size(), 1U);
    EXPECT_EQ(sync.read_resource("test://greeting", request).contents.size(), 1U);
    const auto prompt = sync.get_prompt("greet", {{"name", "Ada"}}, request);
    ASSERT_EQ(prompt.messages.size(), 1U);
    ASSERT_TRUE(std::holds_alternative<mcp::TextContent>(prompt.messages.front().content));
    EXPECT_EQ(std::get<mcp::TextContent>(prompt.messages.front().content).text, "Hello, Ada");

    mcp::CompleteParams complete;
    mcp::PromptReference greet;
    greet.name = "greet";
    complete.ref = greet;
    complete.argument.name = "name";
    complete.argument.value = "a";
    EXPECT_EQ(sync.complete(complete, request).completion.values, (std::vector<std::string>{"alpha"}));

    int reports = 0;
    request.on_progress = [&](const mcp::ProgressNotificationParams&) { ++reports; };
    const auto started = sync.start_tool("progress", json{{"steps", 2}}, request);
    ASSERT_TRUE(std::holds_alternative<mcp::CallToolResult>(started));
    EXPECT_EQ(reports, 2);
}

TEST(ClientSyncTest, EveryCoroutineMemberHasABlockingTwin) {
    std::string header;
    for (const char* name : {"client.hpp", "subscription.hpp", "sync_client.hpp", "task_handle.hpp"}) {
        std::ifstream file(std::string(MCP_SOURCE_DIR) + "/include/mcp/client/" + name);
        ASSERT_TRUE(file) << name << " not found";
        std::stringstream text;
        text << file.rdbuf();
        header += text.str();
    }
    for (const auto& [coroutine_class, sync_class] :
         std::vector<std::pair<std::string, std::string>>{{"Client", "SyncClient"},
                                                          {"TaskHandle", "SyncTaskHandle"},
                                                          {"Subscription", "SyncSubscription"}}) {
        const auto coroutines = member_names(header, coroutine_class, true);
        const auto twins = member_names(header, sync_class, false);
        EXPECT_FALSE(coroutines.empty()) << coroutine_class;
        for (const auto& member : coroutines) {
            EXPECT_TRUE(twins.contains(member)) << sync_class << " has no " << member;
        }
    }
}

// A handler of a server that runs run_stdio() cannot wait for a notification on the event loop it
// runs on, and stop() from a handler ends run_stdio(), and with it the server process.
TEST(ClientSyncTest, AStdioServerRefusesABlockingNotifyFromItsHandlerAndStopsFromOne) {
    mcp::Client client(test_server());
    static_cast<void>(client.sync().connect("sync-test", "1.0"));
    EXPECT_EQ(mcp::result_text(client.sync().call_tool("notify_from_handler", json::object())),
              "refused");

    try {
        static_cast<void>(client.sync().call_tool("stop", json::object()));
    } catch (const std::exception&) {
        // The transport may close before the answer is written.
    }
    // The server process has ended: the next request fails with the connection, not by timing out.
    bool failed = false;
    bool timed_out = false;
    try {
        static_cast<void>(client.sync().list_tools());
    } catch (const mcp::McpError& error) {
        failed = true;
        timed_out = error.code() == mcp::constants::jsonrpc::g_REQUEST_TIMEOUT;
    } catch (const std::exception&) {
        failed = true;
    }
    EXPECT_TRUE(failed);
    EXPECT_FALSE(timed_out);
}

}  // namespace client_sync_test

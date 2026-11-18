#include "mcp/client/client.hpp"
#include "mcp/core/async.hpp"
#include "mcp/core/constants.hpp"
#include "mcp/transport/memory.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <boost/asio/io_context.hpp>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
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
    auto [client_end, server_end] = mcp::create_memory_transport_pair(io_ctx.get_executor());
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

TEST(ClientSyncTest, EveryCoroutineMemberHasABlockingTwin) {
    std::ifstream file(std::string(MCP_SOURCE_DIR) + "/include/mcp/client/client.hpp");
    ASSERT_TRUE(file) << "client.hpp not found";
    std::stringstream text;
    text << file.rdbuf();
    const auto header = text.str();
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

}  // namespace client_sync_test

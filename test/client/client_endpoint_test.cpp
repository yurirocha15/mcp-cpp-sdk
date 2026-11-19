#include "mcp/auth/oauth.hpp"
#include "mcp/client/client.hpp"
#include "mcp/core/async.hpp"
#include "mcp/transport/http_client.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/use_future.hpp>
#include <chrono>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifndef MCP_TEST_STDIO_SERVER
#error "MCP_TEST_STDIO_SERVER must name the stdio test server executable"
#endif

namespace client_endpoint_test {

using nlohmann::json;

class FixedTokenAuthenticator final : public mcp::auth::Authenticator {
   public:
    [[nodiscard]] std::string get_access_token() const override { return "token"; }
    mcp::Task<bool> try_refresh_token() override { co_return false; }
};

mcp::StdioProcessOptions test_server(std::vector<std::string> arguments = {"serve"}) {
    mcp::StdioProcessOptions process;
    process.command = MCP_TEST_STDIO_SERVER;
    process.arguments = std::move(arguments);
    process.stderr_mode = mcp::StderrMode::eDiscard;
    return process;
}

TEST(ClientEndpointTest, RefusesAnEmptyCommandOrFactory) {
    EXPECT_THROW(static_cast<void>(mcp::Endpoint::stdio(std::vector<std::string>{})),
                 std::invalid_argument);
    EXPECT_THROW(static_cast<void>(mcp::Endpoint::stdio(mcp::StdioProcessOptions{})),
                 std::invalid_argument);
    EXPECT_THROW(static_cast<void>(mcp::Endpoint::stdio({})), std::invalid_argument);
    EXPECT_THROW(static_cast<void>(mcp::Endpoint::custom({})), std::invalid_argument);
    const auto null_transport = mcp::Endpoint::custom(
        [](const mcp::Executor&) -> std::shared_ptr<mcp::ITransport> { return nullptr; });
    EXPECT_THROW(mcp::Client client(null_transport), std::invalid_argument);
}

TEST(ClientEndpointTest, LaunchesTheServerAndCallsATool) {
    mcp::Client client(mcp::Endpoint::stdio(test_server({"serve", "first-argument"})));
    const auto result = boost::asio::co_spawn(
                            client.executor().get(),
                            [&client]() -> boost::asio::awaitable<mcp::CallToolResult> {
                                co_await client.connect("endpoint-test", "1.0");
                                co_return co_await client.call_tool("args", json::object());
                            },
                            boost::asio::use_future)
                            .get();
    ASSERT_FALSE(result.isError.value_or(false));
    ASSERT_TRUE(result.structuredContent.has_value());
    const auto arguments = result.structuredContent->dump();
    EXPECT_NE(arguments.find("first-argument"), std::string::npos) << arguments;
}

TEST(ClientEndpointTest, CommandVectorNamesTheProgramAndItsArguments) {
    mcp::Client client(
        mcp::Endpoint::stdio(std::vector<std::string>{MCP_TEST_STDIO_SERVER, "serve", "second"}));
    const auto result = boost::asio::co_spawn(
                            client.executor().get(),
                            [&client]() -> boost::asio::awaitable<mcp::CallToolResult> {
                                co_await client.connect("endpoint-test", "1.0");
                                co_return co_await client.call_tool("args", json::object());
                            },
                            boost::asio::use_future)
                            .get();
    ASSERT_TRUE(result.structuredContent.has_value());
    EXPECT_NE(result.structuredContent->dump().find("second"), std::string::npos);
}

// A command written in braces is the program and its arguments, also when it is the program alone.
TEST(ClientEndpointTest, BracedCommandNamesTheProgramAndItsArguments) {
    const std::vector<std::pair<mcp::Endpoint, json>> cases = {
        {mcp::Endpoint::stdio({MCP_TEST_STDIO_SERVER}), json::array()},
        {mcp::Endpoint::stdio({MCP_TEST_STDIO_SERVER, "serve", "third"}), json{"serve", "third"}}};
    for (const auto& [endpoint, arguments] : cases) {
        mcp::Client client(endpoint);
        client.sync().connect("endpoint-test", "1.0");
        const auto result = client.sync().call_tool("args", json::object());
        ASSERT_TRUE(result.structuredContent.has_value());
        EXPECT_EQ(result.structuredContent->at("args"), arguments);
    }
}

/// The code a blocking call failed with, or 0 when it succeeded.
template <typename Call>
int failure_code(Call call) {
    try {
        call();
    } catch (const mcp::McpError& error) {
        return error.code();
    }
    return 0;
}

json crash(int status) { return json{{"status", status}}; }

// A server that ends on its own is started again, and the same Client, handlers and blocking view
// go on with it; a legacy session initializes the new process first.
TEST(ClientEndpointTest, ARestartedServerServesTheSameClient) {
    for (const bool legacy : {false, true}) {
        SCOPED_TRACE(legacy ? "legacy session" : "modern session");
        mcp::ClientOptions options;
        if (legacy) {
            options.protocol = mcp::ClientProtocol::legacy();
        }
        std::atomic<int> ended_alone{0};
        auto process = test_server({"serve", "restarted"});
        process.on_exit = [&](const mcp::ProcessExit& exit) {
            if (!exit.requested) {
                ++ended_alone;
            }
        };
        mcp::Client client(
            mcp::Endpoint::stdio(process, {.max_restarts = 2, .delay = std::chrono::milliseconds(50)}),
            options);
        const auto connected = client.sync().connect("endpoint-test", "1.0");
        EXPECT_EQ(connected.era, legacy ? mcp::ProtocolEra::eLegacy : mcp::ProtocolEra::eModern);

        EXPECT_EQ(failure_code([&] { client.sync().call_tool("crash", crash(3)); }),
                  mcp::constants::jsonrpc::g_CONNECTION_CLOSED);
        // Made while the server starts again, this call waits for it.
        const auto result = client.sync().call_tool("args", json::object());
        ASSERT_TRUE(result.structuredContent.has_value());
        EXPECT_EQ(result.structuredContent->at("args"), (json{"serve", "restarted"}));
        EXPECT_EQ(ended_alone.load(), 1);

        EXPECT_EQ(failure_code([&] { client.sync().call_tool("crash", crash(4)); }),
                  mcp::constants::jsonrpc::g_CONNECTION_CLOSED);
        EXPECT_FALSE(client.sync().list_tools().tools.empty());
    }
}

// Past the last start the policy allows, the client closes as it does without one.
TEST(ClientEndpointTest, TheClientClosesWhenThePolicyAllowsNoMoreStarts) {
    mcp::Client client(mcp::Endpoint::stdio(
        test_server(), {.max_restarts = 1, .delay = std::chrono::milliseconds(10)}));
    client.sync().connect("endpoint-test", "1.0");
    EXPECT_EQ(failure_code([&] { client.sync().call_tool("crash", crash(3)); }),
              mcp::constants::jsonrpc::g_CONNECTION_CLOSED);
    EXPECT_EQ(failure_code([&] { client.sync().list_tools(); }), 0);
    EXPECT_EQ(failure_code([&] { client.sync().call_tool("crash", crash(3)); }),
              mcp::constants::jsonrpc::g_CONNECTION_CLOSED);
    EXPECT_EQ(failure_code([&] { client.sync().list_tools(); }),
              mcp::constants::jsonrpc::g_CONNECTION_CLOSED);
}

// A server that exits with status 0 is shutting down by choice, unless the policy says otherwise.
TEST(ClientEndpointTest, ACleanExitRestartsOnlyWhenThePolicySaysSo) {
    for (const bool after_clean_exit : {false, true}) {
        SCOPED_TRACE(after_clean_exit ? "restart_after_clean_exit" : "default policy");
        mcp::StdioRestartPolicy policy{.max_restarts = 1, .delay = std::chrono::milliseconds(10)};
        policy.restart_after_clean_exit = after_clean_exit;
        mcp::Client client(mcp::Endpoint::stdio(test_server(), policy));
        client.sync().connect("endpoint-test", "1.0");
        EXPECT_EQ(failure_code([&] { client.sync().call_tool("crash", crash(0)); }),
                  mcp::constants::jsonrpc::g_CONNECTION_CLOSED);
        EXPECT_EQ(failure_code([&] { client.sync().list_tools(); }),
                  after_clean_exit ? 0 : mcp::constants::jsonrpc::g_CONNECTION_CLOSED);
    }
}

// A subscriptions/listen stream does not carry over to the new process.
TEST(ClientEndpointTest, AListenStreamEndsWhenTheServerIsRestarted) {
    mcp::Client client(mcp::Endpoint::stdio(
        test_server(), {.max_restarts = 1, .delay = std::chrono::milliseconds(10)}));
    client.sync().connect("endpoint-test", "1.0");
    mcp::SubscriptionFilter filter;
    filter.toolsListChanged = true;
    auto subscription = client.sync().listen(filter);
    EXPECT_EQ(failure_code([&] { client.sync().call_tool("crash", crash(3)); }),
              mcp::constants::jsonrpc::g_CONNECTION_CLOSED);
    EXPECT_FALSE(subscription.sync().next().has_value());
    ASSERT_TRUE(subscription.end_error().has_value());
    EXPECT_EQ(subscription.end_error()->code, mcp::constants::jsonrpc::g_CONNECTION_CLOSED);
    EXPECT_FALSE(client.sync().list_tools().tools.empty());
}

// A subscription and a task handle may outlive their client: their blocking calls then fail as a
// closed connection does, and destroying them afterwards is safe.
TEST(ClientEndpointTest, HandlesThatOutliveTheClientRefuseBlockingCalls) {
    std::optional<mcp::Subscription> subscription;
    std::optional<mcp::TaskHandle> handle;
    {
        mcp::Client client(mcp::Endpoint::stdio(test_server()));
        client.sync().connect("endpoint-test", "1.0");
        mcp::SubscriptionFilter filter;
        filter.toolsListChanged = true;
        subscription = client.sync().listen(filter);
        handle = client.task_handle("task-of-a-closed-client");
    }
    EXPECT_EQ(failure_code([&] { static_cast<void>(subscription->sync().next()); }),
              mcp::constants::jsonrpc::g_CONNECTION_CLOSED);
    EXPECT_EQ(failure_code([&] { static_cast<void>(handle->sync().get()); }),
              mcp::constants::jsonrpc::g_CONNECTION_CLOSED);
    subscription.reset();
    handle.reset();
}

TEST(ClientEndpointTest, HttpEndpointConfiguresTheTransportAndMakesTheAuthenticator) {
    bool configured = false;
    bool executor_given = false;
    mcp::HttpEndpointOptions options;
    options.configure = [&configured](mcp::HttpClientTransport& transport) {
        transport.set_response_timeout(std::chrono::seconds(5));
        configured = true;
    };
    options.authenticator = [&executor_given](const mcp::Executor& executor) {
        executor_given = static_cast<bool>(executor);
        return std::make_shared<FixedTokenAuthenticator>();
    };
    mcp::Client client(mcp::Endpoint::http("http://127.0.0.1:9/mcp", std::move(options)));
    EXPECT_TRUE(configured);
    EXPECT_TRUE(executor_given);
}

TEST(ClientEndpointTest, DestructionEndsACallInFlight) {
    std::future<mcp::CallToolResult> held;
    {
        mcp::Client client(mcp::Endpoint::stdio(test_server()));
        boost::asio::co_spawn(
            client.executor().get(),
            [&client]() -> boost::asio::awaitable<void> {
                co_await client.connect("endpoint-test", "1.0");
            },
            boost::asio::use_future)
            .get();
        held = boost::asio::co_spawn(
            client.executor().get(),
            [&client]() -> boost::asio::awaitable<mcp::CallToolResult> {
                co_return co_await client.call_tool("hold", json::object());
            },
            boost::asio::use_future);
        while (client.pending_request_count() == 0) {
            std::this_thread::yield();
        }
    }
    EXPECT_THROW(static_cast<void>(held.get()), std::exception);
}

}  // namespace client_endpoint_test

#include <gtest/gtest.h>

#include "../test_utils.hpp"

#include "../support/spawn.hpp"
#include "mcp/client/client.hpp"
#include "mcp/server/server.hpp"
#include "mcp/transport/memory.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <chrono>
#include <string>

namespace e2e_features_test {

class E2EFeaturesTest : public ::testing::Test {
   protected:
    boost::asio::io_context io_ctx_;
};

// One session over a memory transport pair: connect, ping, list a page of tools, call several
// tools and one tool several times, then shut down.
TEST_F(E2EFeaturesTest, OneSessionConnectsPingsListsAndCallsTools) {
    bool test_passed = false;

    auto [client_transport, server_transport] =
        mcp::create_memory_transport_pair(io_ctx_.get_executor());

    mcp::Server server({"test-server", "1.0"}, {});
    server.set_page_size(3);
    const auto text_tool = [&server](const std::string& name, const std::string& text) {
        server.add_tool(name, "Answers " + text, {{"type", "object"}}, [text](const nlohmann::json&) {
            return nlohmann::json{
                {"content", nlohmann::json::array({nlohmann::json{{"type", "text"}, {"text", text}}})}};
        });
    };
    text_tool("add", "3");
    text_tool("multiply", "6");
    text_tool("echo", "echo");
    // More tools than fit on one page.
    for (int i = 0; i < 7; ++i) {
        text_tool("tool_" + std::to_string(i), "ok");
    }

    mcp_test::spawn_detached(io_ctx_, [&]() -> mcp::Task<void> {
        mcp_test::spawn_detached(io_ctx_, [&]() -> mcp::Task<void> {
            try {
                co_await server.run(server_transport, io_ctx_.get_executor());
            } catch (...) {
            }
        });

        mcp::Client client(client_transport, io_ctx_.get_executor(), legacy_client_options());
        auto init_res = co_await client.connect("test-client", "1.0");
        EXPECT_EQ(init_res.serverInfo->name, "test-server");

        co_await client.ping();

        auto page = co_await client.list_tools();
        EXPECT_GE(page.tools.size(), 2);
        EXPECT_LE(page.tools.size(), 3);

        auto add_result = co_await client.call_tool("add", nlohmann::json::object());
        EXPECT_GT(add_result.content.size(), 0);
        auto mul_result = co_await client.call_tool("multiply", nlohmann::json::object());
        EXPECT_GT(mul_result.content.size(), 0);

        for (int call = 0; call < 3; ++call) {
            auto echoed = co_await client.call_tool("echo", nlohmann::json::object());
            EXPECT_GT(echoed.content.size(), 0) << "call " << call;
        }

        co_await client.send_request("shutdown", std::nullopt);
        client.close();
        test_passed = true;
    });

    io_ctx_.run();
    EXPECT_TRUE(test_passed);
}

}  // namespace e2e_features_test

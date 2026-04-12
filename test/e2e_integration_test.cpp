#include <gtest/gtest.h>

#include "mcp/client.hpp"
#include "mcp/server.hpp"
#include "mcp/transport/memory.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <string>

class E2EIntegrationTest : public ::testing::Test {
   protected:
    void SetUp() override {
        auto [client_transport, server_transport] =
            mcp::create_memory_transport_pair(io_ctx_.get_executor());

        mcp::Implementation server_info{"test-server", "1.0"};
        mcp::ServerCapabilities server_caps;
        server_caps.tools = mcp::ServerCapabilities::ToolsCapability{};
        server_ = std::make_unique<mcp::Server>(std::move(server_info), std::move(server_caps));

        mcp::Implementation client_info{"test-client", "1.0"};
        mcp::ClientCapabilities client_caps;

        client_info_ = std::move(client_info);
        client_caps_ = std::move(client_caps);

        client_ = std::make_unique<mcp::Client>(std::move(client_transport), io_ctx_.get_executor());
        server_transport_ = std::move(server_transport);
    }

    boost::asio::io_context io_ctx_;
    std::unique_ptr<mcp::Server> server_;
    std::unique_ptr<mcp::Client> client_;
    std::unique_ptr<mcp::ITransport> server_transport_;
    mcp::Implementation client_info_;
    mcp::ClientCapabilities client_caps_;
};

TEST_F(E2EIntegrationTest, InitializeAndPingSession) {
    bool test_passed = false;

    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            // Run server loop in background
            boost::asio::co_spawn(
                io_ctx_,
                [&]() -> mcp::Task<void> {
                    try {
                        co_await server_->run(std::move(server_transport_), io_ctx_.get_executor());
                    } catch (...) {
                    }
                },
                boost::asio::detached);

            // Initialize client
            auto init_res = co_await client_->connect(client_info_, client_caps_);
            EXPECT_EQ(init_res.serverInfo.name, "test-server");

            // Ping the server
            co_await client_->ping();

            co_await client_->send_request("shutdown", std::nullopt);
            client_->close();

            test_passed = true;
        },
        boost::asio::detached);

    io_ctx_.run();
    EXPECT_TRUE(test_passed);
}

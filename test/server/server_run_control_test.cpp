#include "../support/hang_guard.hpp"
#include "../support/spawn.hpp"

#include "mcp/client/client.hpp"
#include "mcp/core/async.hpp"
#include "mcp/server/server.hpp"
#include "mcp/transport/memory.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <boost/asio/io_context.hpp>
#include <chrono>
#include <functional>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <thread>

namespace server_run_control_test {

namespace {

/// Runs an io_context on a thread of its own, stopping and joining it however the test ends.
class LoopThread {
   public:
    explicit LoopThread(boost::asio::io_context& io_ctx)
        : io_ctx_(io_ctx), work_(io_ctx.get_executor()), thread_([&io_ctx] { io_ctx.run(); }) {}
    LoopThread(const LoopThread&) = delete;
    LoopThread& operator=(const LoopThread&) = delete;
    LoopThread(LoopThread&&) = delete;
    LoopThread& operator=(LoopThread&&) = delete;
    ~LoopThread() {
        io_ctx_.stop();
        thread_.join();
    }

   private:
    boost::asio::io_context& io_ctx_;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work_;
    std::thread thread_;
};

}  // namespace

TEST(ServerRunControlTest, StopAndSyncDoNothingWhileTheServerIsNotRunning) {
    mcp::Server server("idle", "1.0");
    EXPECT_NO_THROW(server.stop());
    EXPECT_NO_THROW(server.sync().notify_tools_list_changed());
    EXPECT_NO_THROW(server.sync().notify_resources_list_changed());
    EXPECT_NO_THROW(server.sync().notify_prompts_list_changed());
    EXPECT_NO_THROW(server.sync().notify_resource_updated("file:///notes.txt"));
    EXPECT_EQ(server.bound_port(), 0);
}

// stop() from another thread closes the transport of run(), whose task then completes; sync()
// cannot wait on the application's executor and says so.
TEST(ServerRunControlTest, StopEndsRunAndSyncRefusesTheApplicationsExecutor) {
    mcp::Server server("run-control", "1.0");
    server.add_tool("echo", "Echoes", nlohmann::json{{"type", "object"}},
                    [](const nlohmann::json& args) { return args; });

    boost::asio::io_context io_ctx;
    auto [client_transport, server_transport] =
        mcp::create_memory_transport_pair(io_ctx.get_executor());
    std::atomic<bool> connected{false};
    std::atomic<bool> run_ended{false};
    mcp_test::spawn_detached(io_ctx, [&]() -> mcp::Task<void> {
        co_await server.run(server_transport, io_ctx.get_executor());
        run_ended = true;
    });
    mcp::ClientOptions options;
    options.protocol = mcp::ClientProtocol::legacy();
    mcp::Client client(client_transport, io_ctx.get_executor(), options);
    mcp_test::spawn_detached(io_ctx, [&]() -> mcp::Task<void> {
        static_cast<void>(co_await client.connect("run-control-client", "1.0"));
        connected = true;
    });
    const LoopThread loop(io_ctx);

    ASSERT_TRUE(eventually([&] { return connected.load(); }));
    EXPECT_THROW(server.sync().notify_tools_list_changed(), std::logic_error);
    EXPECT_FALSE(run_ended.load());
    server.stop();
    EXPECT_TRUE(eventually([&] { return run_ended.load(); }));
    client.close();
}

// stop() from a handler, on the run's own executor, ends the run as well.
TEST(ServerRunControlTest, StopFromAHandlerEndsRun) {
    mcp::Server server("run-control", "1.0");
    server.add_tool("stop", "Stops the server", nlohmann::json{{"type", "object"}},
                    [&server](const nlohmann::json&) -> nlohmann::json {
                        server.stop();
                        return "stopping";
                    });

    boost::asio::io_context io_ctx;
    auto [client_transport, server_transport] =
        mcp::create_memory_transport_pair(io_ctx.get_executor());
    std::atomic<bool> run_ended{false};
    mcp_test::spawn_detached(io_ctx, [&]() -> mcp::Task<void> {
        co_await server.run(server_transport, io_ctx.get_executor());
        run_ended = true;
    });
    mcp::ClientOptions options;
    options.protocol = mcp::ClientProtocol::legacy();
    mcp::Client client(client_transport, io_ctx.get_executor(), options);
    mcp_test::spawn_detached(io_ctx, [&]() -> mcp::Task<void> {
        static_cast<void>(co_await client.connect("run-control-client", "1.0"));
        try {
            static_cast<void>(co_await client.call_tool("stop", nlohmann::json::object()));
        } catch (const std::exception&) {
            // The transport may close before the answer is written.
        }
    });
    const LoopThread loop(io_ctx);

    EXPECT_TRUE(eventually([&] { return run_ended.load(); }));
    client.close();
}

}  // namespace server_run_control_test

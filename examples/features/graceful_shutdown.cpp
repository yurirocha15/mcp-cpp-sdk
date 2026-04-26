/// @file graceful_shutdown.cpp
/// @brief Demonstrates graceful shutdown patterns in MCP server.
///
/// This example shows:
/// - Setting up signal handlers for SIGINT/SIGTERM
/// - Triggering graceful shutdown with mcp::detail::graceful_shutdown()
/// - Closing transport to stop accepting new connections
/// - Waiting for in-flight requests to complete (with timeout)
/// - Forcing shutdown if graceful timeout expires
/// - Using timer-based auto-exit for CI/testing (no actual signal waiting)
/// - Manual transport pattern (not run_stdio convenience)

#include <mcp/detail/signal.hpp>
#include <mcp/server.hpp>
#include <mcp/transport/stdio.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

namespace asio = boost::asio;

int main() {
    try {
        using namespace mcp;

        asio::io_context io_ctx;

        // ========== SERVER SETUP ==========
        ServerCapabilities server_caps;
        server_caps.tools = ServerCapabilities::ToolsCapability{};

        Implementation server_info;
        server_info.name = "graceful-shutdown-server";
        server_info.version = "1.0.0";

        Server server(server_info, server_caps);

        // ========== TOOL REGISTRATION ==========
        // Tool: quick_task — completes immediately
        nlohmann::json quick_schema = {{"type", "object"},
                                       {"properties", {{"value", {{"type", "integer"}}}}},
                                       {"required", nlohmann::json::array({"value"})}};

        server.add_tool("quick_task", "A quick task that completes immediately", quick_schema,
                        [](const nlohmann::json& args) -> nlohmann::json {
                            std::cout << "[Server] quick_task called\n";
                            int value = args.at("value").get<int>();
                            CallToolResult result;
                            TextContent content;
                            content.text = "Quick task result: " + std::to_string(value * 2);
                            result.content.emplace_back(std::move(content));
                            return nlohmann::json(result);
                        });

        // Tool: slow_task — simulates work with delay
        nlohmann::json slow_schema = {{"type", "object"},
                                      {"properties", {{"duration_ms", {{"type", "integer"}}}}},
                                      {"required", nlohmann::json::array({"duration_ms"})}};

        server.add_tool<nlohmann::json, nlohmann::json>(
            "slow_task", "A task that takes time to complete", slow_schema,
            [](nlohmann::json args) -> Task<nlohmann::json> {
                std::cout << "[Server] slow_task called\n";
                int duration_ms = args.at("duration_ms").get<int>();

                // Simulate work with async delay
                asio::steady_timer work_timer(
                    asio::get_associated_executor(co_await asio::this_coro::executor));
                work_timer.expires_after(std::chrono::milliseconds(duration_ms));
                co_await work_timer.async_wait(asio::use_awaitable);

                std::cout << "[Server] slow_task completed after " << duration_ms << "ms\n";
                CallToolResult result;
                TextContent content;
                content.text = "Slow task completed after " + std::to_string(duration_ms) + "ms";
                result.content.emplace_back(std::move(content));
                co_return nlohmann::json(result);
            });

        // ========== TRANSPORT SETUP ==========
        auto transport = std::make_shared<StdioTransport>(io_ctx.get_executor());

        // ========== SIGNAL HANDLING + GRACEFUL SHUTDOWN ==========
        auto signals = std::make_shared<asio::signal_set>(io_ctx, SIGINT, SIGTERM);
        std::shared_ptr<asio::steady_timer> shutdown_timer;

        asio::co_spawn(
            io_ctx,
            [&, signals]() -> Task<void> {
                std::cout << "[Main] Signal handler ready (waiting for SIGINT/SIGTERM)\n";
                auto signal_number = co_await signals->async_wait(asio::use_awaitable);
                std::cout << "[Main] Received signal " << signal_number
                          << ", initiating graceful shutdown\n";
                shutdown_timer = detail::graceful_shutdown(io_ctx, transport);
                std::cout << "[Main] Graceful shutdown initiated (timeout: "
                          << mcp::constants::g_shutdown_timeout_ms << "ms)\n";
            },
            asio::detached);

        // ========== AUTO-EXIT TIMER (for CI/testing) ==========
        asio::co_spawn(
            io_ctx,
            [&]() -> Task<void> {
                asio::steady_timer auto_trigger(io_ctx.get_executor());
                auto_trigger.expires_after(std::chrono::seconds(1));
                co_await auto_trigger.async_wait(asio::use_awaitable);
                std::cout << "[Main] Auto-triggering shutdown signal (for CI/testing)\n";
                detail::trigger_shutdown_signal();
            },
            asio::detached);

        // ========== SERVER COROUTINE ==========
        asio::co_spawn(
            io_ctx,
            [&, transport]() -> Task<void> {
                std::cout << "[Server] Starting\n";
                try {
                    co_await server.run(transport, io_ctx.get_executor());
                    std::cout << "[Server] Stopped normally\n";
                } catch (const std::exception& e) {
                    std::cout << "[Server] Stopped with exception: " << e.what() << "\n";
                }
            },
            asio::detached);

        // ========== RUN IO CONTEXT ==========
        std::cout << "[Main] Starting io_context\n";
        io_ctx.run();

        shutdown_timer.reset();
        std::cout << "[Main] io_context stopped, exiting normally\n";
        return EXIT_SUCCESS;

    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << '\n';
        return EXIT_FAILURE;
    }
}

/// @file progress_cancellation.cpp
/// @brief Demonstrates Context::report_progress() and Context::is_cancelled().
///
/// This example shows:
/// - A long-running tool that reports progress via ctx.report_progress()
/// - A client that subscribes to progress notifications
/// - A client that sends a cancellation request
/// - The tool handler checking ctx.is_cancelled() and exiting early

#include <mcp/client.hpp>
#include <mcp/server.hpp>
#include <mcp/transport/memory.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <cstdlib>
#include <exception>
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
        server_info.name = "progress-server";
        server_info.version = "1.0.0";

        Server server(server_info, server_caps);

        // ========== TOOL REGISTRATION ==========
        nlohmann::json tool_schema = {{"type", "object"},
                                      {"properties", {{"duration_ms", {{"type", "integer"}}}}},
                                      {"required", nlohmann::json::array({"duration_ms"})}};

        server.add_tool<nlohmann::json, nlohmann::json>(
            "long_task", "A long-running task that reports progress and can be cancelled", tool_schema,
            [](Context& ctx, const nlohmann::json& args) -> Task<nlohmann::json> {
                int duration_ms = args.at("duration_ms").get<int>();
                int steps = 10;
                int step_duration_ms = duration_ms / steps;

                std::cout << "Tool: Starting long_task for " << duration_ms << "ms\n";

                for (int i = 0; i < steps; ++i) {
                    if (ctx.is_cancelled()) {
                        std::cout << "Tool: Cancellation detected at step " << i << "\n";
                        co_return nlohmann::json{
                            {"content", nlohmann::json::array({nlohmann::json{
                                            {"type", "text"},
                                            {"text", "Task cancelled at step " + std::to_string(i) +
                                                         "/" + std::to_string(steps)}}})},
                            {"isError", true}};
                    }

                    auto executor = co_await asio::this_coro::executor;
                    asio::steady_timer work_timer(executor);
                    work_timer.expires_after(std::chrono::milliseconds(step_duration_ms));
                    co_await work_timer.async_wait(asio::use_awaitable);

                    int progress = (i + 1) * 100 / steps;
                    std::string message =
                        "Processing step " + std::to_string(i + 1) + "/" + std::to_string(steps);
                    co_await ctx.report_progress(progress, 100, message);
                    std::cout << "Tool: Reported progress " << progress << "%\n";
                }

                std::cout << "Tool: Completed all steps\n";
                co_return nlohmann::json{
                    {"content", nlohmann::json::array(
                                    {nlohmann::json{{"type", "text"},
                                                    {"text", "Task completed successfully with " +
                                                                 std::to_string(steps) + " steps"}}})},
                    {"isError", false}};
            });

        // ========== TRANSPORT & RUN ==========
        auto [client_transport, server_transport] =
            mcp::create_memory_transport_pair(io_ctx.get_executor());

        asio::co_spawn(
            io_ctx,
            [&]() -> Task<void> {
                try {
                    co_await server.run(server_transport, io_ctx.get_executor());
                } catch (const std::exception& e) {
                    std::cerr << "Server error: " << e.what() << "\n";
                }
            },
            asio::detached);

        asio::co_spawn(
            io_ctx,
            [&]() -> Task<void> {
                try {
                    std::cout << "Client: Waiting for server to start\n";
                    asio::steady_timer delay_timer(io_ctx.get_executor());
                    delay_timer.expires_after(std::chrono::milliseconds(50));
                    co_await delay_timer.async_wait(asio::use_awaitable);

                    std::cout << "Client: Creating client\n";
                    mcp::Client client(client_transport, io_ctx.get_executor());

                    Implementation client_info;
                    client_info.name = "progress-client";
                    client_info.version = "1.0.0";

                    std::cout << "Client: Connecting to server\n";
                    auto init_result = co_await client.connect(client_info, ClientCapabilities{});
                    std::cout << "Client: Connected to " << init_result.serverInfo.name << "\n";

                    bool progress_received = false;
                    bool cancel_sent = false;
                    int progress_count = 0;

                    client.on_progress([&](const mcp::ProgressNotificationParams& params) {
                        progress_received = true;
                        progress_count++;
                        if (params.message) {
                            std::cout << "Client: Progress notification - " << *params.message << "\n";
                        }
                        std::cout << "Client: Progress value: " << params.progress << "\n";

                        if (progress_count == 3 && !cancel_sent) {
                            cancel_sent = true;
                            std::cout << "Client: Sending cancellation request\n";
                            // The SDK assigns auto-incrementing integer IDs to JSON-RPC
                            // requests. The `initialize` call uses ID 1, so our
                            // `call_tool` uses ID 2. In production, store the
                            // request ID returned by call_tool instead of hardcoding.
                            asio::co_spawn(
                                io_ctx,
                                [&client]() -> mcp::Task<void> {
                                    co_await client.cancel(mcp::RequestId{"2"},
                                                           "User requested cancellation");
                                },
                                asio::detached);
                        }
                    });

                    std::cout << "Client: Calling long_task\n";
                    nlohmann::json call_args = {{"duration_ms", 1000}};

                    // Create CallToolParams with progress token in _meta
                    mcp::CallToolParams call_params;
                    call_params.name = "long_task";
                    call_params.arguments = call_args;
                    call_params.meta = nlohmann::json{{"progressToken", "progress-tok-1"}};

                    auto result_json =
                        co_await client.send_request("tools/call", nlohmann::json(call_params));
                    auto result = result_json.get<mcp::CallToolResult>();

                    std::cout << "Client: Tool completed\n";
                    for (const auto& content_block : result.content) {
                        if (const auto* text_content = std::get_if<mcp::TextContent>(&content_block)) {
                            std::cout << "Client: Result text: " << text_content->text << "\n";
                        }
                    }

                    if (progress_received) {
                        std::cout << "Client: Successfully received progress notifications\n";
                    }

                    std::cout << "Client: Closing connection\n";
                    client.close();

                } catch (const std::exception& e) {
                    std::cerr << "Client error: " << e.what() << "\n";
                }
            },
            asio::detached);

        io_ctx.run();

        std::cout << "Example completed successfully\n";
        return EXIT_SUCCESS;

    } catch (const std::exception& e) {
        std::cerr << "Exception: " << e.what() << "\n";
        return EXIT_FAILURE;
    } catch (...) {
        std::cerr << "Unknown exception\n";
        return EXIT_FAILURE;
    }
}

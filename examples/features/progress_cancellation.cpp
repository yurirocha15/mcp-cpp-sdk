/// @file progress_cancellation.cpp
/// @brief Demonstrates Context::report_progress() and Context::is_cancelled().
///
/// This example shows:
/// - A long-running tool that reports progress via ctx.report_progress()
/// - A client that subscribes to progress notifications
/// - A client that sends a cancellation request
/// - The tool handler checking ctx.is_cancelled() and exiting early
/// - RequestOptions::log_level asking the server for the log messages of one request
///
/// The client uses the default ClientProtocol::Auto(), so against this server it speaks
/// 2026-07-28. There the server sends nothing more for a request the client cancelled, not even
/// its response, so Client::cancel() ends the wait at once and the call fails with
/// g_REQUEST_CANCELLED. A client of an earlier revision gets the tool's own answer instead; the
/// code below handles both.

#include <mcp/client/client.hpp>
#include <mcp/server/server.hpp>
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
#include <stdexcept>
#include <string>

namespace asio = boost::asio;

int main() {
    try {
        using namespace mcp;

        asio::io_context io_ctx;
        bool client_ok = false;
        bool tool_saw_cancel = false;

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
            [&tool_saw_cancel](Context& ctx, const nlohmann::json& args) -> Task<nlohmann::json> {
                int duration_ms = args.at("duration_ms").get<int>();
                int steps = 10;
                int step_duration_ms = duration_ms / steps;

                std::cout << "Tool: Starting long_task for " << duration_ms << "ms\n";

                for (int i = 0; i < steps; ++i) {
                    if (ctx.is_cancelled()) {
                        std::cout << "Tool: Cancellation detected at step " << i << "\n";
                        tool_saw_cancel = true;
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
                    // Reaches the client only because its request set RequestOptions::log_level.
                    co_await ctx.log_info(message);
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
                    {
                        auto init_result = co_await client.connect(client_info, ClientCapabilities{});
                        // serverInfo is optional: a server may leave it out.
                        if (init_result.serverInfo) {
                            std::cout << "Client: Connected to " << init_result.serverInfo->name
                                      << "\n";
                        }
                    }

                    bool progress_received = false;
                    bool cancel_sent = false;
                    int progress_count = 0;

                    int log_count = 0;
                    client.on_notification("notifications/message",
                                           [&log_count](const nlohmann::json&) { ++log_count; });

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
                            // requests. connect() used ID 1 (the initialize or server/discover
                            // request), so our tools/call uses ID 2. In production, track the
                            // request ID instead of hardcoding it.
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

                    // Ask the server for the log messages of this request only.
                    mcp::RequestOptions request_options;
                    request_options.log_level = mcp::LoggingLevel::eInfo;

                    bool call_cancelled = false;
                    try {
                        auto result_json = co_await client.send_request(
                            "tools/call", nlohmann::json(call_params), request_options);
                        auto result = result_json.get<mcp::CallToolResult>();

                        std::cout << "Client: Tool answered\n";
                        for (const auto& content_block : result.content) {
                            if (const auto* text_content =
                                    std::get_if<mcp::TextContent>(&content_block)) {
                                std::cout << "Client: Result text: " << text_content->text << "\n";
                            }
                        }
                        // Only a session of an earlier revision gets here, with the answer of the
                        // tool that stopped early.
                        call_cancelled = result.isError.value_or(false);
                    } catch (const mcp::McpError& error) {
                        std::cout << "Client: Call ended: " << error.what() << "\n";
                        call_cancelled = error.code() == mcp::g_REQUEST_CANCELLED;
                    }

                    // Give the tool time to see the cancellation and stop.
                    asio::steady_timer settle_timer(io_ctx.get_executor());
                    settle_timer.expires_after(std::chrono::milliseconds(300));
                    co_await settle_timer.async_wait(asio::use_awaitable);

                    std::cout << "Client: Log messages received: " << log_count << "\n";
                    if (!progress_received || !call_cancelled || !tool_saw_cancel || log_count == 0) {
                        throw std::runtime_error("the call was not cancelled as expected");
                    }
                    std::cout << "Client: Successfully received progress and log messages, and "
                                 "cancelled the call\n";

                    std::cout << "Client: Closing connection\n";
                    client.close();
                    client_ok = true;

                } catch (const std::exception& e) {
                    std::cerr << "Client error: " << e.what() << "\n";
                }
            },
            asio::detached);

        io_ctx.run();

        if (!client_ok) {
            std::cerr << "Example failed\n";
            return EXIT_FAILURE;
        }
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

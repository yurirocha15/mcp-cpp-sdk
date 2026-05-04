/// @file pagination.cpp
/// @brief Demonstrates pagination in MCP using set_page_size().
///
/// This example shows:
/// - Server-side pagination: set_page_size() to limit items per response
/// - Server registering many tools (10+)
/// - Client-side pagination: fetching first page, then using cursor for subsequent pages
/// - Loopback pattern with MemoryTransport for in-process communication
/// - Handling nextCursor in list results

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
        server_info.name = "pagination-server";
        server_info.version = "1.0.0";

        Server server(server_info, server_caps);

        // ========== TOOL REGISTRATION ==========
        // Set page size to 3 to demonstrate pagination
        server.set_page_size(3);
        std::cout << "[Server] Page size set to 3\n";

        // Register 12 tools to demonstrate pagination
        nlohmann::json tool_schema = {{"type", "object"},
                                      {"properties", {{"value", {{"type", "string"}}}}},
                                      {"required", nlohmann::json::array({"value"})}};

        for (int i = 1; i <= 12; ++i) {
            std::string tool_name = "tool_" + std::to_string(i);
            std::string description = "Tool number " + std::to_string(i);

            server.add_tool(
                tool_name, description, tool_schema, [i](const nlohmann::json& args) -> nlohmann::json {
                    std::string text = "Tool " + std::to_string(i) +
                                       " executed with: " + args.at("value").get<std::string>();
                    return {{"content",
                             nlohmann::json::array(
                                 {nlohmann::json{{"type", "text"}, {"text", std::move(text)}}})}};
                });
        }

        std::cout << "[Server] Registered 12 tools\n";

        // ========== CLIENT SETUP ==========
        Implementation client_info;
        client_info.name = "pagination-client";
        client_info.version = "1.0.0";

        // Create bidirectional memory transport pair
        auto [server_transport, client_transport] = create_memory_transport_pair(io_ctx.get_executor());

        // ========== SERVER COROUTINE ==========
        asio::co_spawn(
            io_ctx,
            [&, transport = server_transport]() mutable -> Task<void> {
                std::cout << "[Server] Starting\n";
                co_await server.run(transport, io_ctx.get_executor());
            },
            asio::detached);

        // ========== CLIENT COROUTINE ==========
        asio::co_spawn(
            io_ctx,
            [&, transport = client_transport]() -> Task<void> {
                try {
                    // Wait for server to start
                    asio::steady_timer delay_timer(io_ctx.get_executor());
                    delay_timer.expires_after(std::chrono::milliseconds(50));
                    co_await delay_timer.async_wait(asio::use_awaitable);

                    std::cout << "[Client] Connecting\n";
                    Client client(transport, io_ctx.get_executor());

                    auto init_result = co_await client.connect(client_info, ClientCapabilities{});
                    std::cout << "[Client] Connected to: " << init_result.serverInfo.name << '\n';

                    // ========== PAGINATION LOOP ==========
                    std::cout << "\n[Client] Starting pagination through tools\n";

                    int page_num = 1;
                    std::optional<std::string> cursor;
                    int total_tools = 0;

                    while (true) {
                        std::cout << "\n[Client] Fetching page " << page_num;
                        if (cursor) {
                            std::cout << " (cursor: " << cursor.value() << ")";
                        }
                        std::cout << "\n";

                        // Call list_tools with cursor if available
                        auto list_result = co_await client.list_tools(cursor);

                        std::cout << "[Client] Received " << list_result.tools.size() << " tools\n";

                        // Display tools on this page
                        for (const auto& tool : list_result.tools) {
                            total_tools++;
                            std::cout << "  [" << total_tools << "] " << tool.name << ": ";
                            if (tool.description) {
                                std::cout << tool.description.value();
                            } else {
                                std::cout << "(no description)";
                            }
                            std::cout << "\n";
                        }

                        // Check if there are more pages
                        if (list_result.nextCursor) {
                            std::cout << "[Client] More pages available (nextCursor: "
                                      << list_result.nextCursor.value() << ")\n";
                            cursor = list_result.nextCursor;
                            page_num++;

                            // Small delay between pages
                            asio::steady_timer page_timer(io_ctx.get_executor());
                            page_timer.expires_after(std::chrono::milliseconds(50));
                            co_await page_timer.async_wait(asio::use_awaitable);
                        } else {
                            std::cout << "[Client] No more pages (nextCursor is null)\n";
                            break;
                        }
                    }

                    std::cout << "\n[Client] Pagination complete! Total tools fetched: " << total_tools
                              << "\n";

                    // ========== CLEANUP ==========
                    std::cout << "[Client] Closing connection\n";
                    client.close();
                    std::cout << "[Client] Connection closed\n";

                } catch (const std::exception& e) {
                    std::cerr << "[Client] Error: " << e.what() << '\n';
                    std::exit(1);
                }
            },
            asio::detached);

        // ========== AUTO-EXIT TIMER ==========
        asio::co_spawn(
            io_ctx,
            [&]() -> Task<void> {
                asio::steady_timer exit_timer(io_ctx.get_executor());
                exit_timer.expires_after(std::chrono::seconds(5));
                co_await exit_timer.async_wait(asio::use_awaitable);

                std::cout << "\n[Main] Auto-exit timer triggered\n";
                io_ctx.stop();
            },
            asio::detached);

        // Run the event loop
        io_ctx.run();

        std::cout << "[Main] Exiting successfully\n";
        return 0;

    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << '\n';
        return 1;
    }
}

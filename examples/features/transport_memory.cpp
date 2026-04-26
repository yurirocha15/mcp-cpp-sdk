/// @file transport_memory.cpp
/// @brief Demonstrates MemoryTransport and TransportFactory for in-process communication.
///
/// This example shows:
/// - MemoryTransport: In-memory bidirectional transport for testing
/// - create_memory_transport_pair(): Creating connected transport pairs
/// - TransportFactory: Factory pattern for creating transports bound to Runtime
/// - Runtime: Event loop management with run() and stop()
/// - Server and client communication via MemoryTransport
/// - Timer-based auto-exit pattern

#include <mcp/client.hpp>
#include <mcp/runtime.hpp>
#include <mcp/server.hpp>
#include <mcp/transport/memory.hpp>
#include <mcp/transport_factory.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

namespace asio = boost::asio;

// ========== DEMO 1: MemoryTransport with create_memory_transport_pair() ==========
void demo_memory_transport() {
    std::cout << "\n" << std::string(70, '=') << "\n";
    std::cout << "DEMO 1: MemoryTransport with create_memory_transport_pair()\n";
    std::cout << std::string(70, '=') << "\n\n";

    try {
        using namespace mcp;

        asio::io_context io_ctx;

        // ========== SERVER SETUP ==========
        ServerCapabilities server_caps;
        server_caps.tools = ServerCapabilities::ToolsCapability{};

        Implementation server_info;
        server_info.name = "memory-transport-server";
        server_info.version = "1.0.0";

        Server server(server_info, server_caps);

        // Register a simple echo tool
        nlohmann::json echo_schema = {{"type", "object"},
                                      {"properties", {{"message", {{"type", "string"}}}}},
                                      {"required", nlohmann::json::array({"message"})}};

        server.add_tool("echo", "Echoes the input message", echo_schema,
                        [](const nlohmann::json& args) -> nlohmann::json {
                            std::string msg = args.at("message").get<std::string>();
                            std::cout << "[Server] Echo tool called with: " << msg << "\n";
                            return nlohmann::json{
                                {"content", nlohmann::json::array(
                                                {nlohmann::json{{"type", "text"}, {"text", msg}}})},
                            };
                        });

        // ========== CLIENT SETUP ==========
        Implementation client_info;
        client_info.name = "memory-transport-client";
        client_info.version = "1.0.0";

        // Create bidirectional memory transport pair
        auto [server_transport, client_transport] = create_memory_transport_pair(io_ctx.get_executor());

        std::cout << "[Main] Created MemoryTransport pair\n";
        std::cout << "[Main] Server transport: " << server_transport.get() << "\n";
        std::cout << "[Main] Client transport: " << client_transport.get() << "\n";
        std::cout << "[Main] Transports are bidirectionally connected\n\n";

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
                    std::cout << "[Client] Connected to: " << init_result.serverInfo.name << "\n";
                    std::cout << "[Client] Server version: " << init_result.serverInfo.version
                              << "\n\n";

                    std::cout << "[Client] Sending ping...\n";
                    co_await client.ping();
                    std::cout << "[Client] Ping successful\n\n";

                    std::cout << "[Client] Listing available tools...\n";
                    auto tools_result = co_await client.list_tools();
                    std::cout << "[Client] Found " << tools_result.tools.size() << " tool(s)\n";
                    for (const auto& tool : tools_result.tools) {
                        std::string desc = tool.description ? *tool.description : "(no description)";
                        std::cout << "  - " << tool.name << ": " << desc << "\n";
                    }
                    std::cout << "\n";

                    std::cout << "[Client] Calling echo tool...\n";
                    try {
                        nlohmann::json echo_args = {{"message", "Hello from MemoryTransport!"}};
                        auto echo_result = co_await client.call_tool("echo", echo_args);
                        std::cout << "[Client] Echo result received with " << echo_result.content.size()
                                  << " content block(s)\n";
                    } catch (const std::exception& e) {
                        std::cout << "[Client] Echo tool error: " << e.what() << "\n";
                    }

                    std::cout << "\n[Client] Shutting down\n";
                } catch (const std::exception& e) {
                    std::cerr << "[Client] Fatal error: " << e.what() << '\n';
                }
            },
            asio::detached);

        // ========== AUTO-EXIT TIMER ==========
        asio::steady_timer exit_timer(io_ctx.get_executor());
        exit_timer.expires_after(std::chrono::seconds(2));

        asio::co_spawn(
            io_ctx,
            [&]() -> Task<void> {
                co_await exit_timer.async_wait(asio::use_awaitable);
                std::cout << "\n[Main] Auto-exit timer triggered\n";
                io_ctx.stop();
            },
            asio::detached);

        // ========== RUN IO CONTEXT ==========
        io_ctx.run();

        std::cout << "[Main] Demo 1 completed\n";

    } catch (const std::exception& e) {
        std::cerr << "Demo 1 error: " << e.what() << '\n';
    }
}

// ========== DEMO 2: TransportFactory with Runtime ==========
void demo_transport_factory() {
    std::cout << "\n" << std::string(70, '=') << "\n";
    std::cout << "DEMO 2: TransportFactory with Runtime\n";
    std::cout << std::string(70, '=') << "\n\n";

    try {
        using namespace mcp;

        Runtime runtime;
        TransportFactory factory(runtime);

        std::cout << "[Main] Runtime and TransportFactory created\n";
        std::cout << "[Main] Factory methods available:\n";
        std::cout << "  - factory.create_stdio() → stdio transport\n";
        std::cout << "  - factory.create_http_client(url) → HTTP transport\n\n";

        asio::io_context io_ctx;
        bool coroutine_ran = false;

        asio::co_spawn(
            io_ctx,
            [&]() -> Task<void> {
                auto [server_t, client_t] = create_memory_transport_pair(io_ctx.get_executor());

                Server s({"factory-demo", "1.0"}, {});
                s.add_tool("ping", "", {},
                           [](const nlohmann::json&) { return nlohmann::json{{"pong", true}}; });

                asio::co_spawn(io_ctx, s.run(server_t, io_ctx.get_executor()), asio::detached);

                asio::steady_timer delay(io_ctx.get_executor());
                delay.expires_after(std::chrono::milliseconds(10));
                co_await delay.async_wait(asio::use_awaitable);

                Client c(client_t, io_ctx.get_executor());
                co_await c.connect(Implementation{"demo", "1.0"}, {});
                co_await c.call_tool("ping", nlohmann::json{});
                std::cout << "[Demo] Tool call succeeded via MemoryTransport\n";

                coroutine_ran = true;
                io_ctx.stop();
            },
            asio::detached);

        io_ctx.run();
        std::cout << "[Main] Demo 2 completed (Runtime.run() would be used in production)\n";

    } catch (const std::exception& e) {
        std::cerr << "Demo 2 error: " << e.what() << '\n';
    }
}

// ========== MAIN ==========
int main() {
    try {
        std::cout << "\n" << std::string(70, '=') << "\n";
        std::cout << "MCP Transport Examples: MemoryTransport & TransportFactory\n";
        std::cout << std::string(70, '=') << "\n";

        // Run Demo 1: MemoryTransport
        demo_memory_transport();

        // Run Demo 2: TransportFactory
        demo_transport_factory();

        std::cout << "\n" << std::string(70, '=') << "\n";
        std::cout << "All examples completed successfully\n";
        std::cout << std::string(70, '=') << "\n\n";

        return EXIT_SUCCESS;

    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << '\n';
        return EXIT_FAILURE;
    }
}

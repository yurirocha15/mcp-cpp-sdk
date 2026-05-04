/// @file http_server_convenience.cpp
/// @brief Demonstrates the convenience run_http() method for HTTP servers.
///
/// This example shows the difference between:
/// 1. Manual HTTP setup (HttpServerTransport + listen + run)
/// 2. Convenience API (run_http) - simpler, handles everything internally
///
/// The convenience method is ideal for simple use cases where you don't need
/// fine-grained control over the transport lifecycle.
///
/// @note This example uses pthread_kill() to deliver SIGINT to the server thread
///       and is therefore POSIX-only (Linux / macOS). It will not compile on Windows.

#include <mcp/client.hpp>
#include <mcp/server.hpp>
#include <mcp/transport/http_client.hpp>

#include <unistd.h>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

namespace asio = boost::asio;

int main() {
    try {
        using namespace mcp;

        // ========== MANUAL SETUP (for comparison - NOT used in this example) ==========
        // This is what run_http() does internally:
        //
        //   asio::io_context io_ctx;
        //   auto transport = std::make_shared<HttpServerTransport>(
        //       io_ctx.get_executor(), "127.0.0.1", 18100);
        //
        //   asio::co_spawn(io_ctx, transport->listen(), asio::detached);
        //   asio::co_spawn(io_ctx, server.run(transport, io_ctx.get_executor()),
        //                  asio::detached);
        //
        //   io_ctx.run();  // Blocks until shutdown
        //

        // ========== CONVENIENCE SETUP (this example) ==========

        ServerCapabilities server_caps;
        server_caps.tools = ServerCapabilities::ToolsCapability{};

        Implementation server_info;
        server_info.name = "http-convenience-server";
        server_info.version = "1.0.0";

        Server server(server_info, server_caps);

        // Register a simple echo tool
        nlohmann::json tool_schema = {{"type", "object"},
                                      {"properties", {{"text", {{"type", "string"}}}}},
                                      {"required", nlohmann::json::array({"text"})}};

        server.add_tool<nlohmann::json, nlohmann::json>(
            "echo", "Echo the input text", tool_schema,
            [](nlohmann::json input_args) -> nlohmann::json {
                std::string text = input_args.at("text").get<std::string>();
                return nlohmann::json{{"content", nlohmann::json::array({nlohmann::json{
                                                      {"type", "text"}, {"text", "Echo: " + text}}})}};
            });

        constexpr unsigned short http_port = 18100;

        std::cout << "Starting HTTP server on port " << http_port << " (convenience method)\n";
        std::cout << "Server will run in background thread\n";

        // Spawn server in background thread
        std::thread server_thread([&server, http_port]() {
            try {
                // This convenience method handles everything:
                // - Creates io_context
                // - Creates HttpServerTransport
                // - Calls transport->listen()
                // - Calls server.run(transport, executor)
                // - Manages signal handling for graceful shutdown
                server.run_http("127.0.0.1", http_port);
            } catch (const std::exception& e) {
                std::cerr << "Server error: " << e.what() << '\n';
            }
        });

        // Give server time to start
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        // ========== CLIENT SIDE ==========

        asio::io_context client_io_ctx;

        asio::co_spawn(
            client_io_ctx,
            [&]() -> Task<void> {
                try {
                    std::cout << "\nClient: connecting to server\n";

                    auto http_client_transport = std::make_shared<HttpClientTransport>(
                        client_io_ctx.get_executor(), "http://127.0.0.1:18100/mcp");

                    Client client(http_client_transport, client_io_ctx.get_executor());

                    Implementation client_info;
                    client_info.name = "http-convenience-client";
                    client_info.version = "1.0.0";

                    std::cout << "Client: initializing connection\n";
                    auto init_result = co_await client.connect(client_info, ClientCapabilities{});
                    std::cout << "Connected to: " << init_result.serverInfo.name << '\n';

                    // List available tools
                    auto tools = co_await client.list_tools();
                    std::cout << "Available tools: " << tools.tools.size() << '\n';
                    for (const auto& tool : tools.tools) {
                        std::cout << "  - " << tool.name;
                        if (tool.description) {
                            std::cout << ": " << *tool.description;
                        }
                        std::cout << '\n';
                    }

                    // Call the echo tool
                    std::cout << "\nClient: calling echo tool\n";
                    nlohmann::json call_args = {{"text", "Hello from convenience API!"}};
                    auto result = co_await client.call_tool("echo", call_args);

                    std::cout << "Tool result:\n";
                    for (const auto& content_block : result.content) {
                        if (const auto* text_content = std::get_if<TextContent>(&content_block)) {
                            std::cout << "  " << text_content->text << '\n';
                        }
                    }

                    std::cout << "\nClient: closing connection\n";
                    client.close();

                } catch (const std::exception& error) {
                    std::cerr << "Client error: " << error.what() << '\n';
                }
            },
            asio::detached);

        client_io_ctx.run();

        // ========== SHUTDOWN ==========

        std::cout << "\nShutting down server...\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // Send SIGINT only to the server thread (not the whole process)
        ::pthread_kill(server_thread.native_handle(), SIGINT);

        // Wait for server thread to finish
        server_thread.join();

        std::cout << "Example complete\n";
        return EXIT_SUCCESS;

    } catch (const std::exception& e) {
        std::cerr << "Exception: " << e.what() << "\n";
        return EXIT_FAILURE;
    } catch (...) {
        return EXIT_FAILURE;
    }
}

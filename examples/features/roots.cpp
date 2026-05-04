/// @file roots.cpp
/// @brief Demonstrates client roots and server root discovery.
///
/// This example shows:
/// - A client declaring root URIs via client.set_roots()
/// - A client registering a handler for roots/list requests via on_roots_list()
/// - A server tool that calls ctx.request_roots() to discover client roots
/// - The server displaying discovered roots
/// - Loopback communication via MemoryTransport

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
        server_info.name = "roots-server";
        server_info.version = "1.0.0";

        Server server(server_info, server_caps);

        // ========== TOOL REGISTRATION ==========
        nlohmann::json tool_schema = {
            {"type", "object"}, {"properties", {}}, {"required", nlohmann::json::array()}};

        server.add_tool<nlohmann::json, nlohmann::json>(
            "discover_roots", "Discovers and displays the client's declared root URIs", tool_schema,
            [](Context& ctx, const nlohmann::json&) -> Task<nlohmann::json> {
                std::cout << "Tool: Requesting roots from client\n";

                try {
                    auto roots_result = co_await ctx.request_roots();

                    std::cout << "Tool: Received " << roots_result.roots.size() << " root(s)\n";

                    std::string output = "Discovered roots:\n";
                    for (const auto& root : roots_result.roots) {
                        std::cout << "Tool:   - URI: " << root.uri;
                        if (root.name) {
                            std::cout << " (name: " << *root.name << ")";
                        }
                        std::cout << "\n";

                        output += "  - " + root.uri;
                        if (root.name) {
                            output += " (" + *root.name + ")";
                        }
                        output += "\n";
                    }

                    co_return nlohmann::json{
                        {"content",
                         nlohmann::json::array({nlohmann::json{{"type", "text"}, {"text", output}}})},
                        {"isError", false}};
                } catch (const std::exception& e) {
                    std::cout << "Tool: Error requesting roots: " << e.what() << "\n";
                    std::string error_msg = "Error: " + std::string(e.what());
                    co_return nlohmann::json{{"content", nlohmann::json::array({nlohmann::json{
                                                             {"type", "text"}, {"text", error_msg}}})},
                                             {"isError", true}};
                }
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

                    // Declare roots that the client can access
                    std::vector<Root> client_roots = {
                        Root{.uri = "file:///workspace/", .name = "Workspace"},
                        Root{.uri = "mcp://docs/", .name = "Documentation"}};

                    std::cout << "Client: Setting " << client_roots.size() << " root(s)\n";
                    for (const auto& root : client_roots) {
                        std::cout << "Client:   - " << root.uri;
                        if (root.name) {
                            std::cout << " (" << *root.name << ")";
                        }
                        std::cout << "\n";
                    }
                    client.set_roots(client_roots);

                    Implementation client_info;
                    client_info.name = "roots-client";
                    client_info.version = "1.0.0";

                    std::cout << "Client: Connecting to server\n";
                    auto init_result = co_await client.connect(client_info, ClientCapabilities{});
                    std::cout << "Client: Connected to " << init_result.serverInfo.name << "\n";

                    std::cout << "Client: Calling discover_roots tool\n";
                    auto result = co_await client.call_tool("discover_roots", nlohmann::json::object());

                    std::cout << "Client: Tool completed\n";
                    std::cout << "Client: Result has " << result.content.size()
                              << " content block(s)\n";
                    for (const auto& content_block : result.content) {
                        if (const auto* text_content = std::get_if<mcp::TextContent>(&content_block)) {
                            std::cout << "Client: Result:\n" << text_content->text << "\n";
                        }
                    }

                    std::cout << "Client: Closing connection\n";
                    client.close();

                    // Schedule shutdown
                    asio::steady_timer shutdown_timer(io_ctx.get_executor());
                    shutdown_timer.expires_after(std::chrono::milliseconds(100));
                    co_await shutdown_timer.async_wait(asio::use_awaitable);
                    io_ctx.stop();

                } catch (const std::exception& e) {
                    std::cerr << "Client error: " << e.what() << "\n";
                    io_ctx.stop();
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

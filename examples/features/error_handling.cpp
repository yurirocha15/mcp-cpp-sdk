/// @file error_handling.cpp
/// @brief Demonstrates error handling patterns in MCP server and client.
///
/// This example shows:
/// - Server-side error patterns: throwing exceptions, returning error JSON, invalid JSON access
/// - Client-side error handling: catching exceptions from call_tool()
/// - JSON-RPC error codes and messages
/// - Graceful degradation and error recovery
/// - Loopback pattern with MemoryTransport for in-process communication

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
        server_info.name = "error-handling-server";
        server_info.version = "1.0.0";

        Server server(server_info, server_caps);

        // ========== TOOL REGISTRATION ==========
        // Tool 1: Throws std::runtime_error
        nlohmann::json throw_error_schema = {{"type", "object"},
                                             {"properties", {{"message", {{"type", "string"}}}}},
                                             {"required", nlohmann::json::array({"message"})}};

        server.add_tool("throw_error", "Throws a runtime error", throw_error_schema,
                        [](const nlohmann::json& args) -> nlohmann::json {
                            std::cout << "[Server] throw_error handler called\n";
                            std::string msg = args.at("message").get<std::string>();
                            throw std::runtime_error("Intentional error: " + msg);
                        });

        // Tool 2: Returns error JSON explicitly
        nlohmann::json return_error_schema = {{"type", "object"},
                                              {"properties", {{"code", {{"type", "integer"}}}}},
                                              {"required", nlohmann::json::array({"code"})}};

        server.add_tool("return_error", "Returns error JSON", return_error_schema,
                        [](const nlohmann::json& args) -> nlohmann::json {
                            std::cout << "[Server] return_error handler called\n";
                            int code = args.at("code").get<int>();
                            mcp::CallToolResult result;
                            mcp::TextContent content;
                            content.text = "Application error with code " + std::to_string(code);
                            result.content.emplace_back(std::move(content));
                            result.isError = true;
                            return nlohmann::json(result);
                        });

        // Tool 3: Accesses invalid argument (nlohmann::json throws)
        nlohmann::json invalid_access_schema = {
            {"type", "object"},
            {"properties", {{"optional_field", {{"type", "string"}}}}},
            {"required", nlohmann::json::array({})}};

        server.add_tool("invalid_access", "Accesses non-existent argument", invalid_access_schema,
                        [](const nlohmann::json& args) -> nlohmann::json {
                            std::cout << "[Server] invalid_access handler called\n";
                            // This will throw if "required_field" doesn't exist
                            std::string value = args.at("required_field").get<std::string>();
                            mcp::CallToolResult result;
                            mcp::TextContent content;
                            content.text = value;
                            result.content.emplace_back(std::move(content));
                            return nlohmann::json(result);
                        });

        // Tool 4: Successful tool (for comparison)
        nlohmann::json success_schema = {{"type", "object"},
                                         {"properties", {{"value", {{"type", "integer"}}}}},
                                         {"required", nlohmann::json::array({"value"})}};

        server.add_tool("success", "Returns successfully", success_schema,
                        [](const nlohmann::json& args) -> nlohmann::json {
                            std::cout << "[Server] success handler called\n";
                            int value = args.at("value").get<int>();
                            mcp::CallToolResult result;
                            mcp::TextContent content;
                            content.text =
                                "Successfully doubled the value: " + std::to_string(value * 2);
                            result.content.emplace_back(std::move(content));
                            return nlohmann::json(result);
                        });

        // ========== CLIENT SETUP ==========
        Implementation client_info;
        client_info.name = "error-handling-client";
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
                    std::cout << "[Client] Connected to: " << init_result.serverInfo.name << "\n\n";

                    // ========== TEST 1: Catch exception from thrown error ==========
                    std::cout << "=== TEST 1: Catch exception from thrown error ===\n";
                    try {
                        std::cout << "[Client] Calling throw_error tool...\n";
                        nlohmann::json args1 = {{"message", "test error"}};
                        auto result1 = co_await client.call_tool("throw_error", args1);
                        std::cout << "[Client] Server handled error as result (isError=true): "
                                  << result1.content.size() << " content blocks\n";
                    } catch (const std::exception& e) {
                        std::cout << "[Client] Caught exception: " << e.what() << "\n";
                        std::cout << "[Client] Server continues running after error\n";
                    }
                    std::cout << "\n";

                    // Wait between tests
                    asio::steady_timer wait_timer(io_ctx.get_executor());
                    wait_timer.expires_after(std::chrono::milliseconds(100));
                    co_await wait_timer.async_wait(asio::use_awaitable);

                    // ========== TEST 2: Handle error JSON response ==========
                    std::cout << "=== TEST 2: Handle error JSON response ===\n";
                    try {
                        std::cout << "[Client] Calling return_error tool...\n";
                        nlohmann::json args2 = {{"code", 42}};
                        auto result2 = co_await client.call_tool("return_error", args2);
                        std::cout << "[Client] Tool returned (may contain error flag)\n";
                        if (result2.isError && *result2.isError) {
                            std::cout << "[Client] Response has isError=true\n";
                        }
                        std::cout << "[Client] Content blocks: " << result2.content.size() << "\n";
                    } catch (const std::exception& e) {
                        std::cout << "[Client] Caught exception: " << e.what() << "\n";
                    }
                    std::cout << "\n";

                    // Wait between tests
                    wait_timer.expires_after(std::chrono::milliseconds(100));
                    co_await wait_timer.async_wait(asio::use_awaitable);

                    // ========== TEST 3: Catch exception from invalid JSON access ==========
                    std::cout << "=== TEST 3: Catch exception from invalid JSON access ===\n";
                    try {
                        std::cout
                            << "[Client] Calling invalid_access tool (missing required_field)...\n";
                        nlohmann::json args3 = {{"optional_field", "value"}};
                        auto result3 = co_await client.call_tool("invalid_access", args3);
                        std::cout << "[Client] Server handled error as result (isError=true): "
                                  << result3.content.size() << " content blocks\n";
                    } catch (const std::exception& e) {
                        std::cout << "[Client] Caught exception: " << e.what() << "\n";
                        std::cout << "[Client] Server continues running after error\n";
                    }
                    std::cout << "\n";

                    // Wait between tests
                    wait_timer.expires_after(std::chrono::milliseconds(100));
                    co_await wait_timer.async_wait(asio::use_awaitable);

                    // ========== TEST 4: Successful call after errors ==========
                    std::cout << "=== TEST 4: Successful call after errors (graceful recovery) ===\n";
                    try {
                        std::cout << "[Client] Calling success tool...\n";
                        nlohmann::json args4 = {{"value", 21}};
                        auto result4 = co_await client.call_tool("success", args4);
                        std::cout << "[Client] Tool call succeeded\n";
                        std::cout << "[Client] Content blocks: " << result4.content.size() << "\n";
                        std::cout << "[Client] Server recovered from previous errors\n";
                    } catch (const std::exception& e) {
                        std::cout << "[Client] Caught exception: " << e.what() << "\n";
                    }
                    std::cout << "\n";

                    // Wait before shutdown
                    wait_timer.expires_after(std::chrono::milliseconds(100));
                    co_await wait_timer.async_wait(asio::use_awaitable);

                    std::cout << "[Client] Shutting down\n";
                } catch (const std::exception& e) {
                    std::cerr << "[Client] Fatal error: " << e.what() << '\n';
                }
            },
            asio::detached);

        // ========== AUTO-EXIT TIMER ==========
        // Exit after 3 seconds to prevent hanging
        asio::steady_timer exit_timer(io_ctx.get_executor());
        exit_timer.expires_after(std::chrono::seconds(3));

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

        std::cout << "[Main] Exiting normally\n";
        return EXIT_SUCCESS;

    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << '\n';
        return EXIT_FAILURE;
    }
}

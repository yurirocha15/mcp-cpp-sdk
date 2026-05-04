/// @file middleware.cpp
/// @brief Demonstrates server.use() for middleware registration and execution order.
///
/// This example shows:
/// - Registering middleware with server.use()
/// - Middleware execution order: first registered = outermost (logs first)
/// - Middleware chain wrapping tool handlers
/// - Middleware short-circuiting (returning error before handler)
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
        server_info.name = "middleware-server";
        server_info.version = "1.0.0";

        Server server(server_info, server_caps);

        // ========== MIDDLEWARE REGISTRATION ==========
        // Middleware A: Outer middleware (registered first)
        // Logs before and after the next middleware/handler
        server.use([](Context& ctx, const nlohmann::json& params,
                      TypeErasedHandler next) -> Task<nlohmann::json> {
            std::cout << "[Middleware A] Before handler\n";
            co_await ctx.log_info("Middleware A: before");

            auto result = co_await next(ctx, params);

            std::cout << "[Middleware A] After handler\n";
            co_await ctx.log_info("Middleware A: after");
            co_return result;
        });

        // Middleware B: Middle middleware (registered second)
        // Logs before and after, and can inspect params
        server.use([](Context& ctx, const nlohmann::json& params,
                      TypeErasedHandler next) -> Task<nlohmann::json> {
            std::cout << "[Middleware B] Before handler\n";
            co_await ctx.log_info("Middleware B: before");

            auto result = co_await next(ctx, params);

            std::cout << "[Middleware B] After handler\n";
            co_await ctx.log_info("Middleware B: after");
            co_return result;
        });

        // Middleware C: Inner middleware (registered third)
        // This middleware can short-circuit by checking params
        server.use([](Context& ctx, const nlohmann::json& params,
                      TypeErasedHandler next) -> Task<nlohmann::json> {
            std::cout << "[Middleware C] Before handler\n";
            co_await ctx.log_info("Middleware C: before");

            // Check if params contain a "block" flag to demonstrate short-circuiting
            if (params.contains("arguments") && params.at("arguments").contains("block") &&
                params.at("arguments").at("block").is_boolean() &&
                params.at("arguments").at("block").get<bool>()) {
                std::cout << "[Middleware C] Short-circuiting: block flag detected\n";
                co_await ctx.log_info("Middleware C: short-circuit");
                // Return error without calling next
                mcp::CallToolResult err;
                mcp::TextContent content;
                content.text = "Request blocked by middleware C";
                err.content.emplace_back(std::move(content));
                err.isError = true;
                co_return nlohmann::json(err);
            }

            auto result = co_await next(ctx, params);

            std::cout << "[Middleware C] After handler\n";
            co_await ctx.log_info("Middleware C: after");
            co_return result;
        });

        // ========== TOOL REGISTRATION ==========
        // Register a simple tool that will be wrapped by the middleware chain
        nlohmann::json echo_schema = {{"type", "object"},
                                      {"properties", {{"message", {{"type", "string"}}}}},
                                      {"required", nlohmann::json::array({"message"})}};

        server.add_tool("echo", "Echo a message", echo_schema,
                        [](const nlohmann::json& args) -> nlohmann::json {
                            std::cout << "[Handler] Executing echo tool\n";
                            // Return a CallToolResult-compatible structure with content
                            mcp::CallToolResult result;
                            mcp::TextContent content;
                            content.text = args.at("message").get<std::string>();
                            result.content.emplace_back(std::move(content));
                            return nlohmann::json(result);
                        });

        // ========== CLIENT SETUP ==========
        Implementation client_info;
        client_info.name = "middleware-client";
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

                    // ========== TEST 1: Normal execution (middleware chain) ==========
                    std::cout << "=== TEST 1: Normal execution (middleware chain) ===\n";
                    std::cout << "Expected order: A before -> B before -> C before -> Handler -> C "
                                 "after -> B after -> A after\n\n";

                    nlohmann::json args1 = {{"message", "Hello from middleware"}};
                    auto result1 = co_await client.call_tool("echo", args1);
                    std::cout << "[Client] Tool call succeeded\n";
                    if (result1.isError) {
                        std::cout << "[Client] Error flag: " << *result1.isError << "\n";
                    }
                    std::cout << "[Client] Content blocks: " << result1.content.size() << "\n\n";

                    // Wait a bit between tests
                    asio::steady_timer wait_timer(io_ctx.get_executor());
                    wait_timer.expires_after(std::chrono::milliseconds(100));
                    co_await wait_timer.async_wait(asio::use_awaitable);

                    // ========== TEST 2: Short-circuit execution ==========
                    std::cout << "=== TEST 2: Short-circuit execution (middleware C blocks) ===\n";
                    std::cout << "Expected order: A before -> B before -> C before (short-circuit) -> "
                                 "B after -> A after\n";
                    std::cout << "Handler should NOT be called\n\n";

                    try {
                        nlohmann::json args2 = {{"message", "test"}, {"block", true}};
                        auto result2 = co_await client.call_tool("echo", args2);
                        std::cout << "[Client] Tool call succeeded\n";
                        if (result2.isError) {
                            std::cout << "[Client] Error flag: " << *result2.isError << "\n";
                        }
                        std::cout << "[Client] Content blocks: " << result2.content.size() << "\n\n";
                    } catch (const std::exception& e) {
                        std::cout << "[Client] Exception caught: " << e.what() << "\n\n";
                    }

                    // Wait before shutdown
                    wait_timer.expires_after(std::chrono::milliseconds(100));
                    co_await wait_timer.async_wait(asio::use_awaitable);

                    std::cout << "[Client] Shutting down\n";
                } catch (const std::exception& e) {
                    std::cerr << "[Client] Error: " << e.what() << '\n';
                }
            },
            asio::detached);

        // ========== AUTO-EXIT TIMER ==========
        // Exit after 2 seconds to prevent hanging
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

        io_ctx.run();
        std::cout << "[Main] Exiting\n";
        return 0;

    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << '\n';
        return 1;
    }
}

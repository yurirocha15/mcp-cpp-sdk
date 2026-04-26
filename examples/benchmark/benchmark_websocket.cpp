/// @file benchmark_websocket.cpp
/// @brief Example: measuring MCP tool call throughput over WebSocket transport.
///
/// This is a teaching example, not a production benchmark. Use benchmark/ for production load testing.
///
/// This example demonstrates:
/// - WebSocket server setup with TCP acceptor + WebSocketServerTransport
/// - WebSocket client connection with WebSocketClientTransport
/// - Measuring roundtrip latency for tool calls

#include <mcp/client.hpp>
#include <mcp/server.hpp>
#include <mcp/transport/websocket.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>

namespace asio = boost::asio;
using tcp = asio::ip::tcp;

int main() {
    try {
        using namespace mcp;

        const int NUM_ITERATIONS = 1000;
        const std::string HOST = "127.0.0.1";

        asio::io_context io_ctx;
        tcp::acceptor acceptor(io_ctx, tcp::endpoint(tcp::v4(), 0));
        auto port = acceptor.local_endpoint().port();

        std::cout << "\n" << std::string(70, '=') << "\n";
        std::cout << "WebSocket Transport Benchmark\n";
        std::cout << std::string(70, '=') << "\n";
        std::cout << "Iterations: " << NUM_ITERATIONS << "\n";
        std::cout << "Port: " << port << "\n\n";

        ServerCapabilities server_caps;
        server_caps.tools = ServerCapabilities::ToolsCapability{};

        Implementation server_info;
        server_info.name = "ws-benchmark-server";
        server_info.version = "1.0.0";

        Server server(server_info, server_caps);

        nlohmann::json tool_schema = {{"type", "object"},
                                      {"properties", {{"message", {{"type", "string"}}}}}};

        server.add_tool<nlohmann::json, nlohmann::json>(
            "echo", "Echo benchmark tool", tool_schema, [](nlohmann::json args) -> nlohmann::json {
                std::string msg = args.value("message", "ping");
                return nlohmann::json{{"content", nlohmann::json::array({nlohmann::json{
                                                      {"type", "text"}, {"text", msg}}})}};
            });

        asio::co_spawn(
            io_ctx,
            [&]() -> Task<void> {
                auto socket = co_await acceptor.async_accept(asio::use_awaitable);
                auto transport = std::make_shared<WebSocketServerTransport>(std::move(socket));
                co_await server.run(transport, io_ctx.get_executor());
            },
            asio::detached);

        asio::co_spawn(
            io_ctx,
            [&]() -> Task<void> {
                asio::steady_timer delay(io_ctx.get_executor());
                delay.expires_after(std::chrono::milliseconds(50));
                co_await delay.async_wait(asio::use_awaitable);

                auto transport = std::make_shared<WebSocketClientTransport>(io_ctx.get_executor(), HOST,
                                                                            std::to_string(port));
                Client client(transport, io_ctx.get_executor());

                Implementation client_info;
                client_info.name = "ws-benchmark-client";
                client_info.version = "1.0.0";

                auto init = co_await client.connect(client_info, ClientCapabilities{});
                std::cout << "[Client] Connected to: " << init.serverInfo.name << "\n";

                nlohmann::json warmup_args = {{"message", "warmup"}};
                nlohmann::json bench_args = {{"message", "bench"}};

                std::cout << "[Client] Warmup phase (10 calls)...\n";
                for (int i = 0; i < 10; ++i) {
                    co_await client.call_tool("echo", warmup_args);
                }
                std::cout << "[Client] Warmup complete\n\n";

                std::cout << "[Client] Starting benchmark (" << NUM_ITERATIONS << " iterations)...\n";
                auto start = std::chrono::high_resolution_clock::now();

                for (int i = 1; i <= NUM_ITERATIONS; ++i) {
                    co_await client.call_tool("echo", bench_args);
                    if (i % 100 == 0) {
                        std::cout << "[Client] Progress: " << i << "/" << NUM_ITERATIONS << "\n";
                    }
                }

                auto end = std::chrono::high_resolution_clock::now();
                auto total_us =
                    std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
                double total_sec = total_us / 1'000'000.0;
                double avg_ms = (total_us / 1000.0) / NUM_ITERATIONS;
                double calls_per_sec = NUM_ITERATIONS / total_sec;

                std::cout << "\n" << std::string(70, '=') << "\n";
                std::cout << std::fixed << std::setprecision(2);
                std::cout << "Total time: " << total_sec << " seconds\n";
                std::cout << "Average latency: " << avg_ms << " ms/call\n";
                std::cout << "Throughput: " << calls_per_sec << " calls/sec\n";
                std::cout << std::string(70, '=') << "\n";

                std::cout << "\n[Client] Shutting down\n";
                client.close();
                io_ctx.stop();
            },
            asio::detached);

        io_ctx.run();
        return EXIT_SUCCESS;

    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << '\n';
        return EXIT_FAILURE;
    }
}

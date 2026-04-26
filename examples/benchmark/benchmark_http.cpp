/// @file benchmark_http.cpp
/// @brief Example: measuring MCP tool call throughput over HTTP transport.
///
/// This is a teaching example, not a production benchmark. Use benchmark/ for production load testing.
///
/// This example demonstrates:
/// - Roundtrip latency for tool calls over HTTP transport
/// - Throughput (calls per second)
/// - Total time for fixed number of iterations

#include <mcp/client.hpp>
#include <mcp/server.hpp>
#include <mcp/transport/http_client.hpp>
#include <mcp/transport/http_server.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>

namespace asio = boost::asio;

int main() {
    try {
        using namespace mcp;

        const int NUM_ITERATIONS = 1000;
        const std::string HOST = "127.0.0.1";
        const int PORT = 18200;

        std::cout << "\n" << std::string(70, '=') << "\n";
        std::cout << "HTTP Transport Benchmark\n";
        std::cout << std::string(70, '=') << "\n";
        std::cout << "Iterations: " << NUM_ITERATIONS << "\n";
        std::cout << "Server: " << HOST << ":" << PORT << "\n\n";

        asio::io_context io_ctx;

        // ========== SERVER SETUP ==========
        ServerCapabilities server_caps;
        server_caps.tools = ServerCapabilities::ToolsCapability{};

        Implementation server_info;
        server_info.name = "http-benchmark-server";
        server_info.version = "1.0.0";

        Server server(server_info, server_caps);

        // Register a simple echo tool
        nlohmann::json echo_schema = {{"type", "object"},
                                      {"properties", {{"message", {{"type", "string"}}}}},
                                      {"required", nlohmann::json::array({"message"})}};

        server.add_tool("echo", "Echo tool for benchmarking", echo_schema,
                        [](const nlohmann::json& args) -> nlohmann::json {
                            std::string msg = args.at("message").get<std::string>();
                            return nlohmann::json{
                                {"content", nlohmann::json::array(
                                                {nlohmann::json{{"type", "text"}, {"text", msg}}})},
                            };
                        });

        // ========== SERVER COROUTINE ==========
        auto http_server_transport =
            std::make_shared<HttpServerTransport>(io_ctx.get_executor(), HOST, PORT);
        auto* http_transport_ptr = http_server_transport.get();

        asio::co_spawn(
            io_ctx,
            [&, transport = http_server_transport]() mutable -> Task<void> {
                try {
                    asio::co_spawn(io_ctx, http_transport_ptr->listen(), asio::detached);
                    // Give listen() time to start
                    asio::steady_timer startup_delay(io_ctx.get_executor());
                    startup_delay.expires_after(std::chrono::milliseconds(100));
                    co_await startup_delay.async_wait(asio::use_awaitable);
                    std::cout << "[Server] HTTP server listening on " << HOST << ":" << PORT << "\n";
                    co_await server.run(transport, io_ctx.get_executor());
                } catch (const std::exception& e) {
                    std::cerr << "[Server] Error: " << e.what() << '\n';
                }
            },
            asio::detached);

        // ========== CLIENT COROUTINE ==========
        asio::co_spawn(
            io_ctx,
            [&]() -> Task<void> {
                try {
                    // Wait for server to start
                    asio::steady_timer delay_timer(io_ctx.get_executor());
                    delay_timer.expires_after(std::chrono::milliseconds(200));
                    co_await delay_timer.async_wait(asio::use_awaitable);

                    Implementation client_info;
                    client_info.name = "http-benchmark-client";
                    client_info.version = "1.0.0";

                    std::string url = "http://" + HOST + ":" + std::to_string(PORT) + "/mcp";
                    auto http_client =
                        std::make_shared<HttpClientTransport>(io_ctx.get_executor(), url);

                    Client client(std::move(http_client), io_ctx.get_executor());

                    auto init_result = co_await client.connect(client_info, ClientCapabilities{});
                    std::cout << "[Client] Connected to: " << init_result.serverInfo.name << "\n\n";
                    std::cout.flush();

                    // Warmup
                    std::cout << "[Client] Warmup phase (10 calls)...\n";
                    std::cout.flush();
                    for (int i = 0; i < 10; ++i) {
                        nlohmann::json args = {{"message", "warmup"}};
                        co_await client.call_tool("echo", args);
                    }
                    std::cout << "[Client] Warmup complete\n\n";
                    std::cout.flush();

                    // Benchmark
                    std::cout << "[Client] Starting benchmark (" << NUM_ITERATIONS
                              << " iterations)...\n";
                    std::cout.flush();
                    auto start = std::chrono::steady_clock::now();

                    for (int i = 0; i < NUM_ITERATIONS; ++i) {
                        nlohmann::json args = {{"message", "benchmark_" + std::to_string(i)}};
                        co_await client.call_tool("echo", args);
                        if ((i + 1) % 100 == 0) {
                            std::cout << "[Client] Progress: " << (i + 1) << "/" << NUM_ITERATIONS
                                      << "\n";
                            std::cout.flush();
                        }
                    }

                    auto end = std::chrono::steady_clock::now();
                    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

                    // Calculate metrics
                    double total_ms = duration.count();
                    double avg_latency_ms = total_ms / NUM_ITERATIONS;
                    double calls_per_sec = (NUM_ITERATIONS * 1000.0) / total_ms;

                    std::cout << "\n" << std::string(70, '=') << "\n";
                    std::cout << "Benchmark Results (HTTP Transport)\n";
                    std::cout << std::string(70, '=') << "\n";
                    std::cout << std::fixed << std::setprecision(2);
                    std::cout << "Total time:        " << total_ms << " ms\n";
                    std::cout << "Iterations:        " << NUM_ITERATIONS << "\n";
                    std::cout << "Avg latency:       " << avg_latency_ms << " ms/call\n";
                    std::cout << "Throughput:        " << calls_per_sec << " calls/sec\n";
                    std::cout << std::string(70, '=') << "\n\n";

                    std::cout << "[Client] Shutting down\n";
                } catch (const std::exception& e) {
                    std::cerr << "[Client] Error: " << e.what() << '\n';
                }
            },
            asio::detached);

        // ========== AUTO-EXIT TIMER ==========
        asio::steady_timer exit_timer(io_ctx.get_executor());
        exit_timer.expires_after(std::chrono::seconds(15));

        asio::co_spawn(
            io_ctx,
            [&]() -> Task<void> {
                co_await exit_timer.async_wait(asio::use_awaitable);
                io_ctx.stop();
            },
            asio::detached);

        // ========== RUN IO CONTEXT ==========
        io_ctx.run();

        return EXIT_SUCCESS;

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << '\n';
        return EXIT_FAILURE;
    }
}

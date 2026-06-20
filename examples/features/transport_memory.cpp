/// @file transport_memory.cpp
/// @brief Demonstrates MemoryTransport and TransportFactory for in-process communication.
///
/// This example shows:
/// - MemoryTransport: In-memory bidirectional transport for testing
/// - create_memory_transport_pair(): Creating connected transport pairs
/// - TransportFactory: Factory pattern for creating transports bound to Runtime
/// - Runtime: Event loop management with run() and stop()
/// - Bidirectional message exchange via MemoryTransport

#include <mcp/core/runtime.hpp>
#include <mcp/transport/memory.hpp>
#include <mcp/transport/transport_factory.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <cstdlib>
#include <iostream>
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

        // Create bidirectional memory transport pair
        auto [transport_a, transport_b] = create_memory_transport_pair(io_ctx.get_executor());

        std::cout << "[Main] Created MemoryTransport pair\n";
        std::cout << "[Main] Transport A: " << transport_a.get() << "\n";
        std::cout << "[Main] Transport B: " << transport_b.get() << "\n";
        std::cout << "[Main] Transports are bidirectionally connected\n\n";

        bool success = false;
        asio::co_spawn(
            io_ctx,
            [transport_a, transport_b, &io_ctx, &success]() -> Task<void> {
                try {
                    co_await transport_a->write_message("hello from transport A");
                    std::string received_on_b = co_await transport_b->read_message();
                    std::cout << "[Transport B] Received: " << received_on_b << "\n";

                    co_await transport_b->write_message("reply from transport B");
                    std::string received_on_a = co_await transport_a->read_message();
                    std::cout << "[Transport A] Received: " << received_on_a << "\n";

                    std::string json_rpc = R"({"jsonrpc":"2.0","method":"demo/ping"})";
                    co_await transport_a->write_message(json_rpc);
                    std::string json_received = co_await transport_b->read_message();
                    std::cout << "[Transport B] JSON-RPC frame: " << json_received << "\n";
                    success = true;
                } catch (const std::exception& e) {
                    std::cerr << "[MemoryTransport] Fatal error: " << e.what() << '\n';
                }

                io_ctx.stop();
            },
            asio::detached);

        // ========== RUN IO CONTEXT ==========
        io_ctx.run();
        if (!success) {
            throw std::runtime_error("MemoryTransport message exchange did not complete");
        }

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
        std::cout << "  - factory.create_stdio() -> stdio transport\n";
        std::cout << "  - factory.create_http_client(url) -> HTTP transport\n\n";

        (void)factory;
        std::cout << "[Main] Demo 2 completed (use Runtime.run() in production event loops)\n";

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

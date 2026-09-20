/// @file interactive_client.cpp
/// @brief Minimal interactive MCP client over stdio.
///
/// @note On the stdio transport, stdout IS the protocol channel: StdioTransport
///   writes JSON-RPC messages there by default. Every human-readable line in
///   this file therefore goes to stderr. Diagnostics printed to stdout would
///   interleave prose with JSON-RPC and corrupt the stream for the peer.

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <iostream>
#include <mcp/client/client.hpp>
#include <mcp/transport/stdio.hpp>
#include <string>

using namespace mcp;

Task<void> run_client(const std::string& server_path) {
    auto executor = co_await boost::asio::this_coro::executor;

    std::cerr << "Connecting to server: " << server_path << "..." << '\n';

    // Simple stdio transport for demonstration.
    // In a real client, you would spawn the server process and connect its pipes.
    auto transport = std::make_unique<StdioTransport>(executor);
    Client client(std::move(transport), executor);

    Implementation info{"interactive-client", "1.0.0"};
    try {
        co_await client.connect(info, {});
    } catch (const std::exception& e) {
        std::cerr << "Failed to connect: " << e.what() << '\n';
        co_return;
    }

    std::cerr << "Connected! Listing tools..." << '\n';

    auto tools_result = co_await client.list_tools();
    for (const auto& tool : tools_result.tools) {
        std::cerr << "- " << tool.name << ": " << tool.description.value_or("(no description)") << '\n';
    }

    if (!tools_result.tools.empty()) {
        std::string tool_name = tools_result.tools[0].name;
        std::cerr << "Calling first tool: " << tool_name << "..." << '\n';

        nlohmann::json args = nlohmann::json::object();
        try {
            auto result = co_await client.call_tool(tool_name, args);
            std::cerr << "Result: " << nlohmann::json(result.content).dump(2) << '\n';
        } catch (const std::exception& e) {
            std::cerr << "Error calling tool: " << e.what() << '\n';
        }
    }

    co_return;
}

int main(int argc, char* argv[]) {
    try {
        if (argc < 2) {
            std::cerr << "Usage: " << argv[0] << " <server_executable_path>" << '\n';
            // return 1;
        }

        boost::asio::io_context io;
        boost::asio::co_spawn(io, run_client(argc > 1 ? argv[1] : ""), boost::asio::detached);
        io.run();

        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Exception: " << e.what() << "\n";
        return 1;
    } catch (...) {
        return 1;
    }
}

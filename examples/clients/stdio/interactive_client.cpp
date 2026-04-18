#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <iostream>
#include <mcp/client.hpp>
#include <mcp/transport/stdio.hpp>
#include <string>

using namespace mcp;

Task<void> run_client(const std::string& server_path) {
    auto executor = co_await boost::asio::this_coro::executor;

    std::cout << "Connecting to server: " << server_path << "..." << std::endl;

    // Simple stdio transport for demonstration.
    // In a real client, you would spawn the server process and connect its pipes.
    auto transport = std::make_unique<StdioTransport>(executor);
    Client client(std::move(transport), executor);

    Implementation info{"interactive-client", "1.0.0"};
    try {
        co_await client.connect(std::move(info), {});
    } catch (const std::exception& e) {
        std::cerr << "Failed to connect: " << e.what() << std::endl;
        co_return;
    }

    std::cout << "Connected! Listing tools..." << std::endl;

    auto tools_result = co_await client.list_tools();
    for (const auto& tool : tools_result.tools) {
        std::cout << "- " << tool.name << ": " << tool.description.value_or("(no description)")
                  << std::endl;
    }

    if (!tools_result.tools.empty()) {
        std::string tool_name = tools_result.tools[0].name;
        std::cout << "Calling first tool: " << tool_name << "..." << std::endl;

        nlohmann::json args = nlohmann::json::object();
        try {
            auto result = co_await client.call_tool(tool_name, args);
            std::cout << "Result: " << nlohmann::json(result.content).dump(2) << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "Error calling tool: " << e.what() << std::endl;
        }
    }

    co_return;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <server_executable_path>" << std::endl;
        // return 1;
    }

    boost::asio::io_context io;
    boost::asio::co_spawn(io, run_client(argc > 1 ? argv[1] : ""), boost::asio::detached);
    io.run();

    return 0;
}

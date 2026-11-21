/// @file interactive_client.cpp
/// @brief Minimal MCP client that launches a stdio server, lists its tools and calls the first.
///
/// Usage: example-interactive-client <server_executable_path> [server arguments...]

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <cstdlib>
#include <iostream>
#include <mcp/client/client.hpp>
#include <mcp/core/async.hpp>
#include <mcp/transport/stdio_client.hpp>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace mcp;

/// The transport is taken by value: a coroutine keeps its parameters in its frame, and a reference
/// to the caller's object could dangle once the coroutine first suspends. Strings stay out of the
/// frame across co_await, as docs/contributing.rst asks for GCC 11.
Task<bool> run_client(std::shared_ptr<StdioClientTransport> transport) {
    auto executor = co_await boost::asio::this_coro::executor;
    Client client(transport, executor);

    bool ok = true;
    try {
        // The default ClientProtocol::automatic() speaks 2026-07-28 with a server that serves it
        // and falls back to initialize otherwise.
        {
            auto connected = co_await client.connect("interactive-client", "1.0.0");
            std::cerr << "Protocol version: " << connected.protocolVersion << '\n';
        }

        std::cerr << "Connected! Listing tools..." << '\n';
        auto tools_result = co_await client.list_tools();
        for (const auto& tool : tools_result.tools) {
            std::cerr << "- " << tool.name << ": " << tool.description.value_or("(no description)")
                      << '\n';
        }

        if (!tools_result.tools.empty()) {
            const auto& tool = tools_result.tools[0];
            std::cerr << "Calling first tool: " << tool.name << "..." << '\n';

            // Empty arguments: a tool that requires some answers with an isError result that names
            // what is missing.
            nlohmann::json args = nlohmann::json::object();
            auto result = co_await client.call_tool(tool.name, args);
            std::cerr << "Result: " << nlohmann::json(result).dump(2) << '\n';
        }
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << '\n';
        ok = false;
    }

    // End the session, then the server process, and wait for it to exit.
    client.close();
    const ProcessExit exit = co_await transport->shutdown();
    std::cerr << "Server exited with code " << exit.code << '\n';
    co_return ok;
}

int main(int argc, char* argv[]) {
    try {
        if (argc < 2) {
            std::cerr << "Usage: " << argv[0] << " <server_executable_path> [server arguments...]"
                      << '\n';
            return EXIT_FAILURE;
        }

        StdioProcessOptions process;
        process.command = argv[1];
        process.arguments = std::vector<std::string>(argv + 2, argv + argc);
        std::cerr << "Launching server: " << process.command << "..." << '\n';

        boost::asio::io_context io;
        // The transport starts the server process and talks to it over the process's standard
        // input and output.
        auto transport = std::make_shared<StdioClientTransport>(io.get_executor(), std::move(process));
        bool ok = false;
        boost::asio::co_spawn(
            io, run_client(std::move(transport)),
            [&ok](const std::exception_ptr& error, bool result) { ok = !error && result; });
        io.run();

        return ok ? EXIT_SUCCESS : EXIT_FAILURE;
    } catch (const std::exception& e) {
        std::cerr << "Exception: " << e.what() << "\n";
        return EXIT_FAILURE;
    } catch (...) {
        return EXIT_FAILURE;
    }
}

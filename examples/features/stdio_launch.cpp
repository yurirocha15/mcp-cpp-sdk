/// @file stdio_launch.cpp
/// @brief Launches an MCP server as a child process with StdioClientTransport and talks to it.
///
/// This example shows:
/// - StdioProcessOptions: the command, arguments, environment, working directory and what
///   happens to the server's standard error
/// - A Client over the transport, with the era chosen by connect()
/// - Shutdown as the specification describes it: the transport closes the server's standard
///   input, waits, and only then sends SIGTERM and SIGKILL (TerminateProcess on Windows)
/// - StdioClientSession, which also starts the server again when it ends unexpectedly
///
/// The server is the example-server-simple executable next to this one, or the program named by
/// the first argument.

#include <mcp/client/client.hpp>
#include <mcp/client/stdio_session.hpp>
#include <mcp/core/async.hpp>
#include <mcp/transport/stdio_client.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>

namespace asio = boost::asio;

namespace {

std::string server_program(int argc, char** argv) {
    if (argc > 1) {
        return argv[1];
    }
#ifdef _WIN32
    const char* name = "example-server-simple.exe";
#else
    const char* name = "example-server-simple";
#endif
    return (std::filesystem::absolute(argv[0]).parent_path() / name).string();
}

void print_text(const mcp::CallToolResult& result) {
    for (const auto& block : result.content) {
        if (const auto* text = std::get_if<mcp::TextContent>(&block)) {
            std::cout << "Client: tool answered: " << text->text << "\n";
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    try {
        using namespace mcp;
        asio::io_context io_ctx;
        bool succeeded = false;

        StdioProcessOptions process;
        process.command = server_program(argc, argv);
        // Lines the server writes to standard error go to the callback, not to this terminal.
        process.stderr_mode = StderrMode::eCapture;
        process.on_stderr_line = [](std::string_view line) {
            std::cout << "Server log: " << line << "\n";
        };
        process.on_exit = [](const ProcessExit& exit) {
            std::cout << "Server process ended with code " << exit.code
                      << (exit.requested ? " (asked to)" : " (on its own)") << "\n";
        };

        asio::co_spawn(
            io_ctx,
            [&]() -> Task<void> {
                try {
                    // One launch and one Client, shut down by hand.
                    {
                        auto transport =
                            std::make_shared<StdioClientTransport>(io_ctx.get_executor(), process);
                        Client client(transport, io_ctx.get_executor());
                        co_await client.connect("stdio-launch-example", "1.0");
                        std::cout << "Client: connected to process " << transport->process_id() << "\n";
                        const nlohmann::json arguments = {{"name", "World"}};
                        print_text(co_await client.call_tool("hello", arguments));
                        client.close();
                        const auto ended = co_await transport->shutdown();
                        std::cout << "Client: shutdown took " << (ended.forced ? "force" : "no force")
                                  << "\n";
                    }

                    // The same, supervised: a server that ends on its own is started again.
                    {
                        StdioSessionOptions options;
                        options.process = process;
                        options.client_info = Implementation{"stdio-launch-example", "1.0"};
                        options.restart.max_restarts = 2;
                        options.on_connected = [](Client&,
                                                  const ConnectResult& connected) -> Task<void> {
                            std::cout << "Session: connected, protocol " << connected.protocolVersion
                                      << "\n";
                            co_return;
                        };
                        StdioClientSession session(io_ctx.get_executor(), options);
                        co_await session.start();
                        const nlohmann::json arguments = {{"name", "Session"}};
                        print_text(co_await session.client()->call_tool("hello", arguments));
                        co_await session.stop();
                    }
                    succeeded = true;
                } catch (const std::exception& error) {
                    std::cerr << "Client error: " << error.what() << "\n";
                }
                io_ctx.stop();
            },
            asio::detached);

        io_ctx.run();
        if (!succeeded) {
            std::cerr << "Example failed\n";
            return EXIT_FAILURE;
        }
        std::cout << "Example completed successfully\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "Exception: " << error.what() << "\n";
        return EXIT_FAILURE;
    }
}

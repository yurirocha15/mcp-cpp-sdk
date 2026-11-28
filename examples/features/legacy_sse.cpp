/// @file legacy_sse.cpp
/// @brief Demonstrates the HTTP+SSE transport of protocol revision 2024-11-05, client and server.
///
/// This example shows, in sync mode and then in async mode:
/// - A server that also serves the HTTP+SSE transport, with
///   StreamableHttpSessionManager::set_legacy_sse()
/// - A client that connects to it, with Endpoint::legacy_sse() or a LegacySseClientTransport
/// - The session the two make, which is of an older revision than 2026-07-28
///
/// The specification deprecates HTTP+SSE in favour of Streamable HTTP. Use it only to reach a
/// peer that has no Streamable HTTP endpoint.

#include <mcp/core/async.hpp>
#include <mcp/mcp.hpp>
#include <mcp/transport/http_session_manager.hpp>
#include <mcp/transport/sse_legacy.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

namespace asio = boost::asio;

namespace {

std::unique_ptr<mcp::Server> make_server() {
    auto server = std::make_unique<mcp::Server>("legacy-sse-server", "1.0.0");
    server->add_tool(
        "echo", "Returns its text",
        nlohmann::json{{"type", "object"}, {"properties", {{"text", {{"type", "string"}}}}}},
        [](const nlohmann::json& args) -> nlohmann::json { return args.value("text", ""); });
    return server;
}

/// The server in sync mode, with a client that blocks on each call.
bool run_sync() {
    auto owner = make_server();
    mcp::Server& server = *owner;

    // docs-begin: legacy-sse-server
    // The server listens on a port the system chooses. Besides the Streamable HTTP endpoint, the
    // session manager serves the event stream at /sse and takes messages at /messages.
    std::thread serving([&server] {
        try {
            server.run_http("127.0.0.1", 0, [](mcp::StreamableHttpSessionManager& http) {
                http.set_legacy_sse({.sse_path = "/sse", .message_path = "/messages"});
            });
        } catch (const std::exception& error) {
            std::cerr << "Server error: " << error.what() << '\n';
        }
    });
    // docs-end: legacy-sse-server

    for (int attempt = 0; attempt < 200 && server.bound_port() == 0; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    bool succeeded = false;
    try {
        // docs-begin: legacy-sse-client
        // The client opens the event stream and POSTs its messages to the endpoint the stream
        // names.
        const std::string url = "http://127.0.0.1:" + std::to_string(server.bound_port()) + "/sse";
        mcp::Client client(mcp::Endpoint::legacy_sse(url));
        const auto connected = client.sync().connect("legacy-sse-client", "1.0.0");
        std::cout << "Connected, protocol version " << connected.protocolVersion << '\n';

        const auto echoed = client.sync().call_tool("echo", {{"text", "over HTTP+SSE"}});
        std::cout << "echo: " << mcp::result_text(echoed) << '\n';
        // docs-end: legacy-sse-client
        succeeded =
            connected.era == mcp::ProtocolEra::eLegacy && mcp::result_text(echoed) == "over HTTP+SSE";
        client.close();
    } catch (const std::exception& error) {
        std::cerr << "Client error: " << error.what() << '\n';
    }

    server.stop();
    serving.join();
    return succeeded;
}

/// The client in async mode, on the program's own event loop.
mcp::Task<bool> async_client(asio::io_context& io, unsigned short port) {
    // docs-begin: legacy-sse-async-client
    // The transport is made on the program's event loop and given to the client.
    auto transport = std::make_shared<mcp::LegacySseClientTransport>(
        io.get_executor(), "http://127.0.0.1:" + std::to_string(port) + "/sse");
    mcp::Client client(transport, io.get_executor());
    const auto connected = co_await client.connect("legacy-sse-client", "1.0.0");
    std::cout << "Connected, protocol version " << connected.protocolVersion << '\n';

    const nlohmann::json arguments = {{"text", "over HTTP+SSE"}};
    const auto echoed = co_await client.call_tool("echo", arguments);
    std::cout << "echo: " << mcp::result_text(echoed) << '\n';
    // docs-end: legacy-sse-async-client
    client.close();
    co_return connected.era == mcp::ProtocolEra::eLegacy&& mcp::result_text(echoed) == "over HTTP+SSE";
}

/// The same server and client on the program's own event loop.
bool run_async() {
    asio::io_context io;
    bool succeeded = false;
    auto owner = make_server();

    // docs-begin: legacy-sse-async-server
    // The manager serves the Streamable HTTP endpoint, and with set_legacy_sse() the event stream
    // and message path as well.
    mcp::StreamableHttpSessionManager manager(io.get_executor(), "127.0.0.1", 0, *owner);
    manager.set_legacy_sse({});
    asio::co_spawn(io, manager.listen(), asio::detached);
    // docs-end: legacy-sse-async-server

    asio::co_spawn(
        io,
        [&]() -> mcp::Task<void> {
            try {
                succeeded = co_await async_client(io, manager.port());
            } catch (const std::exception& error) {
                std::cerr << "Client error: " << error.what() << '\n';
            }
            manager.close();
            io.stop();
        },
        asio::detached);

    // Fail rather than hang if something never answers.
    asio::steady_timer watchdog(io.get_executor());
    watchdog.expires_after(std::chrono::seconds(10));
    watchdog.async_wait([&io](const boost::system::error_code& error) {
        if (!error) {
            std::cerr << "Timed out\n";
            io.stop();
        }
    });

    io.run();
    return succeeded;
}

}  // namespace

int main() {
    try {
        std::cout << "Sync mode\n";
        const bool sync_ok = run_sync();
        std::cout << "Async mode\n";
        const bool async_ok = run_async();

        if (!sync_ok || !async_ok) {
            std::cerr << "Example failed\n";
            return EXIT_FAILURE;
        }
        std::cout << "Example completed successfully\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "Exception: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}

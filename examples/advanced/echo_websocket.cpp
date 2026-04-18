/// @file echo_websocket.cpp
/// @brief Loopback WebSocket example: in-process server + client over TCP.

#include <mcp/client.hpp>
#include <mcp/server.hpp>
#include <mcp/transport/websocket.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

namespace asio = boost::asio;
using tcp = asio::ip::tcp;

int main() {
    using namespace mcp;

    asio::io_context io_ctx;

    ServerCapabilities caps;
    caps.tools = ServerCapabilities::ToolsCapability{};

    Implementation server_info;
    server_info.name = "ws-echo-server";
    server_info.version = "1.0.0";

    Server server(std::move(server_info), std::move(caps));

    nlohmann::json schema = {{"type", "object"},
                             {"properties", {{"text", {{"type", "string"}}}}},
                             {"required", nlohmann::json::array({"text"})}};

    server.add_tool<nlohmann::json, nlohmann::json>("echo", "Echo the input over WebSocket",
                                                    std::move(schema),
                                                    [](nlohmann::json in) -> nlohmann::json {
                                                        nlohmann::json out = {{"echo", in.at("text")}};
                                                        return out;
                                                    });

    // Bind a TCP acceptor on an ephemeral port
    tcp::acceptor acceptor(io_ctx, tcp::endpoint(tcp::v4(), 0));
    auto port = acceptor.local_endpoint().port();

    // Server side: accept TCP socket → WebSocketServerTransport handles WS upgrade
    asio::co_spawn(
        io_ctx,
        [&]() -> Task<void> {
            auto socket = co_await acceptor.async_accept(asio::use_awaitable);
            auto transport = std::make_shared<WebSocketServerTransport>(std::move(socket));
            co_await server.run(transport, io_ctx.get_executor());
        },
        boost::asio::detached);

    // Client side: WebSocketClientTransport handles connect + WS handshake internally
    asio::co_spawn(
        io_ctx,
        [&]() -> Task<void> {
            auto transport = std::make_shared<WebSocketClientTransport>(
                io_ctx.get_executor(), "127.0.0.1", std::to_string(port));
            Client client(transport, io_ctx.get_executor());

            Implementation client_info;
            client_info.name = "ws-echo-client";
            client_info.version = "1.0.0";

            auto init_result = co_await client.connect(std::move(client_info), ClientCapabilities{});
            std::cout << "Connected to: " << init_result.serverInfo.name << "\n";

            auto tools = co_await client.list_tools();
            std::cout << "Tools: " << tools.tools.size() << "\n";

            nlohmann::json args = {{"text", "hello over websocket"}};
            auto result = co_await client.call_tool("echo", std::move(args));
            for (const auto& block : result.content) {
                if (auto* text = std::get_if<TextContent>(&block)) {
                    std::cout << "Echo: " << text->text << "\n";
                }
            }

            std::cout << "WebSocket example complete\n";
            client.close();
        },
        boost::asio::detached);

    io_ctx.run();
    return EXIT_SUCCESS;
}

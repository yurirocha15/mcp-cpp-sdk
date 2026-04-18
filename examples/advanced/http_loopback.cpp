/// @file http_loopback.cpp
/// @brief Loopback HTTP example: in-process server + client over HTTP transport.

#include <mcp/client.hpp>
#include <mcp/server.hpp>
#include <mcp/transport/http_client.hpp>
#include <mcp/transport/http_server.hpp>

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
    using namespace mcp;

    asio::io_context io_ctx;

    ServerCapabilities server_caps;
    server_caps.tools = ServerCapabilities::ToolsCapability{};

    Implementation server_info;
    server_info.name = "http-echo-server";
    server_info.version = "1.0.0";

    Server server(std::move(server_info), std::move(server_caps));

    nlohmann::json tool_schema = {{"type", "object"},
                                  {"properties", {{"text", {{"type", "string"}}}}},
                                  {"required", nlohmann::json::array({"text"})}};

    server.add_tool<nlohmann::json, nlohmann::json>(
        "echo", "Echo the input over HTTP", std::move(tool_schema),
        [](nlohmann::json input_args) -> nlohmann::json {
            return nlohmann::json{
                {"content",
                 nlohmann::json::array({nlohmann::json{
                     {"type", "text"}, {"text", input_args.at("text").get<std::string>()}}})}};
        });

    constexpr unsigned short http_port = 18099;
    auto http_server_transport =
        std::make_shared<HttpServerTransport>(io_ctx.get_executor(), "127.0.0.1", http_port);

    std::cout << "Starting HTTP server on port " << http_port << std::endl;

    auto* http_transport_ptr = http_server_transport.get();
    asio::co_spawn(
        io_ctx,
        [&, transport = http_server_transport]() mutable -> Task<void> {
            asio::co_spawn(io_ctx, http_transport_ptr->listen(), asio::detached);
            co_await server.run(transport, io_ctx.get_executor());
        },
        asio::detached);

    asio::co_spawn(
        io_ctx,
        [&]() -> Task<void> {
            try {
                std::cout << "Client: waiting for server to start" << std::endl;
                asio::steady_timer delay_timer(io_ctx.get_executor());
                delay_timer.expires_after(std::chrono::milliseconds(100));
                co_await delay_timer.async_wait(asio::use_awaitable);

                std::cout << "Client: creating transport" << std::endl;
                auto http_client_transport = std::make_shared<HttpClientTransport>(
                    io_ctx.get_executor(), "http://127.0.0.1:18099/mcp");

                std::cout << "Client: creating client object" << std::endl;
                Client client(http_client_transport, io_ctx.get_executor());

                Implementation client_info;
                client_info.name = "http-echo-client";
                client_info.version = "1.0.0";

                std::cout << "Client: connecting" << std::endl;
                auto init_result =
                    co_await client.connect(std::move(client_info), ClientCapabilities{});
                std::cout << "Connected to: " << init_result.serverInfo.name << std::endl;

                auto tools = co_await client.list_tools();
                std::cout << "Tools: " << tools.tools.size() << std::endl;

                nlohmann::json call_args = {{"text", "hello over http"}};
                auto result = co_await client.call_tool("echo", std::move(call_args));
                for (const auto& content_block : result.content) {
                    if (auto* text_content = std::get_if<TextContent>(&content_block)) {
                        std::cout << "Echo: " << text_content->text << std::endl;
                    }
                }

                std::cout << "HTTP example complete" << std::endl;
                client.close();

                http_transport_ptr->close();
            } catch (const std::exception& error) {
                std::cerr << "Client error: " << error.what() << std::endl;
                http_transport_ptr->close();
            }
        },
        asio::detached);

    io_ctx.run();
    return EXIT_SUCCESS;
}

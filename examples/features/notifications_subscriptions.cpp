/// @file notifications_subscriptions.cpp
/// @brief Demonstrates notifications and subscriptions in MCP.
///
/// This example shows:
/// - Server-side notifications: notify_tools_list_changed(), notify_resources_list_changed(),
///   notify_prompts_list_changed(), notify_resource_updated()
/// - Server-side subscription callbacks: on_subscribe(), on_unsubscribe()
/// - Client-side notification handling: on_notification()
/// - Loopback pattern with MemoryTransport for in-process communication

#include <mcp/client.hpp>
#include <mcp/protocol/resources.hpp>
#include <mcp/server.hpp>
#include <mcp/transport/memory.hpp>

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
    try {
        using namespace mcp;

        asio::io_context io_ctx;

        // ========== SERVER SETUP ==========
        ServerCapabilities server_caps;
        server_caps.resources = ServerCapabilities::ResourcesCapability{};

        Implementation server_info;
        server_info.name = "notifications-server";
        server_info.version = "1.0.0";

        Server server(server_info, server_caps);

        // ========== RESOURCE REGISTRATION ==========
        // Register a resource that will be updated
        const std::string resource_uri = "mcp://status/counter";
        int counter = 0;

        Resource resource;
        resource.uri = resource_uri;
        resource.name = "counter";
        resource.description = "A counter resource";
        resource.mimeType = "text/plain";

        server.add_resource<nlohmann::json, nlohmann::json>(
            resource, [&counter](const nlohmann::json&) -> nlohmann::json {
                return nlohmann::json{
                    {"content",
                     nlohmann::json::array({nlohmann::json{
                         {"type", "text"}, {"text", "Counter: " + std::to_string(counter)}}})}};
            });

        // ========== SUBSCRIPTION HANDLERS ==========
        // Track subscriptions
        std::cout << "[Server] Registering subscription callbacks\n";
        server.on_subscribe([](const std::string& uri) {
            std::cout << "[Server] Client subscribed to: " << uri << '\n';
        });

        server.on_unsubscribe([](const std::string& uri) {
            std::cout << "[Server] Client unsubscribed from: " << uri << '\n';
        });

        // ========== CLIENT SETUP ==========
        Implementation client_info;
        client_info.name = "notifications-client";
        client_info.version = "1.0.0";

        // Create bidirectional memory transport pair
        auto [server_transport, client_transport] = create_memory_transport_pair(io_ctx.get_executor());

        // ========== SERVER COROUTINE ==========
        asio::co_spawn(
            io_ctx,
            [&, transport = server_transport]() mutable -> Task<void> {
                std::cout << "[Server] Starting\n";
                co_await server.run(transport, io_ctx.get_executor());
            },
            asio::detached);

        // ========== CLIENT COROUTINE ==========
        asio::co_spawn(
            io_ctx,
            [&, transport = client_transport]() -> Task<void> {
                try {
                    // Wait for server to start
                    asio::steady_timer delay_timer(io_ctx.get_executor());
                    delay_timer.expires_after(std::chrono::milliseconds(50));
                    co_await delay_timer.async_wait(asio::use_awaitable);

                    std::cout << "[Client] Connecting\n";
                    Client client(transport, io_ctx.get_executor());

                    auto init_result = co_await client.connect(client_info, ClientCapabilities{});
                    std::cout << "[Client] Connected to: " << init_result.serverInfo.name << '\n';

                    // ========== REGISTER NOTIFICATION HANDLERS ==========
                    std::cout << "[Client] Registering notification handlers\n";

                    // Handle resource updated notifications
                    client.on_notification(
                        "notifications/resources/updated", [](const nlohmann::json& params) {
                            std::cout
                                << "[Client] Received resource updated notification: " << params.dump()
                                << '\n';
                        });

                    // Handle tools list changed notifications
                    client.on_notification(
                        "notifications/tools/list_changed", [](const nlohmann::json& params) {
                            std::cout << "[Client] Received tools list changed notification\n";
                        });

                    // Handle resources list changed notifications
                    client.on_notification(
                        "notifications/resources/list_changed", [](const nlohmann::json& params) {
                            std::cout << "[Client] Received resources list changed notification\n";
                        });

                    // Handle prompts list changed notifications
                    client.on_notification(
                        "notifications/prompts/list_changed", [](const nlohmann::json& params) {
                            std::cout << "[Client] Received prompts list changed notification\n";
                        });

                    // ========== SUBSCRIBE TO RESOURCE ==========
                    std::cout << "[Client] Subscribing to resource: " << resource_uri << '\n';
                    nlohmann::json subscribe_params = {{"uri", resource_uri}};
                    co_await client.send_request("resources/subscribe", subscribe_params);
                    std::cout << "[Client] Subscription successful\n";

                    // ========== TRIGGER NOTIFICATIONS ==========
                    // Wait a bit, then trigger notifications from server
                    asio::steady_timer trigger_timer(io_ctx.get_executor());
                    trigger_timer.expires_after(std::chrono::milliseconds(100));
                    co_await trigger_timer.async_wait(asio::use_awaitable);

                    std::cout << "[Server] Triggering notify_resource_updated\n";
                    co_await server.notify_resource_updated(resource_uri);

                    // Wait for notification to be received
                    asio::steady_timer wait_timer(io_ctx.get_executor());
                    wait_timer.expires_after(std::chrono::milliseconds(100));
                    co_await wait_timer.async_wait(asio::use_awaitable);

                    std::cout << "[Server] Triggering notify_tools_list_changed\n";
                    co_await server.notify_tools_list_changed();

                    // Wait for notification
                    wait_timer.expires_after(std::chrono::milliseconds(100));
                    co_await wait_timer.async_wait(asio::use_awaitable);

                    std::cout << "[Server] Triggering notify_resources_list_changed\n";
                    co_await server.notify_resources_list_changed();

                    // Wait for notification
                    wait_timer.expires_after(std::chrono::milliseconds(100));
                    co_await wait_timer.async_wait(asio::use_awaitable);

                    std::cout << "[Server] Triggering notify_prompts_list_changed\n";
                    co_await server.notify_prompts_list_changed();

                    // Wait for notification
                    wait_timer.expires_after(std::chrono::milliseconds(100));
                    co_await wait_timer.async_wait(asio::use_awaitable);

                    // ========== UNSUBSCRIBE ==========
                    std::cout << "[Client] Unsubscribing from resource: " << resource_uri << '\n';
                    nlohmann::json unsubscribe_params = {{"uri", resource_uri}};
                    co_await client.send_request("resources/unsubscribe", unsubscribe_params);
                    std::cout << "[Client] Unsubscription successful\n";

                    // Wait a bit before closing
                    wait_timer.expires_after(std::chrono::milliseconds(100));
                    co_await wait_timer.async_wait(asio::use_awaitable);

                    std::cout << "[Client] Closing connection\n";
                    client.close();

                    std::cout << "[Example] Notifications and subscriptions demo complete\n";
                } catch (const std::exception& error) {
                    std::cerr << "[Client] Error: " << error.what() << '\n';
                }
            },
            asio::detached);

        io_ctx.run();
        return EXIT_SUCCESS;
    } catch (const std::exception& e) {
        std::cerr << "Exception: " << e.what() << "\n";
        return EXIT_FAILURE;
    } catch (...) {
        return EXIT_FAILURE;
    }
}

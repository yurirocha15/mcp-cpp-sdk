/// @file notifications_subscriptions.cpp
/// @brief Demonstrates server notifications delivered over Client::listen().
///
/// This example shows:
/// - Client::listen(): a subscriptions/listen stream (protocol revision 2026-07-28) that names the
///   notifications the client wants
/// - Subscription::acknowledged() and unhonored(): the subset the server agrees to send
/// - Server-side notifications: notify_tools_list_changed(), notify_resources_list_changed(),
///   notify_resource_updated(); the server sends them to every open stream that asked for the type
/// - Subscription::next() reading the notifications, close() ending a stream, and
///   Server::close_subscriptions() ending the streams from the server
/// - Subscription::end_reason() telling the two apart
///
/// The older revisions subscribe to a resource with resources/subscribe; that flow is in
/// legacy_protocols.cpp.

#include <mcp/client/client.hpp>
#include <mcp/protocol/resources.hpp>
#include <mcp/server/server.hpp>
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
#include <optional>
#include <stdexcept>
#include <string>

namespace asio = boost::asio;

namespace {

const char* yes_no(bool value) { return value ? "yes" : "no"; }

void print_filter(const char* label, const mcp::SubscriptionFilter& filter) {
    std::cout << "[Client] " << label << ": tools=" << yes_no(filter.toolsListChanged)
              << " prompts=" << yes_no(filter.promptsListChanged)
              << " resources=" << yes_no(filter.resourcesListChanged)
              << " resource URIs=" << filter.resourceSubscriptions.size() << '\n';
}

const char* end_reason_name(std::optional<mcp::SubscriptionEnd> reason) {
    if (!reason) {
        return "still open";
    }
    switch (*reason) {
        case mcp::SubscriptionEnd::eServerClosed:
            return "closed by the server";
        case mcp::SubscriptionEnd::eCancelled:
            return "cancelled by the client";
        case mcp::SubscriptionEnd::eDropped:
            return "connection dropped";
        case mcp::SubscriptionEnd::eFailed:
            return "failed";
    }
    return "unknown";
}

}  // namespace

int main() {
    try {
        using namespace mcp;

        asio::io_context io_ctx;
        bool succeeded = false;

        // ========== SERVER SETUP ==========
        // A list-changed notification is honored only for a capability that sets listChanged, and
        // resource updates only when resources.subscribe is set. This server declares no prompts
        // capability, so it cannot honor a request for prompt notifications.
        ServerCapabilities server_caps;
        ServerCapabilities::ToolsCapability tools_cap;
        tools_cap.listChanged = true;
        server_caps.tools = tools_cap;
        ServerCapabilities::ResourcesCapability resources_cap;
        resources_cap.listChanged = true;
        resources_cap.subscribe = true;
        server_caps.resources = resources_cap;

        Implementation server_info;
        server_info.name = "notifications-server";
        server_info.version = "1.0.0";

        Server server(server_info, server_caps);

        // ========== RESOURCE REGISTRATION ==========
        const std::string resource_uri = "mcp://status/counter";

        Resource resource;
        resource.uri = resource_uri;
        resource.name = "counter";
        resource.description = "A counter resource";
        resource.mimeType = "text/plain";

        server.add_resource<nlohmann::json, nlohmann::json>(
            resource, [resource_uri](const nlohmann::json&) -> nlohmann::json {
                return nlohmann::json{
                    {"contents",
                     nlohmann::json::array({nlohmann::json{
                         {"uri", resource_uri}, {"mimeType", "text/plain"}, {"text", "Counter: 1"}}})}};
            });

        // ========== TRANSPORT ==========
        auto [client_transport, server_transport] = create_memory_transport_pair(io_ctx.get_executor());

        asio::co_spawn(
            io_ctx,
            [&, transport = server_transport]() mutable -> Task<void> {
                std::cout << "[Server] Starting\n";
                try {
                    co_await server.run(transport, io_ctx.get_executor());
                } catch (const std::exception& error) {
                    std::cerr << "[Server] Error: " << error.what() << '\n';
                }
            },
            asio::detached);

        // ========== CLIENT COROUTINE ==========
        asio::co_spawn(
            io_ctx,
            [&, transport = client_transport]() -> Task<void> {
                try {
                    std::cout << "[Client] Connecting\n";
                    Client client(transport, io_ctx.get_executor());

                    Implementation client_info;
                    client_info.name = "notifications-client";
                    client_info.version = "1.0.0";

                    // Client::listen() needs a 2026-07-28 session, which Auto() gives with this
                    // server.
                    {
                        auto connected = co_await client.connect(client_info, ClientCapabilities{});
                        if (connected.era != ProtocolEra::eModern) {
                            throw std::runtime_error("expected a 2026-07-28 session");
                        }
                        std::cout << "[Client] Connected, protocol version: "
                                  << connected.protocolVersion << '\n';
                    }

                    // ========== OPEN A STREAM ==========
                    SubscriptionFilter filter;
                    filter.toolsListChanged = true;
                    filter.promptsListChanged = true;
                    filter.resourcesListChanged = true;
                    filter.resourceSubscriptions = {resource_uri};
                    print_filter("Requested", filter);

                    Subscription stream = co_await client.listen(filter);
                    print_filter("Acknowledged", stream.acknowledged());
                    print_filter("Left out by the server", stream.unhonored());
                    if (!stream.unhonored().promptsListChanged || stream.unhonored().toolsListChanged) {
                        throw std::runtime_error(
                            "expected only the prompt notifications to be left out");
                    }

                    // ========== SERVER SENDS, CLIENT READS ==========
                    std::cout << "[Server] Triggering notify_resource_updated\n";
                    co_await server.notify_resource_updated(resource_uri);
                    std::cout << "[Server] Triggering notify_tools_list_changed\n";
                    co_await server.notify_tools_list_changed();
                    std::cout << "[Server] Triggering notify_resources_list_changed\n";
                    co_await server.notify_resources_list_changed();
                    // Not asked for by anything that was honored, so no stream receives it.
                    std::cout << "[Server] Triggering notify_prompts_list_changed (nobody listens)\n";
                    co_await server.notify_prompts_list_changed();

                    const char* expected[] = {"notifications/resources/updated",
                                              "notifications/tools/list_changed",
                                              "notifications/resources/list_changed"};
                    for (const char* method : expected) {
                        std::optional<SubscriptionEvent> event = co_await stream.next();
                        if (!event) {
                            throw std::runtime_error("the stream ended before " + std::string(method));
                        }
                        std::cout << "[Client] Received " << event->method;
                        if (event->params.contains("uri")) {
                            std::cout << " for " << event->params.at("uri").dump();
                        }
                        std::cout << '\n';
                        if (event->method != method) {
                            throw std::runtime_error("unexpected notification " + event->method);
                        }
                    }

                    // ========== THE CLIENT ENDS A STREAM ==========
                    co_await stream.close();
                    std::cout << "[Client] First stream: " << end_reason_name(stream.end_reason())
                              << '\n';
                    if (stream.end_reason() != SubscriptionEnd::eCancelled) {
                        throw std::runtime_error("the first stream was not cancelled");
                    }

                    // ========== THE SERVER ENDS A STREAM ==========
                    SubscriptionFilter tools_only;
                    tools_only.toolsListChanged = true;
                    Subscription second = co_await client.listen(tools_only);
                    std::cout << "[Server] Open streams: " << server.subscription_count() << '\n';
                    server.close_subscriptions();
                    std::optional<SubscriptionEvent> last = co_await second.next();
                    std::cout << "[Client] Second stream: " << end_reason_name(second.end_reason())
                              << '\n';
                    if (last || second.end_reason() != SubscriptionEnd::eServerClosed) {
                        throw std::runtime_error("the second stream was not closed by the server");
                    }

                    std::cout << "[Client] Closing connection\n";
                    client.close();
                    succeeded = true;
                } catch (const std::exception& error) {
                    std::cerr << "[Client] Error: " << error.what() << '\n';
                }
                io_ctx.stop();
            },
            asio::detached);

        // Fail rather than hang if something never answers.
        asio::steady_timer watchdog(io_ctx.get_executor());
        watchdog.expires_after(std::chrono::seconds(10));
        watchdog.async_wait([&io_ctx](const boost::system::error_code& error) {
            if (!error) {
                io_ctx.stop();
            }
        });

        io_ctx.run();

        if (!succeeded) {
            std::cerr << "Example failed\n";
            return EXIT_FAILURE;
        }
        std::cout << "[Example] Notifications and subscriptions demo complete\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& e) {
        std::cerr << "Exception: " << e.what() << "\n";
        return EXIT_FAILURE;
    } catch (...) {
        return EXIT_FAILURE;
    }
}

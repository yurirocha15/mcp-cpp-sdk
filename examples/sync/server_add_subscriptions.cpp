// server_base.cpp plus a resource that changes: a tool updates it and tells the clients that
// subscribed to it, such as client_add_subscriptions.cpp.

#include <mcp/core/async.hpp>
#include <mcp/mcp.hpp>

#include <memory>
#include <mutex>
#include <string>

int main() {
    mcp::Server server("greeter", "1.0.0");

    server.add_tool("greet", "Greets someone by name",
                    mcp::tool_params({{"name", "string", "Who to greet"}}),
                    [](const nlohmann::json& args) -> nlohmann::json {
                        return "Hello, " + args.at("name").get<std::string>() + "!";
                    });

    const std::string last_uri = "greeter://last";
    auto last = std::make_shared<std::string>("nobody");
    auto last_mutex = std::make_shared<std::mutex>();
    server.add_text_resource(last_uri, "The last name remembered", [last, last_mutex] {
        const std::lock_guard lock(*last_mutex);
        return *last;
    });
    // Registration implies the resources capability, but not that its resources can be subscribed to.
    server.capabilities().resources->subscribe = true;
    server.add_tool("remember", "Remembers a name", mcp::tool_params({{"name", "string", "The name"}}),
                    [&server, &last_uri, last,
                     last_mutex](const nlohmann::json& args) -> mcp::Task<nlohmann::json> {
                        {
                            const std::lock_guard lock(*last_mutex);
                            *last = args.at("name").get<std::string>();
                        }
                        co_await server.notify_resource_updated(last_uri);
                        co_return "Remembered";
                    });

    server.run_stdio();
}

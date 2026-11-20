// server_base.cpp plus an MCP Apps view: an HTML page that a host which supports the extension
// shows for the result of a tool.

#include <mcp/mcp.hpp>
#include <mcp/server/apps.hpp>

#include <string>

int main() {
    mcp::Server server("greeter", "1.0.0");

    server.add_tool("greet", "Greets someone by name",
                    mcp::tool_params({{"name", "string", "Who to greet"}}),
                    [](const nlohmann::json& args) -> nlohmann::json {
                        return "Hello, " + args.at("name").get<std::string>() + "!";
                    });

    mcp::apps::ViewResource view;
    view.uri = "ui://greeter/card";
    view.name = "Greeting card";
    view.html = "<!doctype html><html><body><h1 id=\"greeting\"></h1></body></html>";
    mcp::apps::add_view_resource(server, view);

    mcp::Tool card;
    card.name = "greeting_card";
    card.description = "Shows a greeting card";
    card.inputSchema = mcp::tool_params({{"name", "string", "Who to greet"}});
    mcp::apps::add_view_tool(
        server, card, {.resourceUri = view.uri, .visibility = std::nullopt},
        [](const nlohmann::json& args) -> nlohmann::json {
            return mcp::make_tool_text_result("A card for " + args.at("name").get<std::string>());
        });

    server.run_stdio();
}

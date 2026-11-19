// A simple-mode MCP server: one tool, served over stdio.
//
// Each server_add_*.cpp file next to this one is this program plus one feature. Build it and
// let an MCP client launch it, or run it through ./client_base.

#include <mcp/mcp.hpp>

#include <string>

int main() {
    mcp::Server server("greeter", "1.0.0");

    server.add_tool("greet", "Greets someone by name",
                    mcp::tool_params({{"name", "string", "Who to greet"}}),
                    [](const nlohmann::json& args) -> nlohmann::json {
                        return "Hello, " + args.at("name").get<std::string>() + "!";
                    });

    server.run_stdio();
}

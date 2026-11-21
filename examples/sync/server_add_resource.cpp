// server_base.cpp plus a resource: text that clients read with resources/read.

#include <mcp/mcp.hpp>

#include <string>

int main() {
    mcp::Server server("greeter", "1.0.0");

    server.add_tool("greet", "Greets someone by name",
                    mcp::tool_params({{"name", "string", "Who to greet"}}),
                    [](const nlohmann::json& args) -> nlohmann::json {
                        return "Hello, " + args.at("name").get<std::string>() + "!";
                    });

    server.add_text_resource("greeter://motd", "Message of the day",
                             [] { return std::string("Be kind to each other."); });

    server.run_stdio();
}

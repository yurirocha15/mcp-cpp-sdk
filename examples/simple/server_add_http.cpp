// server_base.cpp served over Streamable HTTP instead of stdio, at http://127.0.0.1:8080/mcp
// unless a port is given on the command line.

#include <mcp/mcp.hpp>

#include <cstdint>
#include <string>

int main(int argc, char** argv) {
    mcp::Server server("greeter", "1.0.0");

    server.add_tool("greet", "Greets someone by name",
                    mcp::tool_params({{"name", "string", "Who to greet"}}),
                    [](const nlohmann::json& args) -> nlohmann::json {
                        return "Hello, " + args.at("name").get<std::string>() + "!";
                    });

    const auto port = static_cast<std::uint16_t>(argc > 1 ? std::stoi(argv[1]) : 8080);
    server.run_http("127.0.0.1", port);
}

#include <mcp/mcp.hpp>

int main() {
    mcp::ServerCapabilities caps;
    caps.tools = mcp::ServerCapabilities::ToolsCapability{};

    mcp::Implementation info{"hello-server", "1.0.0"};
    mcp::Server server(std::move(info), std::move(caps));

    nlohmann::json schema = {{"type", "object"},
                             {"properties", {{"name", {{"type", "string"}}}}},
                             {"required", nlohmann::json::array({"name"})}};

    server.add_tool("hello", "Greets the user", std::move(schema),
                    [](const nlohmann::json& args) -> nlohmann::json {
                        return {{"message", "Hello, " + args.at("name").get<std::string>() + "!"}};
                    });

    server.run_stdio();
}

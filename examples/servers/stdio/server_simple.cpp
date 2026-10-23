#include <cstdlib>
#include <iostream>
#include <mcp/mcp.hpp>

int main() {
    try {
        mcp::ServerCapabilities caps;
        caps.tools = mcp::ServerCapabilities::ToolsCapability{};

        mcp::Implementation info{"hello-server", "1.0.0"};
        mcp::Server server(info, caps);

        nlohmann::json schema = {{"type", "object"},
                                 {"properties", {{"name", {{"type", "string"}}}}},
                                 {"required", nlohmann::json::array({"name"})}};

        server.add_tool("hello", "Greets the user", schema,
                        [](const nlohmann::json& args) -> nlohmann::json {
                            return {{"message", "Hello, " + args.at("name").get<std::string>() + "!"}};
                        });

        // Serves clients of every revision the SDK supports, both 2026-07-28 requests and
        // sessions that start with initialize.
        server.run_stdio();
    } catch (const std::exception& e) {
        std::cerr << "Exception: " << e.what() << "\n";
        return EXIT_FAILURE;
    } catch (...) {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

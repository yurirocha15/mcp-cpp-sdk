// server_add_http.cpp with HTTP settings: only requests that carry the bearer token are served,
// and browser pages from one origin may call the server.

#include <mcp/mcp.hpp>
#include <mcp/transport/http_session_manager.hpp>

#include <cstdint>
#include <string>
#include <string_view>

int main(int argc, char** argv) {
    mcp::Server server("greeter", "1.0.0");

    server.add_tool("greet", "Greets someone by name",
                    mcp::tool_params({{"name", "string", "Who to greet"}}),
                    [](const nlohmann::json& args) -> nlohmann::json {
                        return "Hello, " + args.at("name").get<std::string>() + "!";
                    });

    const auto port = static_cast<std::uint16_t>(argc > 1 ? std::stoi(argv[1]) : 8080);
    server.run_http("127.0.0.1", port, [](mcp::StreamableHttpSessionManager& http) {
        http.set_bearer_token_validator(
            [](std::string_view token) { return token == "greeter-secret-token"; });
        http.set_allowed_origins({"http://localhost:3000"});
    });
}

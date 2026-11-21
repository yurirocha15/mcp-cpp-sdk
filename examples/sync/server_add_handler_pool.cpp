// server_base.cpp whose tool, resource and prompt handlers run on a pool of four threads, so
// that a slow one does not hold up the others.

#include <mcp/mcp.hpp>

#include <string>

int main() {
    mcp::Server server("greeter", "1.0.0");

    server.add_tool("greet", "Greets someone by name",
                    mcp::tool_params({{"name", "string", "Who to greet"}}),
                    [](const nlohmann::json& args) -> nlohmann::json {
                        return "Hello, " + args.at("name").get<std::string>() + "!";
                    });

    mcp::Runtime pool;
    pool.start(4);
    server.set_handler_executor(pool.executor());

    server.run_stdio();
}

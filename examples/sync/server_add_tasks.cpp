// server_base.cpp whose tool may run as a task: a client that understands tasks gets a handle at
// once and fetches the result when the tool has finished. Task handlers run on the pool of
// server_add_handler_pool.cpp, since a task's handler would otherwise run on the server's loop.

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

    server.enable_tasks();
    server.set_tool_task_mode("greet", mcp::ToolTaskMode::eAuto);

    server.run_stdio();
}

// server_base.cpp on an event loop the program runs itself, as a program that serves other
// transports or runs other work beside the server does. Only the run line changes.

#include <mcp/core/async.hpp>
#include <mcp/mcp.hpp>
#include <mcp/transport/stdio.hpp>

#include <memory>
#include <string>

int main() {
    mcp::Server server("greeter", "1.0.0");

    server.add_tool("greet", "Greets someone by name",
                    mcp::tool_params({{"name", "string", "Who to greet"}}),
                    [](const nlohmann::json& args) -> nlohmann::json {
                        return "Hello, " + args.at("name").get<std::string>() + "!";
                    });

    mcp::Runtime runtime;
    runtime.spawn(
        server.run(std::make_shared<mcp::StdioTransport>(runtime.executor()), runtime.executor()));
    runtime.run();
}

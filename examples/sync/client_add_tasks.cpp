// client_base.cpp calling the tool so that the server may run it as a task, as
// server_add_tasks.cpp allows, and waiting for the task's result.

#include <mcp/mcp.hpp>

#include <iostream>
#include <string>
#include <variant>

int main(int argc, char** argv) {
    const std::string command = argc > 1 ? argv[1] : "./greeter";
    mcp::Client client(mcp::Endpoint::stdio({command}));
    client.sync().connect("greeter-client", "1.0.0");

    const auto result = client.sync().call_tool("greet", {{"name", "Ada"}});
    std::cout << mcp::result_text(result) << "\n";
    auto started = client.sync().start_tool("greet", {{"name", "Grace"}});
    if (auto* task = std::get_if<mcp::TaskHandle>(&started)) {
        std::cout << "task: " << task->id() << "\n";
        started = task->sync().result();
    }
    std::cout << mcp::result_text(std::get<mcp::CallToolResult>(started)) << "\n";
}

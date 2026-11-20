// client_base.cpp plus a coroutine run on the client's own loop, for calls that await each other.
// The blocking calls before it stay as they are.

#include <mcp/core/async.hpp>
#include <mcp/mcp.hpp>

#include <iostream>
#include <string>

int main(int argc, char** argv) {
    const std::string command = argc > 1 ? argv[1] : "./greeter";
    mcp::Client client(mcp::Endpoint::stdio({command}));
    client.sync().connect("greeter-client", "1.0.0");

    const auto result = client.sync().call_tool("greet", {{"name", "Ada"}});
    std::cout << mcp::result_text(result) << "\n";

    client.run([&client]() -> mcp::Task<void> {
        const auto tools = co_await client.list_tools();
        for (const auto& tool : tools.tools) {
            std::cout << "tool: " << tool.name << "\n";
        }
    });
}

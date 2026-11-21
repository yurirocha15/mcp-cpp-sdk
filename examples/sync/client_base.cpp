// A sync-mode MCP client: it launches the greeter server, calls its tool and prints the answer.
//
// Each client_add_*.cpp file next to this one is this program plus one feature. The server to
// launch is ./greeter unless another command is given on the command line.

#include <mcp/mcp.hpp>

#include <iostream>
#include <string>

int main(int argc, char** argv) {
    const std::string command = argc > 1 ? argv[1] : "./greeter";
    mcp::Client client(mcp::Endpoint::stdio({command}));
    client.sync().connect("greeter-client", "1.0.0");

    const auto result = client.sync().call_tool("greet", {{"name", "Ada"}});
    std::cout << mcp::result_text(result) << "\n";
}

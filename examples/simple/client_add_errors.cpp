// client_base.cpp handling the two kinds of failure: a tool that reports an error in its result,
// and a request the server refuses, which throws mcp::McpError.

#include <mcp/mcp.hpp>

#include <iostream>
#include <string>

int main(int argc, char** argv) {
    const std::string command = argc > 1 ? argv[1] : "./greeter";
    mcp::Client client(mcp::Endpoint::stdio({command}));
    client.sync().connect("greeter-client", "1.0.0");

    const auto result = client.sync().call_tool("greet", {{"name", "Ada"}});
    std::cout << mcp::result_text(result) << "\n";
    const auto refused = client.sync().call_tool("greet", nlohmann::json::object());
    if (refused.isError.value_or(false)) {
        std::cout << "tool error: " << mcp::result_text(refused) << "\n";
    }
    try {
        client.sync().read_resource("greeter://missing");
    } catch (const mcp::McpError& error) {
        std::cout << "request error " << error.code() << "\n";
    }
}

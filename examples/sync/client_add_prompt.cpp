// client_base.cpp getting a prompt: the welcome message of server_add_prompt.cpp, filled in for Ada.

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

    const auto welcome = client.sync().get_prompt("welcome", {{"name", "Ada"}});
    std::cout << std::get<mcp::TextContent>(welcome.messages.at(0).content).text << "\n";
}

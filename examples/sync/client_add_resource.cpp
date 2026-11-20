// client_base.cpp reading a resource: the message of the day of server_add_resource.cpp.

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

    const auto motd = client.sync().read_resource("greeter://motd");
    std::cout << std::get<mcp::TextResourceContents>(motd.contents.at(0)).text << "\n";
}

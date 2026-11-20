// client_base.cpp connecting to a Streamable HTTP server, http://127.0.0.1:8080/mcp unless
// another URL is given on the command line, instead of launching one.

#include <mcp/mcp.hpp>

#include <iostream>
#include <string>

int main(int argc, char** argv) {
    const std::string url = argc > 1 ? argv[1] : "http://127.0.0.1:8080/mcp";
    mcp::Client client(mcp::Endpoint::http(url));
    client.sync().connect("greeter-client", "1.0.0");

    const auto result = client.sync().call_tool("greet", {{"name", "Ada"}});
    std::cout << mcp::result_text(result) << "\n";
}

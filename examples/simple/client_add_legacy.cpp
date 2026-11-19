// client_base.cpp speaking protocol revision 2025-11-25 and earlier only, with initialize, as a
// client of an older server would, instead of detecting the revision.

#include <mcp/mcp.hpp>

#include <iostream>
#include <string>

int main(int argc, char** argv) {
    const std::string command = argc > 1 ? argv[1] : "./greeter";
    mcp::ClientOptions options;
    options.protocol = mcp::ClientProtocol::legacy();
    mcp::Client client(mcp::Endpoint::stdio({command}), options);
    const auto connected = client.sync().connect("greeter-client", "1.0.0");
    std::cout << "protocol: " << connected.protocolVersion << "\n";

    const auto result = client.sync().call_tool("greet", {{"name", "Ada"}});
    std::cout << mcp::result_text(result) << "\n";
}

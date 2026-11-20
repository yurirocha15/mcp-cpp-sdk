// client_base.cpp printing the progress the server reports for the tool call, such as that of
// server_add_context.cpp.

#include <mcp/mcp.hpp>

#include <iostream>
#include <string>

int main(int argc, char** argv) {
    const std::string command = argc > 1 ? argv[1] : "./greeter";
    mcp::Client client(mcp::Endpoint::stdio({command}));
    client.sync().connect("greeter-client", "1.0.0");

    mcp::RequestOptions options;
    options.on_progress = [](const mcp::ProgressNotificationParams& progress) {
        std::cout << "progress: " << progress.progress << "\n";
    };
    const auto result = client.sync().call_tool("greet", {{"name", "Ada"}}, options);
    std::cout << mcp::result_text(result) << "\n";
}

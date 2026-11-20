// client_base.cpp printing the log messages the server sends while it serves the tool call, such
// as those of server_add_context.cpp.

#include <mcp/mcp.hpp>

#include <iostream>
#include <string>

int main(int argc, char** argv) {
    const std::string command = argc > 1 ? argv[1] : "./greeter";
    mcp::Client client(mcp::Endpoint::stdio({command}));
    client.on_notification("notifications/message", [](const nlohmann::json& params) {
        std::cout << "log: " << params.value("data", nlohmann::json()).dump() << "\n";
    });
    client.sync().connect("greeter-client", "1.0.0");

    mcp::RequestOptions options;
    options.log_level = mcp::LoggingLevel::eInfo;
    const auto result = client.sync().call_tool("greet", {{"name", "Ada"}}, options);
    std::cout << mcp::result_text(result) << "\n";
}

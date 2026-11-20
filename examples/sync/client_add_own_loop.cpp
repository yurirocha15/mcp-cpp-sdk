// client_base.cpp on an event loop the program runs itself. A client on the program's executor is
// used through its coroutines, so the calls move into one.

#include <mcp/core/async.hpp>
#include <mcp/mcp.hpp>
#include <mcp/transport/stdio_client.hpp>

#include <iostream>
#include <memory>
#include <string>

int main(int argc, char** argv) {
    const std::string command = argc > 1 ? argv[1] : "./greeter";
    mcp::Runtime runtime;
    mcp::StdioProcessOptions process;
    process.command = command;
    mcp::Client client(std::make_shared<mcp::StdioClientTransport>(runtime.executor(), process),
                       runtime.executor());

    const auto session = [&client]() -> mcp::Task<void> {
        co_await client.connect("greeter-client", "1.0.0");
        const nlohmann::json args{{"name", "Ada"}};
        const auto result = co_await client.call_tool("greet", args);
        std::cout << mcp::result_text(result) << "\n";
        client.close();
    };
    runtime.spawn(session());
    runtime.run();
}

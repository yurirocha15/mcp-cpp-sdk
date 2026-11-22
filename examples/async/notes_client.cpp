// The notes client of examples/sync/notes_client.cpp in async mode: the program runs the event
// loop, an mcp::Runtime, and awaits the calls in a coroutine. The server to launch is
// ./notes_server unless another command is given on the command line.

#include <mcp/core/async.hpp>
#include <mcp/mcp.hpp>
#include <mcp/transport/stdio_client.hpp>

#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <variant>

int main(int argc, char** argv) {
    // The client launches the server and runs on the program's event loop.
    mcp::Runtime runtime;
    mcp::StdioProcessOptions process;
    process.command = argc > 1 ? argv[1] : "./notes_server";
    mcp::Client client(std::make_shared<mcp::StdioClientTransport>(runtime.executor(), process),
                       runtime.executor());

    // The session: a coroutine awaits each call, then closes the client.
    const auto session = [&client]() -> mcp::Task<void> {
        co_await client.connect("notes-client", "1.0.0");

        mcp::SubscriptionFilter filter;
        filter.resourceSubscriptions = {"notes://all"};
        auto subscription = co_await client.listen(filter);
        const nlohmann::json note{{"text", "Buy milk"}};
        co_await client.call_tool("add_note", note);
        const auto event = co_await subscription.next();
        if (event) {
            std::cout << "changed: " << event->params.value("uri", std::string()) << "\n";
        }
        co_await subscription.close();

        const auto read = co_await client.read_resource("notes://all");
        std::cout << std::get<mcp::TextResourceContents>(read.contents.at(0)).text;
        const std::map<std::string, std::string> no_arguments;
        const auto prompt = co_await client.get_prompt("summarize", no_arguments);
        std::cout << std::get<mcp::TextContent>(prompt.messages.at(0).content).text << "\n";

        client.close();
    };
    runtime.spawn(session());
    runtime.run();
}

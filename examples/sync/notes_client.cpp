// A client of the notes server in sync mode: it subscribes to the notes, adds one, waits for the
// change, then reads the notes and gets the prompt. The server to launch is ./notes_server unless
// another command is given on the command line. examples/async/notes_client.cpp is the same client
// in async mode.

#include <mcp/mcp.hpp>

#include <iostream>
#include <string>
#include <variant>

int main(int argc, char** argv) {
    // The client launches the server and runs its own event loop.
    const std::string command = argc > 1 ? argv[1] : "./notes_server";
    mcp::Client client(mcp::Endpoint::stdio({command}));

    // The session: each call blocks until it is answered.
    client.sync().connect("notes-client", "1.0.0");

    mcp::SubscriptionFilter filter;
    filter.resourceSubscriptions = {"notes://all"};
    auto subscription = client.sync().listen(filter);
    client.sync().call_tool("add_note", {{"text", "Buy milk"}});
    const auto event = subscription.sync().next();
    if (event) {
        std::cout << "changed: " << event->params.value("uri", std::string()) << "\n";
    }
    subscription.sync().close();

    const auto read = client.sync().read_resource("notes://all");
    std::cout << std::get<mcp::TextResourceContents>(read.contents.at(0)).text;
    const auto prompt = client.sync().get_prompt("summarize");
    std::cout << std::get<mcp::TextContent>(prompt.messages.at(0).content).text << "\n";
}

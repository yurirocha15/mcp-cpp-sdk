// The notes server of examples/sync/notes_server.cpp in async mode: its handlers are coroutines,
// and the program runs the event loop, an mcp::Runtime, on which Server::run() serves stdio until
// standard input closes.

#include <mcp/core/async.hpp>
#include <mcp/mcp.hpp>
#include <mcp/transport/stdio.hpp>

#include <memory>
#include <string>
#include <vector>

int main() {
    mcp::Server server("notes", "1.0.0");

    std::vector<std::string> notes;
    const std::string notes_uri = "notes://all";

    // The resource: the notes, read again on each request. Clients may subscribe to it.
    server.add_text_resource(notes_uri, "All notes", [&notes]() -> mcp::Task<std::string> {
        std::string text;
        for (const auto& note : notes) {
            text += "- " + note + "\n";
        }
        co_return text;
    });
    server.capabilities().resources->subscribe = true;

    // The tool: adds a note and tells the resource's subscribers.
    server.add_tool(
        "add_note", "Adds a note",
        mcp::tool_params({{.name = "text", .type = "string", .description = "The note"}}),
        [&server, &notes, &notes_uri](const nlohmann::json& args) -> mcp::Task<nlohmann::json> {
            notes.push_back(args.at("text").get<std::string>());
            co_await server.notify_resource_updated(notes_uri);
            co_return "Added";
        });

    // The prompt: asks the model to summarize the notes.
    server.add_text_prompt("summarize", "Asks for a summary of the notes", {},
                           [&notes](const mcp::Server::PromptArguments&) -> mcp::Task<std::string> {
                               co_return "Summarize these " + std::to_string(notes.size()) + " notes.";
                           });

    // The event loop: Server::run() serves stdio on it until standard input closes.
    mcp::Runtime runtime;
    runtime.spawn(
        server.run(std::make_shared<mcp::StdioTransport>(runtime.executor()), runtime.executor()));
    runtime.run();
}

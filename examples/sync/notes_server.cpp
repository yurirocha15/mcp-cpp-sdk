// A notes server in sync mode: a tool that adds a note, a resource that lists the notes and tells
// its subscribers when they change, and a prompt that asks for a summary, served over stdio.
// examples/async/notes_server.cpp is the same server in async mode.

#include <mcp/mcp.hpp>

#include <mutex>
#include <string>
#include <vector>

int main() {
    mcp::Server server("notes", "1.0.0");

    std::mutex mutex;
    std::vector<std::string> notes;
    const std::string notes_uri = "notes://all";

    // The resource: the notes, read again on each request. Clients may subscribe to it.
    server.add_text_resource(notes_uri, "All notes", [&mutex, &notes] {
        const std::scoped_lock lock(mutex);
        std::string text;
        for (const auto& note : notes) {
            text += "- " + note + "\n";
        }
        return text;
    });
    server.capabilities().resources->subscribe = true;

    // The tool: adds a note and tells the resource's subscribers.
    server.add_tool(
        "add_note", "Adds a note",
        mcp::tool_params({{.name = "text", .type = "string", .description = "The note"}}),
        [&server, &mutex, &notes, &notes_uri](const nlohmann::json& args) -> nlohmann::json {
            {
                const std::scoped_lock lock(mutex);
                notes.push_back(args.at("text").get<std::string>());
            }
            server.sync().notify_resource_updated(notes_uri);
            return "Added";
        });

    // The prompt: asks the model to summarize the notes.
    server.add_text_prompt("summarize", "Asks for a summary of the notes", {},
                           [&mutex, &notes](const mcp::Server::PromptArguments&) {
                               const std::scoped_lock lock(mutex);
                               return "Summarize these " + std::to_string(notes.size()) + " notes.";
                           });

    // The handler pool: handlers run on two threads of their own, where server.sync() can wait for
    // the notification it sends from the server's event loop.
    mcp::Runtime pool;
    pool.start(2);
    server.set_handler_executor(pool.executor());

    // The event loop: run_stdio() runs it until standard input closes or a signal arrives.
    server.run_stdio();
}

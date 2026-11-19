// server_base.cpp plus argument completion: suggestions for the prompt's name argument.

#include <mcp/core/async.hpp>
#include <mcp/mcp.hpp>

#include <string>

int main() {
    mcp::Server server("greeter", "1.0.0");

    server.add_tool("greet", "Greets someone by name",
                    mcp::tool_params({{"name", "string", "Who to greet"}}),
                    [](const nlohmann::json& args) -> nlohmann::json {
                        return "Hello, " + args.at("name").get<std::string>() + "!";
                    });

    server.add_text_prompt("welcome", "Welcomes a new user",
                           {{.name = "name", .description = "The user's name", .required = true}},
                           [](const mcp::Server::PromptArguments& args) {
                               return "Write a short welcome message for " + args.at("name") + ".";
                           });
    server.set_completion_provider([](const mcp::CompleteParams&) -> mcp::Task<mcp::CompleteResult> {
        mcp::CompleteResult result;
        result.completion.values = {"Ada", "Grace", "Linus"};
        co_return result;
    });

    server.run_stdio();
}

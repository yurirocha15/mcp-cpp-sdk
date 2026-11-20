// server_base.cpp plus a tool that asks the user a question through the client before it
// answers. The client needs an elicitation handler, as client_add_elicitation.cpp registers.

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

    server.add_tool("greet_formally", "Greets someone by the title the user gives",
                    mcp::tool_params({{"name", "string", "Who to greet"}}),
                    [](mcp::Context& ctx, const nlohmann::json& args) -> mcp::Task<nlohmann::json> {
                        mcp::ElicitRequestFormParams form;
                        form.message = "How should they be addressed?";
                        form.requestedSchema =
                            mcp::tool_params({{"title", "string", "Such as Dr. or Ms."}});
                        const mcp::ElicitRequestParams question = form;
                        const auto answer = co_await ctx.elicit(question);
                        const auto title = answer.action == mcp::ElicitAction::eAccept && answer.content
                                               ? answer.content->value("title", std::string()) + " "
                                               : std::string();
                        co_return "Hello, " + title + args.at("name").get<std::string>() + "!";
                    });

    server.run_stdio();
}

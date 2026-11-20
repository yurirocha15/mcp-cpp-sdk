// server_base.cpp plus an Agent Skill: instructions a host can load for its model, served
// through the skills extension.

#include <mcp/mcp.hpp>
#include <mcp/server/skills.hpp>

#include <optional>
#include <string>
#include <utility>

int main() {
    mcp::Server server("greeter", "1.0.0");

    server.add_tool("greet", "Greets someone by name",
                    mcp::tool_params({{"name", "string", "Who to greet"}}),
                    [](const nlohmann::json& args) -> nlohmann::json {
                        return "Hello, " + args.at("name").get<std::string>() + "!";
                    });

    mcp::skills::SkillsExtension skills(server);
    mcp::skills::SkillDefinition greeting;
    greeting.path = "greeting";
    greeting.frontmatter = {{"name", "greeting"},
                            {"description", "How to greet people with the greet tool"}};
    greeting.files.push_back({"SKILL.md",
                              "---\nname: greeting\ndescription: How to greet people with the "
                              "greet tool\n---\nCall greet with the person's name.\n",
                              std::nullopt,
                              {}});
    skills.add_skill(std::move(greeting));

    server.run_stdio();
}

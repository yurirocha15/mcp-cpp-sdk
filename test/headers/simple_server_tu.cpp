// Compiled on its own, without <mcp/core/async.hpp>: Task is only declared here. Registering
// synchronous handlers of every kind through <mcp/mcp.hpp> must still compile.
#include <mcp/mcp.hpp>

#include <nlohmann/json.hpp>
#include <optional>
#include <string>

namespace simple_server_tu {

struct Greeting {
    std::string name;
};

inline void to_json(nlohmann::json& json_obj, const Greeting& greeting) {
    json_obj = nlohmann::json{{"name", greeting.name}};
}

inline void from_json(const nlohmann::json& json_obj, Greeting& greeting) {
    json_obj.at("name").get_to(greeting.name);
}

void register_handlers(mcp::Server& server) {
    const nlohmann::json schema{{"type", "object"}};
    server.add_tool("echo", "Echoes its arguments", schema,
                    [](const nlohmann::json& arguments) { return arguments; });
    server.add_tool<Greeting, nlohmann::json>(
        "greet", "Greets", schema, [](Greeting greeting) -> nlohmann::json {
            return nlohmann::json{{"text", "Hello, " + greeting.name}};
        });
    server.add_tool<Greeting, nlohmann::json>(
        "greet_with_context", "Greets", schema,
        [](mcp::Context& ctx, Greeting greeting) -> nlohmann::json {
            static_cast<void>(ctx.report_progress_sync(1.0));
            return nlohmann::json{{"text", "Hello, " + greeting.name}};
        });
    server.add_tool<Greeting, nlohmann::json>(
        "greet_structured", "Greets", schema, schema,
        [](Greeting greeting) -> nlohmann::json { return nlohmann::json{{"name", greeting.name}}; });

    mcp::Resource resource;
    resource.uri = "config://motd";
    resource.name = "motd";
    server.add_resource<nlohmann::json, nlohmann::json>(
        resource, [](const nlohmann::json&) -> nlohmann::json { return nlohmann::json::object(); });

    mcp::ResourceTemplate resource_template;
    resource_template.uriTemplate = "notes://{id}";
    resource_template.name = "note";
    server.add_resource_template<nlohmann::json, nlohmann::json>(
        resource_template, [](mcp::Context&, const nlohmann::json&) -> nlohmann::json {
            return nlohmann::json::object();
        });

    mcp::Prompt prompt;
    prompt.name = "review";
    server.add_prompt<nlohmann::json, nlohmann::json>(
        prompt, [](const nlohmann::json&) -> nlohmann::json { return nlohmann::json::object(); });

    const auto params = mcp::tool_params({{"name", "string", "Who to greet"}});
    server.add_tool("greet_simply", "Greets", params,
                    [](mcp::Context&, const nlohmann::json& arguments) -> nlohmann::json {
                        return arguments.at("name");
                    });
    mcp::Tool tool;
    tool.name = "described";
    tool.title = "Described";
    tool.inputSchema = params;
    server.add_tool(tool, [](const nlohmann::json& arguments) { return arguments; });
    tool.name = "described_with_context";
    server.add_tool(tool, [](mcp::Context&, const nlohmann::json& arguments) { return arguments; });
    tool.name = "raw";
    server.add_raw_tool(tool, [](const nlohmann::json&) -> nlohmann::json {
        return mcp::make_tool_text_result("raw");
    });

    server.add_text_resource("notes://motd", "motd", [] { return std::string("hello"); });
    server.add_text_resource(
        "notes://readme", "readme", [] { return std::string("# Readme"); }, "text/markdown");
    mcp::Resource text_resource;
    text_resource.uri = "notes://today";
    text_resource.name = "today";
    server.add_text_resource(text_resource, [] { return std::string("today"); });

    server.add_text_prompt("summarize", "Summarize", {{"text", "The text", true, std::nullopt}},
                           [](const mcp::Server::PromptArguments& arguments) {
                               return "Summarize " + arguments.at("text");
                           });
    mcp::Prompt text_prompt;
    text_prompt.name = "hello";
    server.add_text_prompt(text_prompt,
                           [](const mcp::Server::PromptArguments&) { return std::string("Hello"); });
}

std::string first_text(const mcp::CallToolResult& result) { return mcp::result_text(result); }

void serve(mcp::Server& server) {
    register_handlers(server);
    server.run_stdio();
}

}  // namespace simple_server_tu

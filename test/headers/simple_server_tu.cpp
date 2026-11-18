// Compiled on its own, without <mcp/core/async.hpp>: Task is only declared here. Registering
// synchronous handlers of every kind through <mcp/mcp.hpp> must still compile.
#include <mcp/mcp.hpp>

#include <nlohmann/json.hpp>
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
}

void serve(mcp::Server& server) {
    register_handlers(server);
    server.run_stdio();
}

}  // namespace simple_server_tu

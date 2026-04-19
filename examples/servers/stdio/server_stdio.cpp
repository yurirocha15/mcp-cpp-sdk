/// @file server_stdio.cpp
/// @brief Full-featured MCP server over stdio showing all 4 handler signatures,
///   custom JsonSerializable types, resources, templates, prompts, and context logging.

#include <mcp/server.hpp>
#include <mcp/transport/stdio.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <cstdlib>
#include <string>

struct AddInput {
    double a;
    double b;
};

inline void to_json(nlohmann::json& j, const AddInput& v) {
    j = nlohmann::json{{"a", v.a}, {"b", v.b}};
}

inline void from_json(const nlohmann::json& j, AddInput& v) {
    j.at("a").get_to(v.a);
    j.at("b").get_to(v.b);
}

struct AddOutput {
    double result;
};

inline void to_json(nlohmann::json& j, const AddOutput& v) { j = nlohmann::json{{"result", v.result}}; }

inline void from_json(const nlohmann::json& j, AddOutput& v) { j.at("result").get_to(v.result); }

struct GreetInput {
    std::string name;
};

inline void to_json(nlohmann::json& j, const GreetInput& v) { j = nlohmann::json{{"name", v.name}}; }

inline void from_json(const nlohmann::json& j, GreetInput& v) { j.at("name").get_to(v.name); }

struct GreetOutput {
    std::string message;
};

inline void to_json(nlohmann::json& j, const GreetOutput& v) {
    j = nlohmann::json{{"message", v.message}};
}

inline void from_json(const nlohmann::json& j, GreetOutput& v) { j.at("message").get_to(v.message); }

int main() {
    try {
        using namespace mcp;

        ServerCapabilities caps;
        caps.tools = ServerCapabilities::ToolsCapability{.listChanged = true};
        caps.resources = ServerCapabilities::ResourcesCapability{.listChanged = true};
        caps.prompts = ServerCapabilities::PromptsCapability{.listChanged = true};
        caps.logging = nlohmann::json::object();

        Implementation info;
        info.name = "example-server";
        info.version = "1.0.0";

        Server server(info, caps);

        // Tool: add — sync handler, no context
        nlohmann::json add_schema = {
            {"type", "object"},
            {"properties", {{"a", {{"type", "number"}}}, {"b", {{"type", "number"}}}}},
            {"required", nlohmann::json::array({"a", "b"})}};

        server.add_tool<AddInput, AddOutput>(
            "add", "Add two numbers", add_schema,
            [](AddInput in) -> AddOutput { return AddOutput{.result = in.a + in.b}; });

        // Tool: greet — sync handler, with context
        nlohmann::json greet_schema = {{"type", "object"},
                                       {"properties", {{"name", {{"type", "string"}}}}},
                                       {"required", nlohmann::json::array({"name"})}};

        server.add_tool<GreetInput, GreetOutput>(
            "greet", "Greet someone by name", greet_schema,
            [](Context& /*ctx*/, const GreetInput& in) -> GreetOutput {
                return GreetOutput{.message = "Hello, " + in.name + "!"};
            });

        // Tool: echo_async — async handler, no context
        nlohmann::json echo_schema = {{"type", "object"},
                                      {"properties", {{"text", {{"type", "string"}}}}},
                                      {"required", nlohmann::json::array({"text"})}};

        server.add_tool<nlohmann::json, nlohmann::json>(
            "echo_async", "Echo the input asynchronously", echo_schema,
            [](nlohmann::json in) -> Task<nlohmann::json> {
                nlohmann::json out = {{"echo", in.at("text")}};
                co_return out;
            });

        // Tool: log_and_upper — async handler, with context (demonstrates ctx.log_info)
        nlohmann::json upper_schema = {{"type", "object"},
                                       {"properties", {{"text", {{"type", "string"}}}}},
                                       {"required", nlohmann::json::array({"text"})}};

        server.add_tool<nlohmann::json, nlohmann::json>(
            "log_and_upper", "Uppercase the input and log via context", upper_schema,
            [](Context& ctx, nlohmann::json in) -> Task<nlohmann::json> {
                auto text = in.at("text").get<std::string>();
                co_await ctx.log_info("Uppercasing: " + text);
                for (auto& ch : text) {
                    ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
                }
                nlohmann::json out = {{"result", text}};
                co_return out;
            });

        // Resource: config://app/settings
        Resource config_resource;
        config_resource.uri = "config://app/settings";
        config_resource.name = "App Settings";
        config_resource.description = "Current application settings";
        config_resource.mimeType = "application/json";

        server.add_resource<ReadResourceRequestParams, ReadResourceResult>(
            config_resource, [](const ReadResourceRequestParams& params) -> ReadResourceResult {
                TextResourceContents contents;
                contents.uri = params.uri;
                contents.text = R"({"theme":"dark","language":"en"})";
                contents.mimeType = "application/json";
                ReadResourceResult result;
                result.contents.emplace_back(std::move(contents));
                return result;
            });

        // Resource template: users://{user_id}/profile
        ResourceTemplate user_template;
        user_template.uriTemplate = "users://{user_id}/profile";
        user_template.name = "User Profile";
        user_template.description = "Retrieve a user's profile by ID";
        user_template.mimeType = "application/json";

        server.add_resource_template(user_template);

        // Prompt: code_review (parameterized)
        Prompt review_prompt;
        review_prompt.name = "code_review";
        review_prompt.description = "Generate a code review prompt for the given language and code";

        PromptArgument lang_arg;
        lang_arg.name = "language";
        lang_arg.description = "Programming language";
        lang_arg.required = true;

        PromptArgument code_arg;
        code_arg.name = "code";
        code_arg.description = "The code snippet to review";
        code_arg.required = true;

        review_prompt.arguments = std::vector<PromptArgument>{std::move(lang_arg), std::move(code_arg)};

        server.add_prompt<GetPromptRequestParams, GetPromptResult>(
            review_prompt, [](GetPromptRequestParams params) -> GetPromptResult {
                std::string language = "unknown";
                std::string code;
                if (params.arguments) {
                    if (auto it = params.arguments->find("language"); it != params.arguments->end()) {
                        language = it->second;
                    }
                    if (auto it = params.arguments->find("code"); it != params.arguments->end()) {
                        code = it->second;
                    }
                }

                PromptMessage msg;
                msg.role = Role::eUser;
                TextContent text_content;
                text_content.text = "Please review the following " + language + " code:\n\n```" +
                                    language + "\n" + code + "\n```";
                msg.content = std::move(text_content);

                GetPromptResult result;
                result.description = "Code review prompt for " + language;
                result.messages.push_back(std::move(msg));
                return result;
            });

        boost::asio::io_context io_ctx;
        auto transport = std::make_shared<StdioTransport>(io_ctx.get_executor());

        boost::asio::co_spawn(
            io_ctx, [&]() -> Task<void> { co_await server.run(transport, io_ctx.get_executor()); },
            boost::asio::detached);

        io_ctx.run();
        return EXIT_SUCCESS;
    } catch (const std::exception& e) {
        std::cerr << "Exception: " << e.what() << "\n";
        return EXIT_FAILURE;
    } catch (...) {
        return EXIT_FAILURE;
    }
}

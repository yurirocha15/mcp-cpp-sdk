// An MCP server with every feature, in sync mode: handlers are plain functions unless they await
// the client, and run_http() runs the event loop, serving HTTP with a bearer token and an allowed
// origin until SIGINT or SIGTERM. examples/async/server.cpp is the same server in async mode, and
// client.cpp talks to it.

#include <mcp/core/async.hpp>
#include <mcp/mcp.hpp>
#include <mcp/server/apps.hpp>
#include <mcp/server/skills.hpp>
#include <mcp/transport/http_session_manager.hpp>

#include <atomic>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

int main(int argc, char** argv) {
    mcp::Server server("greeter", "1.0.0");

    // Handler pool: tool, resource and prompt handlers run on four threads of their own, and the
    // server's own work stays on its event loop.
    mcp::Runtime pool;
    pool.start(4);
    server.set_handler_executor(pool.executor());

    // Tools.
    server.add_tool("greet", "Greets someone by name",
                    mcp::tool_params({{"name", "string", "Who to greet"}}),
                    [](const nlohmann::json& args) -> nlohmann::json {
                        return "Hello, " + args.at("name").get<std::string>() + "!";
                    });

    // Logging and progress. A server that logs declares the logging capability.
    server.capabilities().logging = nlohmann::json::object();
    server.add_tool("compose_greeting", "Composes a greeting, logging and reporting progress",
                    mcp::tool_params({{"name", "string", "Who to greet"}}),
                    [](mcp::Context& ctx, const nlohmann::json& args) -> mcp::Task<nlohmann::json> {
                        co_await ctx.log_info("Composing a greeting");
                        co_await ctx.report_progress(1, 1);
                        co_return "Hello, " + args.at("name").get<std::string>() + "!";
                    });

    // Resources.
    server.add_text_resource("greeter://motd", "Message of the day",
                             [] { return std::string("Be kind to each other."); });

    // Resource subscriptions: the remember tool changes a resource and tells its subscribers.
    // Registering a resource declares the resources capability; this flag adds subscriptions.
    const std::string last_uri = "greeter://last";
    auto last = std::make_shared<std::string>("nobody");
    auto last_mutex = std::make_shared<std::mutex>();
    server.add_text_resource(last_uri, "The last name remembered", [last, last_mutex] {
        const std::lock_guard lock(*last_mutex);
        return *last;
    });
    server.capabilities().resources->subscribe = true;
    server.add_tool(
        "remember", "Remembers a name", mcp::tool_params({{"name", "string", "The name"}}),
        [&server, &last_uri, last, last_mutex](const nlohmann::json& args) -> nlohmann::json {
            {
                const std::lock_guard lock(*last_mutex);
                *last = args.at("name").get<std::string>();
            }
            server.sync().notify_resource_updated(last_uri);
            return "Remembered";
        });

    // Prompts.
    server.add_text_prompt("welcome", "Welcomes a new user",
                           {{.name = "name", .description = "The user's name", .required = true}},
                           [](const mcp::Server::PromptArguments& args) {
                               return "Write a short welcome message for " + args.at("name") + ".";
                           });

    // Completions.
    server.set_completion_provider([](const mcp::CompleteParams&) -> mcp::Task<mcp::CompleteResult> {
        mcp::CompleteResult result;
        result.completion.values = {"Ada", "Grace", "Linus"};
        co_return result;
    });

    // Elicitation: a tool asks the user, through the client, to fill in a form.
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

    // Sampling: a tool asks the client's model for a reply.
    server.add_tool(
        "write_greeting", "Asks the client's model to write a greeting",
        mcp::tool_params({{"name", "string", "Who to greet"}}),
        [](mcp::Context& ctx, const nlohmann::json& args) -> mcp::Task<nlohmann::json> {
            mcp::CreateMessageRequestParams request;
            request.maxTokens = 100;
            request.messages.push_back(
                {mcp::Role::eUser,
                 mcp::TextContent{.text = "Write a greeting for " + args.at("name").get<std::string>()},
                 std::nullopt});
            const auto reply = co_await ctx.sample_llm(request);
            const auto* block = std::get_if<mcp::ContentBlock>(&reply.content);
            const auto* text = block != nullptr ? std::get_if<mcp::TextContent>(block) : nullptr;
            co_return text != nullptr ? text->text : std::string("(no text)");
        });

    // Roots: a tool asks for the client's roots.
    server.add_tool("count_roots", "Counts the client's roots", nlohmann::json{{"type", "object"}},
                    [](mcp::Context& ctx, const nlohmann::json&) -> mcp::Task<nlohmann::json> {
                        const auto listed = co_await ctx.request_roots();
                        co_return "The client has " + std::to_string(listed.roots.size()) + " root(s)";
                    });

    // Middleware: each request that an application handler answers passes through it.
    std::atomic<int> calls{0};
    server.use([&calls](mcp::Context& ctx, const nlohmann::json& params,
                        mcp::TypeErasedHandler next) -> mcp::Task<nlohmann::json> {
        std::cerr << "handler call " << ++calls << "\n";
        co_return co_await next(ctx, params);
    });

    // Pagination: lists answer two items at a time.
    server.set_page_size(2);

    // Tasks: a call of greet may become a task, whose handler runs on the handler pool.
    server.enable_tasks();
    server.set_tool_task_mode("greet", mcp::ToolTaskMode::eAuto);

    // MCP Apps: an HTML view, and a tool linked to it.
    mcp::apps::ViewResource view;
    view.uri = "ui://greeter/card";
    view.name = "Greeting card";
    view.html = "<!doctype html><html><body><h1 id=\"greeting\"></h1></body></html>";
    mcp::apps::add_view_resource(server, view);

    mcp::Tool card;
    card.name = "greeting_card";
    card.description = "Shows a greeting card";
    card.inputSchema = mcp::tool_params({{"name", "string", "Who to greet"}});
    mcp::apps::add_view_tool(
        server, card, {.resourceUri = view.uri, .visibility = std::nullopt},
        [](const nlohmann::json& args) -> nlohmann::json {
            return mcp::make_tool_text_result("A card for " + args.at("name").get<std::string>());
        });

    // Agent Skills.
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

    // HTTP until a signal: run_http() runs the event loop and handles SIGINT and SIGTERM, and
    // configures the session manager it serves through before it listens.
    const auto port = static_cast<std::uint16_t>(argc > 1 ? std::stoi(argv[1]) : 8080);
    const auto configure = [](mcp::StreamableHttpSessionManager& http) {
        http.set_bearer_token_validator(
            [](std::string_view token) { return token == "greeter-secret-token"; });
        http.set_allowed_origins({"http://localhost:3000"});
    };
    server.run_http("127.0.0.1", port, configure);
}

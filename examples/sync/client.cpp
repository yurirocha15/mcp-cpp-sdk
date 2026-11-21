// An MCP client with every feature, in sync mode: the client runs its own event loop, and each call
// blocks until it is answered. examples/async/client.cpp is the same client in async mode. It
// talks to server.cpp.

#include <mcp/core/async.hpp>
#include <mcp/mcp.hpp>
#include <mcp/transport/http_client.hpp>

#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>

// Every feature of the client, one blocking call after another.
void use_every_feature(mcp::Client& client) {
    // Tools.
    const auto greeted = client.sync().call_tool("greet", {{"name", "Ada"}});
    std::cout << mcp::result_text(greeted) << "\n";

    // Logging and progress: the options ask for the call's progress and log messages.
    mcp::RequestOptions options;
    options.on_progress = [](const mcp::ProgressNotificationParams& progress) {
        std::cout << "progress: " << progress.progress << "\n";
    };
    options.log_level = mcp::LoggingLevel::eInfo;
    const auto composed = client.sync().call_tool("compose_greeting", {{"name", "Ada"}}, options);
    std::cout << mcp::result_text(composed) << "\n";

    // Resources.
    const auto motd = client.sync().read_resource("greeter://motd");
    std::cout << std::get<mcp::TextResourceContents>(motd.contents.at(0)).text << "\n";

    // Prompts.
    const auto welcome = client.sync().get_prompt("welcome", {{"name", "Ada"}});
    std::cout << std::get<mcp::TextContent>(welcome.messages.at(0).content).text << "\n";

    // Completions.
    mcp::CompleteParams completing;
    completing.ref = mcp::PromptReference{.name = "welcome"};
    completing.argument = {.name = "name", .value = "A"};
    const auto completed = client.sync().complete(completing);
    std::cout << "completion: " << completed.completion.values.at(0) << "\n";

    // Elicitation, sampling and roots: tools that ask this client's handlers.
    const auto formal = client.sync().call_tool("greet_formally", {{"name", "Ada"}});
    std::cout << mcp::result_text(formal) << "\n";
    const auto written = client.sync().call_tool("write_greeting", {{"name", "Ada"}});
    std::cout << mcp::result_text(written) << "\n";
    const auto roots = client.sync().call_tool("count_roots", nlohmann::json::object());
    std::cout << mcp::result_text(roots) << "\n";

    // Tasks: start_tool() returns a handle when the server makes the call a task.
    auto started = client.sync().start_tool("greet", {{"name", "Grace"}});
    if (auto* task = std::get_if<mcp::TaskHandle>(&started)) {
        std::cout << "task: " << task->id() << "\n";
        started = task->sync().result();
    }
    std::cout << mcp::result_text(std::get<mcp::CallToolResult>(started)) << "\n";

    // Resource subscriptions: the remember tool changes the resource.
    mcp::SubscriptionFilter filter;
    filter.resourceSubscriptions = {"greeter://last"};
    auto subscription = client.sync().listen(filter);
    if (!subscription.unhonored().resourceSubscriptions.empty()) {
        throw std::runtime_error("the server does not offer subscriptions to greeter://last");
    }
    client.sync().call_tool("remember", {{"name", "Ada"}});
    const auto event = subscription.sync().next();
    if (event) {
        std::cout << "updated: " << event->method << "\n";
    }
    subscription.sync().close();

    // Errors: a tool error is a result; a request error is an McpError.
    const auto refused = client.sync().call_tool("greet", nlohmann::json::object());
    if (refused.isError.value_or(false)) {
        std::cout << "tool error: " << mcp::result_text(refused) << "\n";
    }
    try {
        client.sync().read_resource("greeter://missing");
    } catch (const mcp::McpError& error) {
        std::cout << "request error " << error.code() << "\n";
    }

    // Pagination: each page's nextCursor asks for the next one.
    std::optional<std::string> cursor;
    do {
        const auto page = client.sync().list_tools(cursor);
        for (const auto& tool : page.tools) {
            std::cout << "tool: " << tool.name << "\n";
        }
        cursor = page.nextCursor;
    } while (cursor);
}

int main(int argc, char** argv) {
    const std::string url = argc > 1 ? argv[1] : "http://127.0.0.1:8080/mcp";

    // The connection: the client runs its own event loop, and its transport sends a bearer token.
    mcp::HttpEndpointOptions http;
    http.configure = [](mcp::HttpClientTransport& transport) {
        transport.set_bearer_token_provider([] { return std::string("greeter-secret-token"); });
    };
    mcp::Client client(mcp::Endpoint::http(url, http));

    // Logging: the server's log messages arrive as notifications.
    client.on_notification("notifications/message", [](const nlohmann::json& params) {
        std::cout << "log: " << params.value("data", nlohmann::json()).dump() << "\n";
    });

    // Elicitation: this handler answers the server's questions for the user.
    client.on_elicitation([](const mcp::ElicitRequestParams&) -> mcp::Task<mcp::ElicitResult> {
        co_return mcp::ElicitResult{mcp::ElicitAction::eAccept, nlohmann::json{{"title", "Dr."}},
                                    std::nullopt};
    });

    // Sampling: this handler answers the server's requests for model output.
    client.on_sampling(
        [](const mcp::CreateMessageRequestParams&) -> mcp::Task<mcp::CreateMessageResult> {
            mcp::CreateMessageResult reply;
            reply.role = mcp::Role::eAssistant;
            reply.content = mcp::ContentBlock{mcp::TextContent{.text = "Greetings, friend!"}};
            reply.model = "example-model";
            co_return reply;
        });

    // Roots: this handler lists the client's roots.
    client.on_roots_list([](const nlohmann::json&) -> mcp::Task<mcp::ListRootsResult> {
        mcp::ListRootsResult roots;
        roots.roots.push_back({"file:///home/user/projects", "projects", std::nullopt});
        co_return roots;
    });

    // The session: connect, then use every feature.
    client.sync().connect("greeter-client", "1.0.0");
    use_every_feature(client);

    // Coroutines on the client's own event loop: client.run() blocks until the coroutine ends.
    const auto greet_in_a_coroutine = [&client]() -> mcp::Task<void> {
        const nlohmann::json linus{{"name", "Linus"}};
        const auto result = co_await client.call_tool("greet", linus);
        std::cout << "from a coroutine: " << mcp::result_text(result) << "\n";
    };
    client.run(greet_in_a_coroutine);
}

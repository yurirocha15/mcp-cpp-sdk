// client_base.cpp with every add-on that combines with the others: a resource and a prompt, HTTP
// with a bearer token, log messages and progress for a call, handlers for the server's questions,
// model requests and roots, a tool run as a task, a resource subscription, error handling, and a
// coroutine on the client's loop. It talks to server_all.cpp.

#include <mcp/core/async.hpp>
#include <mcp/mcp.hpp>
#include <mcp/transport/http_client.hpp>

#include <iostream>
#include <string>
#include <variant>

int main(int argc, char** argv) {
    const std::string url = argc > 1 ? argv[1] : "http://127.0.0.1:8080/mcp";
    mcp::HttpEndpointOptions http;
    http.configure = [](mcp::HttpClientTransport& transport) {
        transport.set_bearer_token_provider([] { return std::string("greeter-secret-token"); });
    };
    mcp::Client client(mcp::Endpoint::http(url, http));
    client.on_notification("notifications/message", [](const nlohmann::json& params) {
        std::cout << "log: " << params.value("data", nlohmann::json()).dump() << "\n";
    });
    client.on_elicitation([](const mcp::ElicitRequestParams&) -> mcp::Task<mcp::ElicitResult> {
        co_return mcp::ElicitResult{mcp::ElicitAction::eAccept, nlohmann::json{{"title", "Dr."}},
                                    std::nullopt};
    });
    client.on_sampling(
        [](const mcp::CreateMessageRequestParams&) -> mcp::Task<mcp::CreateMessageResult> {
            mcp::CreateMessageResult reply;
            reply.role = mcp::Role::eAssistant;
            reply.content = mcp::ContentBlock{mcp::TextContent{.text = "Greetings, friend!"}};
            reply.model = "example-model";
            co_return reply;
        });
    client.on_roots_list([](const nlohmann::json&) -> mcp::Task<mcp::ListRootsResult> {
        mcp::ListRootsResult roots;
        roots.roots.push_back({"file:///home/user/projects", "projects", std::nullopt});
        co_return roots;
    });
    client.sync().connect("greeter-client", "1.0.0");

    mcp::RequestOptions options;
    options.on_progress = [](const mcp::ProgressNotificationParams& progress) {
        std::cout << "progress: " << progress.progress << "\n";
    };
    options.log_level = mcp::LoggingLevel::eInfo;
    const auto result = client.sync().call_tool("greet", {{"name", "Ada"}}, options);
    std::cout << mcp::result_text(result) << "\n";
    const auto motd = client.sync().read_resource("greeter://motd");
    std::cout << std::get<mcp::TextResourceContents>(motd.contents.at(0)).text << "\n";
    const auto welcome = client.sync().get_prompt("welcome", {{"name", "Ada"}});
    std::cout << std::get<mcp::TextContent>(welcome.messages.at(0).content).text << "\n";
    std::cout << mcp::result_text(client.sync().call_tool("greet_formally", {{"name", "Ada"}})) << "\n";
    std::cout << mcp::result_text(client.sync().call_tool("write_greeting", {{"name", "Ada"}})) << "\n";
    std::cout << mcp::result_text(client.sync().call_tool("count_roots", nlohmann::json::object()))
              << "\n";

    auto started = client.sync().start_tool("greet", {{"name", "Grace"}});
    if (auto* task = std::get_if<mcp::TaskHandle>(&started)) {
        std::cout << "task: " << task->id() << "\n";
        started = task->sync().result();
    }
    std::cout << mcp::result_text(std::get<mcp::CallToolResult>(started)) << "\n";

    mcp::SubscriptionFilter filter;
    filter.resourceSubscriptions = {"greeter://last"};
    auto subscription = client.sync().listen(filter);
    if (!subscription.unhonored().resourceSubscriptions.empty()) {
        std::cout << "the server does not offer subscriptions to greeter://last\n";
        return 1;
    }
    client.sync().call_tool("remember", {{"name", "Ada"}});
    if (auto event = subscription.sync().next()) {
        std::cout << "updated: " << event->method << "\n";
    }

    const auto refused = client.sync().call_tool("greet", nlohmann::json::object());
    if (refused.isError.value_or(false)) {
        std::cout << "tool error: " << mcp::result_text(refused) << "\n";
    }
    try {
        client.sync().read_resource("greeter://missing");
    } catch (const mcp::McpError& error) {
        std::cout << "request error " << error.code() << "\n";
    }

    client.run([&client]() -> mcp::Task<void> {
        const auto tools = co_await client.list_tools();
        for (const auto& tool : tools.tools) {
            std::cout << "tool: " << tool.name << "\n";
        }
    });
}

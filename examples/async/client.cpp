// An MCP client with every feature, in async mode: the program runs the event loop, and every call
// is awaited in a coroutine, two of them in flight at the same time. examples/sync/client.cpp is
// the same client in sync mode. It talks to server.cpp.

#include <mcp/core/async.hpp>
#include <mcp/mcp.hpp>
#include <mcp/transport/http_client.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>

#include <exception>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>

// Every feature of the client, one awaited call after another. The arguments of each co_await are
// built in named variables first, as GCC 11 to 13 need.
mcp::Task<void> use_every_feature(mcp::Client& client) {
    const nlohmann::json ada{{"name", "Ada"}};

    // Tools.
    const auto greeted = co_await client.call_tool("greet", ada);
    std::cout << mcp::result_text(greeted) << "\n";

    // Logging and progress: the options ask for the call's progress and log messages.
    mcp::RequestOptions options;
    options.on_progress = [](const mcp::ProgressNotificationParams& progress) {
        std::cout << "progress: " << progress.progress << "\n";
    };
    options.log_level = mcp::LoggingLevel::eInfo;
    const auto composed = co_await client.call_tool("compose_greeting", ada, options);
    std::cout << mcp::result_text(composed) << "\n";

    // Resources.
    const auto motd = co_await client.read_resource("greeter://motd");
    std::cout << std::get<mcp::TextResourceContents>(motd.contents.at(0)).text << "\n";

    // Prompts.
    const std::map<std::string, std::string> prompt_args{{"name", "Ada"}};
    const auto welcome = co_await client.get_prompt("welcome", prompt_args);
    std::cout << std::get<mcp::TextContent>(welcome.messages.at(0).content).text << "\n";

    // Completions.
    mcp::CompleteParams completing;
    completing.ref = mcp::PromptReference{.name = "welcome"};
    completing.argument = {.name = "name", .value = "A"};
    const auto completed = co_await client.complete(completing);
    std::cout << "completion: " << completed.completion.values.at(0) << "\n";

    // Elicitation, sampling and roots: tools that ask this client's handlers.
    const auto formal = co_await client.call_tool("greet_formally", ada);
    std::cout << mcp::result_text(formal) << "\n";
    const auto written = co_await client.call_tool("write_greeting", ada);
    std::cout << mcp::result_text(written) << "\n";
    const nlohmann::json no_arguments = nlohmann::json::object();
    const auto roots = co_await client.call_tool("count_roots", no_arguments);
    std::cout << mcp::result_text(roots) << "\n";

    // Tasks: start_tool() returns a handle when the server makes the call a task.
    const nlohmann::json grace{{"name", "Grace"}};
    auto started = co_await client.start_tool("greet", grace);
    if (auto* task = std::get_if<mcp::TaskHandle>(&started)) {
        std::cout << "task: " << task->id() << "\n";
        started = co_await task->result();
    }
    std::cout << mcp::result_text(std::get<mcp::CallToolResult>(started)) << "\n";

    // Resource subscriptions: the remember tool changes the resource.
    mcp::SubscriptionFilter filter;
    filter.resourceSubscriptions = {"greeter://last"};
    auto subscription = co_await client.listen(filter);
    if (!subscription.unhonored().resourceSubscriptions.empty()) {
        throw std::runtime_error("the server does not offer subscriptions to greeter://last");
    }
    co_await client.call_tool("remember", ada);
    const auto event = co_await subscription.next();
    if (event) {
        std::cout << "updated: " << event->method << "\n";
    }
    co_await subscription.close();

    // Errors: a tool error is a result; a request error is an McpError.
    const auto refused = co_await client.call_tool("greet", no_arguments);
    if (refused.isError.value_or(false)) {
        std::cout << "tool error: " << mcp::result_text(refused) << "\n";
    }
    try {
        co_await client.read_resource("greeter://missing");
    } catch (const mcp::McpError& error) {
        std::cout << "request error " << error.code() << "\n";
    }

    // Pagination: each page's nextCursor asks for the next one.
    std::optional<std::string> cursor;
    do {
        const auto page = co_await client.list_tools(cursor);
        for (const auto& tool : page.tools) {
            std::cout << "tool: " << tool.name << "\n";
        }
        cursor = page.nextCursor;
    } while (cursor);
}

int main(int argc, char** argv) {
    const std::string url = argc > 1 ? argv[1] : "http://127.0.0.1:8080/mcp";

    // The connection: the transport and the client run on the program's event loop, and the
    // transport sends a bearer token.
    boost::asio::io_context io;
    auto transport = std::make_shared<mcp::HttpClientTransport>(io.get_executor(), url);
    transport->set_bearer_token_provider([] { return std::string("greeter-secret-token"); });
    mcp::Client client(transport, io.get_executor());

    // Logging: the server's log messages arrive as notifications.
    client.on_notification("notifications/message", [](const nlohmann::json& params) {
        std::cout << "log: " << params.value("data", nlohmann::json()).dump() << "\n";
    });

    // Elicitation: this handler answers the server's questions for the user.
    client.on_elicitation([](const mcp::ElicitRequestParams&) -> mcp::Task<mcp::ElicitResult> {
        co_return mcp::ElicitResult{
            .action = mcp::ElicitAction::eAccept,
            .content = nlohmann::json{{"title", "Dr."}},
            .meta = std::nullopt,
        };
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
        roots.roots.push_back(
            {.uri = "file:///home/user/projects", .name = "projects", .meta = std::nullopt});
        co_return roots;
    });

    // co_spawn hands what a coroutine throws to this handler; rethrowing it makes io.run() throw.
    const auto rethrow = [](const std::exception_ptr& failure) {
        if (failure) {
            std::rethrow_exception(failure);
        }
    };

    // Two calls in flight at the same time: each runs in a coroutine of its own, so neither waits
    // for the other's answer. The client closes once both have returned.
    const auto greet_together = [&io, &client, rethrow] {
        auto calls_left = std::make_shared<int>(2);
        for (const char* name : {"Ada", "Grace"}) {
            boost::asio::co_spawn(
                io,
                [&client, calls_left, args = nlohmann::json{{"name", name}}]() -> mcp::Task<void> {
                    const auto result = co_await client.call_tool("greet", args);
                    std::cout << "together: " << mcp::result_text(result) << "\n";
                    if (--*calls_left == 0) {
                        client.close();
                    }
                },
                rethrow);
        }
    };

    // The session: connect, use every feature, then make the two calls together.
    boost::asio::co_spawn(
        io,
        [&client, greet_together]() -> mcp::Task<void> {
            co_await client.connect("greeter-client", "1.0.0");
            co_await use_every_feature(client);
            greet_together();
        },
        rethrow);
    io.run();
}

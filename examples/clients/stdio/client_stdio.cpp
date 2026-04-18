/// @file client_stdio.cpp
/// @brief MCP client over stdio demonstrating connect, list/call tools,
///   list/read resources, list templates, list/get prompts, complete, and notifications.

#include <mcp/client.hpp>
#include <mcp/transport/stdio.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <cstdlib>
#include <iostream>
#include <map>
#include <string>

struct AddArgs {
    double a;
    double b;
};

inline void to_json(nlohmann::json& j, const AddArgs& v) { j = nlohmann::json{{"a", v.a}, {"b", v.b}}; }

inline void from_json(const nlohmann::json& j, AddArgs& v) {
    j.at("a").get_to(v.a);
    j.at("b").get_to(v.b);
}

int main() {
    using namespace mcp;

    boost::asio::io_context io_ctx;
    auto transport = std::make_shared<StdioTransport>(io_ctx.get_executor());
    Client client(transport, io_ctx.get_executor());

    boost::asio::co_spawn(
        io_ctx,
        [&]() -> Task<void> {
            Implementation client_info;
            client_info.name = "example-client";
            client_info.version = "1.0.0";

            ClientCapabilities caps;
            caps.sampling = ClientCapabilities::SamplingCapability{};

            auto init_result = co_await client.connect(std::move(client_info), std::move(caps));
            std::cout << "Connected to: " << init_result.serverInfo.name << " "
                      << init_result.serverInfo.version << "\n";

            // list_tools
            auto tools = co_await client.list_tools();
            std::cout << "Tools (" << tools.tools.size() << "):\n";
            for (const auto& tool : tools.tools) {
                std::cout << "  - " << tool.name << "\n";
            }

            // call_tool with typed arguments
            auto add_result = co_await client.call_tool("add", AddArgs{.a = 3.0, .b = 4.0});
            for (const auto& block : add_result.content) {
                if (auto* text = std::get_if<TextContent>(&block)) {
                    std::cout << "add(3, 4) = " << text->text << "\n";
                }
            }

            // call_tool with raw JSON arguments
            nlohmann::json echo_args = {{"text", "hello"}};
            auto echo_result = co_await client.call_tool("echo_async", std::move(echo_args));
            for (const auto& block : echo_result.content) {
                if (auto* text = std::get_if<TextContent>(&block)) {
                    std::cout << "echo_async: " << text->text << "\n";
                }
            }

            // list_resources
            auto resources = co_await client.list_resources();
            std::cout << "Resources (" << resources.resources.size() << "):\n";
            for (const auto& res : resources.resources) {
                std::cout << "  - " << res.uri << " (" << res.name << ")\n";
            }

            // read_resource
            if (!resources.resources.empty()) {
                auto read_result = co_await client.read_resource(resources.resources[0].uri);
                for (const auto& content : read_result.contents) {
                    if (auto* text = std::get_if<TextResourceContents>(&content)) {
                        std::cout << "Resource content: " << text->text << "\n";
                    }
                }
            }

            // list_resource_templates
            auto templates = co_await client.list_resource_templates();
            std::cout << "Resource templates (" << templates.resourceTemplates.size() << "):\n";
            for (const auto& tmpl : templates.resourceTemplates) {
                std::cout << "  - " << tmpl.uriTemplate << " (" << tmpl.name << ")\n";
            }

            // list_prompts
            auto prompts = co_await client.list_prompts();
            std::cout << "Prompts (" << prompts.prompts.size() << "):\n";
            for (const auto& prompt : prompts.prompts) {
                std::cout << "  - " << prompt.name << "\n";
            }

            // get_prompt
            if (!prompts.prompts.empty()) {
                std::map<std::string, std::string> prompt_args;
                prompt_args["language"] = "C++";
                prompt_args["code"] = "int main() { return 0; }";
                auto prompt_result =
                    co_await client.get_prompt(prompts.prompts[0].name, std::move(prompt_args));
                for (const auto& msg : prompt_result.messages) {
                    if (auto* text = std::get_if<TextContent>(&msg.content)) {
                        std::cout << "Prompt message: " << text->text << "\n";
                    }
                }
            }

            // complete
            PromptReference prompt_ref;
            prompt_ref.name = "code_review";

            CompleteParamsArgument complete_arg;
            complete_arg.name = "language";
            complete_arg.value = "C+";

            CompleteParams complete_params;
            complete_params.ref = std::move(prompt_ref);
            complete_params.argument = std::move(complete_arg);
            auto complete_result = co_await client.complete(std::move(complete_params));
            std::cout << "Completions: " << complete_result.completion.values.size() << " values\n";

            // send_notification
            co_await client.send_notification("notifications/cancelled", std::nullopt);
            std::cout << "Sent cancellation notification\n";
        },
        boost::asio::detached);

    io_ctx.run();
    return EXIT_SUCCESS;
}

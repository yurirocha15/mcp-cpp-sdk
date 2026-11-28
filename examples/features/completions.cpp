/// @file completions.cpp
/// @brief Demonstrates Server::set_completion_provider() and Client::complete().
///
/// This example shows:
/// - A server that registers a prompt whose arguments support completion
/// - A server that sets up a completion provider handler
/// - A client that calls complete() to get argument completion suggestions
/// - The completion flow: client sends partial argument, server returns suggestions
/// - A completion for the argument of a resource template, named by a ResourceTemplateReference

#include <mcp/client/client.hpp>
#include <mcp/core/async.hpp>
#include <mcp/mcp.hpp>
#include <mcp/transport/memory.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace asio = boost::asio;

int main() {
    try {
        using namespace mcp;

        asio::io_context io_ctx;
        bool client_ok = false;

        // ========== SERVER SETUP ==========
        Implementation server_info;
        server_info.name = "completions-server";
        server_info.version = "1.0.0";

        Server server(server_info, ServerCapabilities{});

        // ========== PROMPT REGISTRATION ==========
        // Completions suggest values for the arguments of a prompt or a resource template.
        server.add_text_prompt(
            "code_analyzer", "Analyzes code in a specific language and framework",
            {PromptArgument{
                 .name = "language", .description = "Programming language", .required = true},
             PromptArgument{.name = "framework", .description = "Framework name"}},
            [](const Server::PromptArguments& args) {
                std::string text = "Analyze this code, written in " + args.at("language");
                if (const auto framework = args.find("framework"); framework != args.end()) {
                    text += " with " + framework->second;
                }
                return text;
            });

        // ========== COMPLETION PROVIDER ==========
        server.set_completion_provider([](const CompleteParams& params) -> Task<CompleteResult> {
            std::cout << "Server: Completion request for argument: " << params.argument.name << " = "
                      << params.argument.value << "\n";

            CompleteResult result;

            // docs-begin: completion-resource-server
            // A ResourceTemplateReference names the URI template whose argument is completed.
            if (const auto* tmpl = std::get_if<ResourceTemplateReference>(&params.ref)) {
                if (tmpl->uri == "mcp://logs/{node}") {
                    for (const std::string node : {"web-1", "web-2", "db-1"}) {
                        if (node.starts_with(params.argument.value)) {
                            result.completion.values.push_back(node);
                        }
                    }
                }
                co_return result;
            }
            // docs-end: completion-resource-server

            // Only the arguments of the code_analyzer prompt have suggestions.
            const auto* prompt = std::get_if<PromptReference>(&params.ref);
            if (prompt == nullptr || prompt->name != "code_analyzer") {
                co_return result;
            }

            // Provide completions based on argument name
            if (params.argument.name == "language") {
                // Language completions
                std::vector<std::string> languages = {
                    "C++",  "C",  "Python", "JavaScript", "TypeScript",
                    "Java", "Go", "Rust",   "Ruby",       "PHP",
                };

                // Filter by partial match
                std::string partial = params.argument.value;
                for (const auto& lang : languages) {
                    if (lang.starts_with(partial)) {  // Starts with partial
                        result.completion.values.push_back(lang);
                    }
                }
            } else if (params.argument.name == "framework") {
                // Framework completions
                std::vector<std::string> frameworks = {
                    "React",  "Vue",     "Angular", "Django", "Flask",
                    "Spring", "Express", "FastAPI", "Rails",  "Laravel",
                };

                // Filter by partial match
                std::string partial = params.argument.value;
                for (const auto& fw : frameworks) {
                    if (fw.starts_with(partial)) {  // Starts with partial
                        result.completion.values.push_back(fw);
                    }
                }
            }

            std::cout << "Server: Returning " << result.completion.values.size() << " completions\n";
            co_return result;
        });

        // ========== TRANSPORT & RUN ==========
        auto [client_transport, server_transport] = make_memory_transport_pair(io_ctx.get_executor());

        // Spawn server coroutine
        asio::co_spawn(
            io_ctx,
            [&]() -> Task<void> {
                try {
                    co_await server.run(server_transport, io_ctx.get_executor());
                } catch (const std::exception& e) {
                    std::cerr << "Server error: " << e.what() << "\n";
                }
            },
            asio::detached);

        // Spawn client coroutine
        asio::co_spawn(
            io_ctx,
            [&]() -> Task<void> {
                try {
                    // Create client
                    std::cout << "Client: Creating client\n";
                    Client client(client_transport, io_ctx.get_executor());

                    Implementation client_info;
                    client_info.name = "completions-client";
                    client_info.version = "1.0.0";

                    // Connect to server
                    std::cout << "Client: Connecting to server\n";
                    {
                        auto init_result = co_await client.connect(client_info, ClientCapabilities{});
                        // serverInfo is optional: a server may leave it out.
                        if (init_result.serverInfo) {
                            std::cout << "Client: Connected to " << init_result.serverInfo->name
                                      << "\n";
                        }
                    }

                    // Test 1: Complete language argument with "C"
                    std::cout << "\n--- Test 1: Language completions for 'C' ---\n";
                    CompleteParamsArgument lang_arg;
                    lang_arg.name = "language";
                    lang_arg.value = "C";

                    CompleteParams lang_params;
                    lang_params.ref = PromptReference{"ref/prompt", "code_analyzer"};
                    lang_params.argument = lang_arg;

                    auto lang_result = co_await client.complete(lang_params);
                    std::cout << "Client: Received " << lang_result.completion.values.size()
                              << " language completions:\n";
                    for (const auto& val : lang_result.completion.values) {
                        std::cout << "  - " << val << "\n";
                    }

                    // Test 2: Complete framework argument with "R"
                    std::cout << "\n--- Test 2: Framework completions for 'R' ---\n";
                    CompleteParamsArgument fw_arg;
                    fw_arg.name = "framework";
                    fw_arg.value = "R";

                    CompleteParams fw_params;
                    fw_params.ref = PromptReference{"ref/prompt", "code_analyzer"};
                    fw_params.argument = fw_arg;

                    auto fw_result = co_await client.complete(fw_params);
                    std::cout << "Client: Received " << fw_result.completion.values.size()
                              << " framework completions:\n";
                    for (const auto& val : fw_result.completion.values) {
                        std::cout << "  - " << val << "\n";
                    }

                    // Test 3: Complete with empty partial (all suggestions)
                    std::cout << "\n--- Test 3: All language completions (empty partial) ---\n";
                    CompleteParamsArgument all_lang_arg;
                    all_lang_arg.name = "language";
                    all_lang_arg.value = "";

                    CompleteParams all_lang_params;
                    all_lang_params.ref = PromptReference{"ref/prompt", "code_analyzer"};
                    all_lang_params.argument = all_lang_arg;

                    auto all_lang_result = co_await client.complete(all_lang_params);
                    std::cout << "Client: Received " << all_lang_result.completion.values.size()
                              << " language completions:\n";
                    for (const auto& val : all_lang_result.completion.values) {
                        std::cout << "  - " << val << "\n";
                    }

                    // Test 4: Complete the argument of a resource template
                    std::cout << "\n--- Test 4: Node completions for 'web' ---\n";
                    // docs-begin: completion-resource-client
                    CompleteParams node_params;
                    node_params.ref = ResourceTemplateReference{.uri = "mcp://logs/{node}"};
                    node_params.argument = {.name = "node", .value = "web"};

                    auto node_result = co_await client.complete(node_params);
                    // docs-end: completion-resource-client
                    std::cout << "Client: Received " << node_result.completion.values.size()
                              << " node completions:\n";
                    for (const auto& val : node_result.completion.values) {
                        std::cout << "  - " << val << "\n";
                    }
                    if (node_result.completion.values.size() != 2) {
                        throw std::runtime_error("expected two node completions");
                    }

                    std::cout << "\nClient: All completion tests completed successfully\n";
                    std::cout << "Client: Closing connection\n";
                    client.close();
                    client_ok = true;

                } catch (const std::exception& e) {
                    std::cerr << "Client error: " << e.what() << "\n";
                }
                io_ctx.stop();
            },
            asio::detached);

        // Fail rather than hang if something never answers.
        asio::steady_timer watchdog(io_ctx.get_executor());
        watchdog.expires_after(std::chrono::seconds(10));
        watchdog.async_wait([&io_ctx](const boost::system::error_code& error) {
            if (!error) {
                std::cerr << "Timed out\n";
                io_ctx.stop();
            }
        });

        io_ctx.run();

        if (!client_ok) {
            std::cerr << "Example failed\n";
            return EXIT_FAILURE;
        }
        std::cout << "Example completed successfully\n";
        return EXIT_SUCCESS;

    } catch (const std::exception& e) {
        std::cerr << "Exception: " << e.what() << "\n";
        return EXIT_FAILURE;
    } catch (...) {
        std::cerr << "Unknown exception\n";
        return EXIT_FAILURE;
    }
}

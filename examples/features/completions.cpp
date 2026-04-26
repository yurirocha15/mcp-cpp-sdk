/// @file completions.cpp
/// @brief Demonstrates Server::set_completion_provider() and Client::complete().
///
/// This example shows:
/// - A server that registers a tool with arguments supporting completion
/// - A server that sets up a completion provider handler
/// - A client that calls complete() to get argument completion suggestions
/// - The completion flow: client sends partial argument, server returns suggestions

#include <mcp/client.hpp>
#include <mcp/mcp.hpp>
#include <mcp/transport/memory.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace asio = boost::asio;

int main() {
    try {
        using namespace mcp;

        asio::io_context io_ctx;

        // ========== SERVER SETUP ==========
        ServerCapabilities server_caps;
        server_caps.tools = ServerCapabilities::ToolsCapability{};

        Implementation server_info;
        server_info.name = "completions-server";
        server_info.version = "1.0.0";

        Server server(server_info, server_caps);

        // ========== TOOL REGISTRATION ==========
        // Define a tool with arguments that support completion
        nlohmann::json tool_schema = {
            {"type", "object"},
            {"properties",
             {{"language", {{"type", "string"}, {"description", "Programming language"}}},
              {"framework", {{"type", "string"}, {"description", "Framework name"}}}}},
            {"required", nlohmann::json::array({"language"})}};

        server.add_tool<nlohmann::json, nlohmann::json>(
            "code_analyzer", "Analyzes code in a specific language and framework", tool_schema,
            [](const nlohmann::json& args) -> Task<nlohmann::json> {
                std::string language = args.at("language").get<std::string>();
                std::string framework = args.value("framework", "");

                std::cout << "Tool: Analyzing code in " << language;
                if (!framework.empty()) {
                    std::cout << " with " << framework;
                }
                std::cout << "\n";

                co_return nlohmann::json{
                    {"status", "analyzed"}, {"language", language}, {"framework", framework}};
            });

        // ========== COMPLETION PROVIDER ==========
        server.set_completion_provider([](const CompleteParams& params) -> Task<CompleteResult> {
            std::cout << "Server: Completion request for argument: " << params.argument.name << " = "
                      << params.argument.value << "\n";

            CompleteResult result;

            // Provide completions based on argument name
            if (params.argument.name == "language") {
                // Language completions
                std::vector<std::string> languages = {"C++",        "C",    "Python", "JavaScript",
                                                      "TypeScript", "Java", "Go",     "Rust",
                                                      "Ruby",       "PHP"};

                // Filter by partial match
                std::string partial = params.argument.value;
                for (const auto& lang : languages) {
                    if (lang.find(partial) == 0) {  // Starts with partial
                        result.completion.values.push_back(lang);
                    }
                }
            } else if (params.argument.name == "framework") {
                // Framework completions
                std::vector<std::string> frameworks = {"React", "Vue",    "Angular", "Django",
                                                       "Flask", "Spring", "Express", "FastAPI",
                                                       "Rails", "Laravel"};

                // Filter by partial match
                std::string partial = params.argument.value;
                for (const auto& fw : frameworks) {
                    if (fw.find(partial) == 0) {  // Starts with partial
                        result.completion.values.push_back(fw);
                    }
                }
            }

            std::cout << "Server: Returning " << result.completion.values.size() << " completions\n";
            co_return result;
        });

        // ========== TRANSPORT & RUN ==========
        auto [client_transport, server_transport] = create_memory_transport_pair(io_ctx.get_executor());

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
                    // Wait for server to start
                    std::cout << "Client: Waiting for server to start\n";
                    asio::steady_timer delay_timer(io_ctx.get_executor());
                    delay_timer.expires_after(std::chrono::milliseconds(50));
                    co_await delay_timer.async_wait(asio::use_awaitable);

                    // Create client
                    std::cout << "Client: Creating client\n";
                    Client client(client_transport, io_ctx.get_executor());

                    Implementation client_info;
                    client_info.name = "completions-client";
                    client_info.version = "1.0.0";

                    // Connect to server
                    std::cout << "Client: Connecting to server\n";
                    auto init_result = co_await client.connect(client_info, ClientCapabilities{});
                    std::cout << "Client: Connected to " << init_result.serverInfo.name << "\n";

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

                    std::cout << "\nClient: All completion tests completed successfully\n";
                    std::cout << "Client: Closing connection\n";
                    client.close();

                } catch (const std::exception& e) {
                    std::cerr << "Client error: " << e.what() << "\n";
                }
            },
            asio::detached);

        // Run the io_context with a timeout
        asio::steady_timer exit_timer(io_ctx.get_executor());
        exit_timer.expires_after(std::chrono::seconds(5));

        asio::co_spawn(
            io_ctx,
            [&]() -> Task<void> {
                co_await exit_timer.async_wait(asio::use_awaitable);
                std::cout << "\nTimeout: Exiting\n";
                io_ctx.stop();
            },
            asio::detached);

        io_ctx.run();

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

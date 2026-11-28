/// @file client_stdio.cpp
/// @brief MCP client over stdio demonstrating connect, list/call tools,
///   list/read resources, list templates and list/get prompts.
///
/// The client launches the server as a child process with StdioClientTransport and talks to
/// it over the child's standard input and output. The server is the example-server-stdio
/// executable next to this one, or the program named by the first argument.

#include <mcp/client/client.hpp>
#include <mcp/core/async.hpp>
#include <mcp/transport/stdio_client.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <cstdlib>
#include <filesystem>
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

static std::string server_program(int argc, char** argv) {
    if (argc > 1) {
        return argv[1];
    }
#ifdef _WIN32
    const char* name = "example-server-stdio.exe";
#else
    const char* name = "example-server-stdio";
#endif
    return (std::filesystem::absolute(argv[0]).parent_path() / name).string();
}

int main(int argc, char** argv) {  // NOLINT(readability-function-cognitive-complexity)
    try {
        using namespace mcp;

        boost::asio::io_context io_ctx;
        bool succeeded = false;

        // The transport starts the server process at once and owns it.
        StdioProcessOptions process;
        process.command = server_program(argc, argv);
        auto transport = std::make_shared<StdioClientTransport>(io_ctx.get_executor(), process);
        Client client(transport, io_ctx.get_executor());

        boost::asio::co_spawn(
            io_ctx,
            [&]() -> Task<void> {
                try {
                    Implementation client_info;
                    client_info.name = "example-client";
                    client_info.version = "1.0.0";

                    ClientCapabilities caps;
                    caps.sampling = ClientCapabilities::SamplingCapability{};

                    {
                        auto init_result = co_await client.connect(client_info, caps);
                        // serverInfo is optional: a server may leave it out. The default automatic()
                        // speaks 2026-07-28 with a server that serves it and falls back to initialize
                        // otherwise; era and protocolVersion say which one happened.
                        if (init_result.serverInfo) {
                            std::cerr << "Connected to: " << init_result.serverInfo->name << " "
                                      << init_result.serverInfo->version << "\n";
                        }
                        std::cerr << "Protocol version: " << init_result.protocolVersion << " ("
                                  << (init_result.era == ProtocolEra::eModern ? "modern" : "legacy")
                                  << " session)\n";
                    }

                    auto tools = co_await client.list_tools();
                    std::cerr << "Tools (" << tools.tools.size() << "):\n";
                    for (const auto& tool : tools.tools) {
                        std::cerr << "  - " << tool.name << "\n";
                    }

                    // call_tool with typed arguments
                    auto add_result =
                        co_await client.call_tool("add", AddArgs{.a = 3.0, .b = 4.0} /* NOLINT */);
                    for (const auto& block : add_result.content) {
                        if (const auto* text = std::get_if<TextContent>(&block)) {
                            std::cerr << "add(3, 4) = " << text->text << "\n";
                        }
                    }

                    // call_tool with raw JSON arguments
                    nlohmann::json echo_args = {{"text", "hello"}};
                    auto echo_result = co_await client.call_tool("echo_async", echo_args);
                    for (const auto& block : echo_result.content) {
                        if (const auto* text = std::get_if<TextContent>(&block)) {
                            std::cerr << "echo_async: " << text->text << "\n";
                        }
                    }

                    auto resources = co_await client.list_resources();
                    std::cerr << "Resources (" << resources.resources.size() << "):\n";
                    for (const auto& res : resources.resources) {
                        std::cerr << "  - " << res.uri << " (" << res.name << ")\n";
                    }

                    if (!resources.resources.empty()) {
                        auto read_result = co_await client.read_resource(resources.resources[0].uri);
                        for (const auto& content : read_result.contents) {
                            if (const auto* text = std::get_if<TextResourceContents>(&content)) {
                                std::cerr << "Resource content: " << text->text << "\n";
                            }
                        }
                    }

                    auto templates = co_await client.list_resource_templates();
                    std::cerr << "Resource templates (" << templates.resourceTemplates.size() << "):\n";
                    for (const auto& tmpl : templates.resourceTemplates) {
                        std::cerr << "  - " << tmpl.uriTemplate << " (" << tmpl.name << ")\n";
                    }

                    auto prompts = co_await client.list_prompts();
                    std::cerr << "Prompts (" << prompts.prompts.size() << "):\n";
                    for (const auto& prompt : prompts.prompts) {
                        std::cerr << "  - " << prompt.name << "\n";
                    }

                    if (!prompts.prompts.empty()) {
                        std::map<std::string, std::string> prompt_args;
                        prompt_args["language"] = "C++";
                        prompt_args["code"] = "int main() { return 0; }";
                        auto prompt_result =
                            co_await client.get_prompt(prompts.prompts[0].name, prompt_args);
                        for (const auto& msg : prompt_result.messages) {
                            if (const auto* text = std::get_if<TextContent>(&msg.content)) {
                                std::cerr << "Prompt message: " << text->text << "\n";
                            }
                        }
                    }

                    // End the session, then the server process, and wait for it to exit.
                    client.close();
                    const ProcessExit exit = co_await transport->shutdown();
                    std::cerr << "Server exited with code " << exit.code << "\n";
                    succeeded = true;
                } catch (const std::exception& e) {
                    std::cerr << "Client error: " << e.what() << "\n";
                }
            },
            boost::asio::detached);

        io_ctx.run();
        return succeeded ? EXIT_SUCCESS : EXIT_FAILURE;
    } catch (const std::exception& e) {
        std::cerr << "Exception: " << e.what() << "\n";
        return EXIT_FAILURE;
    } catch (...) {
        return EXIT_FAILURE;
    }
}

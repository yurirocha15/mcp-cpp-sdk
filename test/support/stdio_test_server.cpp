// A small MCP server that the stdio client tests launch as a child process. Its first argument
// picks how it behaves once its standard input ends:
//   serve               serve until standard input ends, then exit (the default)
//   ignore-stdin-close  keep running after standard input ends
//   ignore-sigterm      keep running after standard input ends and after SIGTERM (POSIX)
//   exit-at-once <n>    exit with status n without serving
//   write-raw <n> <nl>  write n bytes to standard output, followed by LF when nl is 1 or CRLF
//                       when it is 2, and exit
//   grandchild <file>   start a helper process that never exits on its own, write its process id
//                       to <file>, then serve until standard input ends (POSIX)
// The tools report what the process sees (arguments, environment, working directory), write to
// standard error, and end the process.

#include <mcp/core/async.hpp>
#include <mcp/mcp.hpp>

#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#else
#include <signal.h>
#include <unistd.h>
#endif

namespace {

using nlohmann::json;

void add_resource_and_prompt(mcp::Server& server) {
    mcp::Resource greeting;
    greeting.uri = "test://greeting";
    greeting.name = "greeting";
    server.add_resource<mcp::ReadResourceRequestParams, mcp::ReadResourceResult>(
        greeting, [](const mcp::ReadResourceRequestParams& params) {
            mcp::TextResourceContents text;
            text.uri = params.uri;
            text.text = "hello";
            mcp::ReadResourceResult result;
            result.contents.emplace_back(std::move(text));
            return result;
        });
    mcp::Prompt greet;
    greet.name = "greet";
    greet.arguments = std::vector<mcp::PromptArgument>{{"name", "Who to greet", true}};
    server.add_prompt<mcp::GetPromptRequestParams, mcp::GetPromptResult>(
        greet, [](const mcp::GetPromptRequestParams& params) {
            mcp::PromptMessage message;
            message.role = mcp::Role::eUser;
            mcp::TextContent text;
            text.text = "Hello";
            if (params.arguments && params.arguments->contains("name")) {
                text.text += ", " + params.arguments->at("name");
            }
            message.content = std::move(text);
            mcp::GetPromptResult result;
            result.messages.push_back(std::move(message));
            return result;
        });
    server.set_completion_provider([](const mcp::CompleteParams&) -> mcp::Task<mcp::CompleteResult> {
        mcp::CompleteResult result;
        result.completion.values = {"alpha"};
        co_return result;
    });
}

void add_tools(mcp::Server& server, int argc, char** argv) {
    server.add_tool<json, json>("env", "Reads an environment variable", json{{"type", "object"}},
                                [](mcp::Context&, json args) -> mcp::Task<json> {
                                    const auto name = args.value("name", std::string());
                                    const char* value = std::getenv(name.c_str());
                                    co_return json{{"set", value != nullptr},
                                                   {"value", value != nullptr ? value : ""}};
                                });
    server.add_tool<json, json>("cwd", "Reports the working directory", json{{"type", "object"}},
                                [](mcp::Context&, json) -> mcp::Task<json> {
                                    co_return json{{"cwd", std::filesystem::current_path().string()}};
                                });
    server.add_tool<json, json>("args", "Reports the arguments", json{{"type", "object"}},
                                [argc, argv](mcp::Context&, json) -> mcp::Task<json> {
                                    json list = json::array();
                                    for (int i = 1; i < argc; ++i) {
                                        list.push_back(argv[i]);
                                    }
                                    co_return json{{"args", list}};
                                });
    server.add_tool<json, json>("log", "Writes a line to standard error", json{{"type", "object"}},
                                [](mcp::Context&, json args) -> mcp::Task<json> {
                                    std::cerr << args.value("text", std::string()) << '\n'
                                              << std::flush;
                                    co_return json{{"logged", true}};
                                });
    server.add_tool<json, json>("hold", "Answers after an hour", json{{"type", "object"}},
                                [](mcp::Context&, json) -> mcp::Task<json> {
                                    boost::asio::steady_timer timer(
                                        co_await boost::asio::this_coro::executor,
                                        std::chrono::hours(1));
                                    co_await timer.async_wait(boost::asio::use_awaitable);
                                    co_return json::object();
                                });
    server.add_tool<json, json>(
        "progress", "Reports progress in steps, then answers, after wait_ms unless cancelled",
        json{{"type", "object"}}, [](mcp::Context& ctx, json args) -> mcp::Task<json> {
            const int steps = args.value("steps", 3);
            for (int step = 1; step <= steps; ++step) {
                co_await ctx.report_progress(step, steps);
            }
            const auto until =
                std::chrono::steady_clock::now() + std::chrono::milliseconds(args.value("wait_ms", 0));
            while (std::chrono::steady_clock::now() < until && !ctx.is_cancelled()) {
                boost::asio::steady_timer timer(co_await boost::asio::this_coro::executor,
                                                std::chrono::milliseconds(10));
                co_await timer.async_wait(boost::asio::use_awaitable);
            }
            co_return json{{"steps", steps}};
        });
    server.add_tool("stop", "Stops the server", json{{"type", "object"}},
                    [&server](const json&) -> json {
                        server.stop();
                        return "stopping";
                    });
    server.add_tool("notify_from_handler", "Sends a blocking notification from its handler",
                    json{{"type", "object"}}, [&server](const json&) -> json {
                        try {
                            server.sync().notify_tools_list_changed();
                        } catch (const std::logic_error&) {
                            return "refused";
                        }
                        return "sent";
                    });
    server.add_tool<json, json>(
        "notify_tools_changed", "Tells open subscriptions that the tool list changed",
        json{{"type", "object"}}, [&server](mcp::Context&, json) -> mcp::Task<json> {
            co_await server.notify_tools_list_changed();
            co_return json{{"notified", true}};
        });
    server.add_tool<json, json>("close_subscriptions", "Ends every open subscription",
                                json{{"type", "object"}},
                                [&server](mcp::Context&, json) -> mcp::Task<json> {
                                    server.close_subscriptions();
                                    co_return json{{"closed", true}};
                                });
    server.add_tool<json, json>("subscription_count", "Answers with the number of open subscriptions",
                                json{{"type", "object"}},
                                [&server](mcp::Context&, json) -> mcp::Task<json> {
                                    co_return json{{"count", server.subscription_count()}};
                                });
    server.add_tool<json, json>(
        "ask",
        "Asks the client for a value and answers with it; ends the process crash_after_ms after asking",
        json{{"type", "object"}}, [](mcp::Context& ctx, json args) -> mcp::Task<json> {
            const int crash_after_ms = args.value("crash_after_ms", 0);
            if (crash_after_ms > 0) {
                std::thread([crash_after_ms] {
                    std::this_thread::sleep_for(std::chrono::milliseconds(crash_after_ms));
                    std::_Exit(9);
                }).detach();
            }
            // On the heap: a std::string in the coroutine frame must not live across co_await.
            const auto question = std::make_shared<const mcp::ElicitRequestParams>([&args] {
                mcp::ElicitRequestFormParams form;
                form.message = args.value("question", std::string());
                form.requestedSchema = json{{"type", "object"}};
                return mcp::ElicitRequestParams(std::move(form));
            }());
            const auto answer = co_await ctx.elicit(*question);
            co_return json{{"answer", answer.content ? answer.content->value("v", json()) : json()}};
        });
    server.add_tool<json, json>("crash", "Ends the process at once", json{{"type", "object"}},
                                [](mcp::Context&, json args) -> mcp::Task<json> {
                                    std::cout.flush();
                                    std::_Exit(args.value("status", 7));
                                    co_return json::object();
                                });
}

void wait_forever() {
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

}  // namespace

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "serve";
    if (mode == "exit-at-once") {
        return argc > 2 ? std::atoi(argv[2]) : 1;
    }
    if (mode == "write-raw") {
#if defined(_WIN32)
        // Binary, so that the line ending is the one asked for and not LF turned into CRLF.
        (void)_setmode(_fileno(stdout), _O_BINARY);
#endif
        const auto count = argc > 2 ? static_cast<std::size_t>(std::atoll(argv[2])) : 0;
        std::string text(count, 'x');
        const std::string ending = argc > 3 ? argv[3] : "0";
        if (ending == "1") {
            text += "\n";
        } else if (ending == "2") {
            text += "\r\n";
        }
        std::fwrite(text.data(), 1, text.size(), stdout);
        std::fflush(stdout);
        return 0;
    }
#if !defined(_WIN32)
    if (mode == "grandchild" && argc > 2) {
        const pid_t helper = ::fork();
        if (helper == 0) {
            for (;;) {
                ::pause();
            }
        }
        // Written under another name first, so that a reader never sees half a number.
        const std::string target = argv[2];
        {
            std::ofstream out(target + ".tmp");
            out << helper << '\n';
        }
        std::rename((target + ".tmp").c_str(), target.c_str());
    }
    if (mode == "ignore-sigterm") {
        ::signal(SIGTERM, SIG_IGN);
    }
#endif
    mcp::ServerCapabilities capabilities;
    // listChanged lets a subscriptions/listen stream take notify_tools_changed's notification.
    capabilities.tools = mcp::ServerCapabilities::ToolsCapability{.listChanged = true};
    capabilities.resources = mcp::ServerCapabilities::ResourcesCapability{};
    capabilities.prompts = mcp::ServerCapabilities::PromptsCapability{};
    capabilities.completions = json::object();
    mcp::Server server(mcp::Implementation{"stdio-test-server", "1.0"}, capabilities);
    add_tools(server, argc, argv);
    add_resource_and_prompt(server);
    std::cerr << "test server started" << '\n' << std::flush;
    server.run_stdio();
    if (mode == "ignore-stdin-close" || mode == "ignore-sigterm") {
#if !defined(_WIN32)
        // run_stdio() restores the dispositions it found, which may not be the ones set above.
        if (mode == "ignore-sigterm") {
            ::signal(SIGTERM, SIG_IGN);
        }
#endif
        wait_forever();
    }
    return 0;
}

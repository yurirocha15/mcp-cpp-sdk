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

#include <mcp/mcp.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#else
#include <signal.h>
#include <unistd.h>
#endif

namespace {

using nlohmann::json;

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
    capabilities.tools = mcp::ServerCapabilities::ToolsCapability{};
    mcp::Server server(mcp::Implementation{"stdio-test-server", "1.0"}, capabilities);
    add_tools(server, argc, argv);
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

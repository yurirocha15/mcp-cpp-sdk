/// @file llama_server.cpp
/// @brief MCP server adapter for llama.cpp llama-server, with stdio and HTTP transport.

#include <mcp/core/async.hpp>
#include <mcp/server/server.hpp>

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <string_view>

namespace beast = boost::beast;
namespace http = beast::http;
namespace asio = boost::asio;
using tcp = asio::ip::tcp;

struct AppOptions {
    std::string transport = "stdio";
    std::string host = "127.0.0.1";
    unsigned short port = 8808;
    std::string llama_host = "127.0.0.1";
    unsigned short llama_port = 8080;
};

static void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog << " [options]\n"
              << "Options:\n"
              << "  --transport=<stdio|http>  Transport to use (default: stdio)\n"
              << "  --host=<addr>             HTTP listen address (default: 127.0.0.1)\n"
              << "  --port=<n>                HTTP listen port (default: 8808)\n"
              << "  --llama-host=<addr>       llama-server address (default: 127.0.0.1)\n"
              << "  --llama-port=<n>          llama-server port (default: 8080)\n"
              << "  --help                    Show this help\n";
}

enum class ParseResult : std::uint8_t {
    eOk,
    eHelp,
    eError
};

static ParseResult parse_args(int argc, char** argv, AppOptions& opts) {
    using namespace std::literals;

    constexpr auto transport_prefix = "--transport="sv;
    constexpr auto host_prefix = "--host="sv;
    constexpr auto port_prefix = "--port="sv;
    constexpr auto llama_host_prefix = "--llama-host="sv;
    constexpr auto llama_port_prefix = "--llama-port="sv;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--help"sv) {
            print_usage(argv[0]);
            return ParseResult::eHelp;
        }

        if (arg.starts_with(transport_prefix)) {
            opts.transport = std::string(arg.substr(transport_prefix.size()));
            if (opts.transport != "stdio" && opts.transport != "http") {
                std::cerr << "Invalid transport: " << opts.transport << "\n";
                print_usage(argv[0]);
                return ParseResult::eError;
            }
        } else if (arg.starts_with(host_prefix)) {
            opts.host = std::string(arg.substr(host_prefix.size()));
        } else if (arg.starts_with(port_prefix)) {
            opts.port =
                static_cast<unsigned short>(std::stoi(std::string(arg.substr(port_prefix.size()))));
        } else if (arg.starts_with(llama_host_prefix)) {
            opts.llama_host = std::string(arg.substr(llama_host_prefix.size()));
        } else if (arg.starts_with(llama_port_prefix)) {
            opts.llama_port = static_cast<unsigned short>(
                std::stoi(std::string(arg.substr(llama_port_prefix.size()))));
        } else {
            std::cerr << "Unknown option: " << arg << "\n";
            print_usage(argv[0]);
            return ParseResult::eError;
        }
    }

    return ParseResult::eOk;
}

/// Sends one request to llama-server and returns the body of its answer.
///
/// The parameters are taken by value: a coroutine keeps its parameters in its frame, so a
/// reference to a caller's temporary would dangle once the coroutine suspends.
static mcp::Task<std::string> call_llama(std::string host, unsigned short port, http::verb method,
                                         std::string target, std::string body) {
    auto executor = co_await asio::this_coro::executor;
    tcp::resolver resolver(executor);
    beast::tcp_stream stream(executor);

    // Generating text can take a while. One deadline covers connecting, sending and reading.
    stream.expires_after(std::chrono::seconds(120 /* NOLINT */));

    try {
        auto const endpoints =
            co_await resolver.async_resolve(host, std::to_string(port), asio::use_awaitable);
        co_await stream.async_connect(endpoints, asio::use_awaitable);
    } catch (const boost::system::system_error& error) {
        throw std::runtime_error("cannot reach llama-server at " + host + ":" + std::to_string(port) +
                                 ": " + error.code().message());
    }

    http::request<http::string_body> req{method, target, 11 /* NOLINT */};
    req.set(http::field::host, host);
    req.set(http::field::content_type, "application/json");
    req.body() = std::move(body);
    req.prepare_payload();
    co_await http::async_write(stream, req, asio::use_awaitable);

    beast::flat_buffer buf;
    http::response<http::string_body> res;
    co_await http::async_read(stream, buf, res, asio::use_awaitable);

    beast::error_code ec;
    (void)stream.socket().shutdown(tcp::socket::shutdown_both, ec);

    if (res.result_int() < 200 || res.result_int() >= 300) {  // NOLINT
        throw std::runtime_error("llama-server answered " + target + " with HTTP " +
                                 std::to_string(res.result_int()));
    }
    co_return std::move(res.body());
}

int main(int argc, char** argv) {  // NOLINT(readability-function-cognitive-complexity)
    try {
        using namespace mcp;

        AppOptions opts;
        const auto parse_result = parse_args(argc, argv, opts);
        if (parse_result == ParseResult::eHelp) {
            return EXIT_SUCCESS;
        }
        if (parse_result == ParseResult::eError) {
            return EXIT_FAILURE;
        }

        ServerCapabilities caps;
        caps.tools = ServerCapabilities::ToolsCapability{};
        caps.resources = ServerCapabilities::ResourcesCapability{};
        caps.prompts = ServerCapabilities::PromptsCapability{};

        Implementation info;
        info.name = "llama-mcp-server";
        info.version = "1.0.0";
        info.description = "MCP adapter for llama.cpp llama-server";

        Server server(info, caps);

        // The handlers below await llama-server without blocking: while one waits for a reply,
        // the server goes on reading and answering other requests on the same thread. A handler
        // that throws (llama-server unreachable, an error status, an unexpected reply) is answered
        // with an isError result carrying the exception's message.
        nlohmann::json chat_schema = {{"type", "object"},
                                      {"properties",
                                       {{"messages", {{"type", "array"}}},
                                        {"temperature", {{"type", "number"}}},
                                        {"max_tokens", {{"type", "integer"}}}}},
                                      {"required", nlohmann::json::array({"messages"})}};
        server.add_tool<nlohmann::json, CallToolResult>(
            "chat", "Send chat messages to the llama-server model", chat_schema,
            [opts](nlohmann::json args) -> Task<CallToolResult> {
                nlohmann::json body;
                body["messages"] = std::move(args.at("messages"));
                if (args.contains("temperature")) {
                    body["temperature"] = args["temperature"];
                }
                if (args.contains("max_tokens")) {
                    body["max_tokens"] = args["max_tokens"];
                }

                auto resp_str = co_await call_llama(opts.llama_host, opts.llama_port, http::verb::post,
                                                    "/v1/chat/completions", body.dump());

                auto resp_json = nlohmann::json::parse(resp_str);
                co_return make_tool_text_result(
                    resp_json.at("choices").at(0).at("message").at("content").get<std::string>());
            });

        nlohmann::json complete_schema = {{"type", "object"},
                                          {"properties",
                                           {{"prompt", {{"type", "string"}}},
                                            {"temperature", {{"type", "number"}}},
                                            {"max_tokens", {{"type", "integer"}}}}},
                                          {"required", nlohmann::json::array({"prompt"})}};
        server.add_tool<nlohmann::json, CallToolResult>(
            "complete", "Generate text completion from the llama-server model", complete_schema,
            [opts](nlohmann::json args) -> Task<CallToolResult> {
                nlohmann::json body;
                body["prompt"] = std::move(args.at("prompt"));
                if (args.contains("temperature")) {
                    body["temperature"] = args["temperature"];
                }
                if (args.contains("max_tokens")) {
                    body["max_tokens"] = args["max_tokens"];
                }

                auto resp_str = co_await call_llama(opts.llama_host, opts.llama_port, http::verb::post,
                                                    "/v1/completions", body.dump());

                auto resp_json = nlohmann::json::parse(resp_str);
                co_return make_tool_text_result(
                    resp_json.at("choices").at(0).at("text").get<std::string>());
            });

        nlohmann::json embed_schema = {{"type", "object"},
                                       {"properties", {{"input", {{"type", "string"}}}}},
                                       {"required", nlohmann::json::array({"input"})}};
        server.add_tool<nlohmann::json, CallToolResult>(
            "embed", "Generate embedding vector from the llama-server model", embed_schema,
            [opts](nlohmann::json args) -> Task<CallToolResult> {
                nlohmann::json body;
                body["input"] = std::move(args.at("input"));

                auto resp_str = co_await call_llama(opts.llama_host, opts.llama_port, http::verb::post,
                                                    "/v1/embeddings", body.dump());

                auto resp_json = nlohmann::json::parse(resp_str);
                co_return make_tool_text_result(resp_json.at("data").at(0).at("embedding").dump());
            });

        mcp::Resource models_resource;
        models_resource.uri = "llama://models";
        models_resource.name = "Loaded Models";
        models_resource.description = "Models currently loaded in llama-server";
        models_resource.mimeType = "application/json";

        // A resource handler that throws is answered with a JSON-RPC error.
        server.add_resource<mcp::ReadResourceRequestParams, mcp::ReadResourceResult>(
            models_resource,
            [opts](mcp::ReadResourceRequestParams params) -> Task<mcp::ReadResourceResult> {
                auto resp_str = co_await call_llama(opts.llama_host, opts.llama_port, http::verb::get,
                                                    "/v1/models", "");

                mcp::TextResourceContents contents;
                contents.uri = std::move(params.uri);
                contents.text = std::move(resp_str);
                contents.mimeType = "application/json";

                mcp::ReadResourceResult result;
                result.contents.emplace_back(std::move(contents));
                co_return result;
            });

        mcp::Prompt summarize_prompt;
        summarize_prompt.name = "summarize";
        summarize_prompt.description = "Generate a summarization prompt for the given text";

        mcp::PromptArgument text_arg;
        text_arg.name = "text";
        text_arg.description = "The text to summarize";
        text_arg.required = true;
        summarize_prompt.arguments = std::vector<mcp::PromptArgument>{std::move(text_arg)};

        server.add_prompt<mcp::GetPromptRequestParams, mcp::GetPromptResult>(
            summarize_prompt, [](mcp::GetPromptRequestParams params) -> mcp::GetPromptResult {
                std::string text;
                if (params.arguments) {
                    if (auto it = params.arguments->find("text"); it != params.arguments->end()) {
                        text = it->second;
                    }
                }

                mcp::PromptMessage msg;
                msg.role = mcp::Role::eUser;
                mcp::TextContent tc;
                tc.text = "Please provide a concise summary of the following text:\n\n" + text;
                msg.content = std::move(tc);

                mcp::GetPromptResult result;
                result.description = "Summarization prompt";
                result.messages.push_back(std::move(msg));
                return result;
            });

        // Both block: run_stdio() until standard input ends, and either until SIGINT or SIGTERM.
        if (opts.transport == "stdio") {
            server.run_stdio();
        } else {
            std::cerr << "Listening on http://" << opts.host << ":" << opts.port << "/mcp\n";
            server.run_http(opts.host, opts.port);
        }
        return EXIT_SUCCESS;
    } catch (const std::exception& e) {
        std::cerr << "Exception: " << e.what() << "\n";
        return EXIT_FAILURE;
    } catch (...) {
        return EXIT_FAILURE;
    }
}

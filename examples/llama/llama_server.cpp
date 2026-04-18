/// @file llama_server.cpp
/// @brief MCP server adapter for llama.cpp llama-server, with stdio and HTTP transport.

#include <mcp/server.hpp>
#include <mcp/transport/http_server.hpp>
#include <mcp/transport/stdio.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
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

enum class ParseResult {
    Ok,
    Help,
    Error
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
            return ParseResult::Help;
        }

        if (arg.rfind(transport_prefix, 0) == 0) {
            opts.transport = std::string(arg.substr(transport_prefix.size()));
            if (opts.transport != "stdio" && opts.transport != "http") {
                std::cerr << "Invalid transport: " << opts.transport << "\n";
                print_usage(argv[0]);
                return ParseResult::Error;
            }
        } else if (arg.rfind(host_prefix, 0) == 0) {
            opts.host = std::string(arg.substr(host_prefix.size()));
        } else if (arg.rfind(port_prefix, 0) == 0) {
            opts.port =
                static_cast<unsigned short>(std::stoi(std::string(arg.substr(port_prefix.size()))));
        } else if (arg.rfind(llama_host_prefix, 0) == 0) {
            opts.llama_host = std::string(arg.substr(llama_host_prefix.size()));
        } else if (arg.rfind(llama_port_prefix, 0) == 0) {
            opts.llama_port = static_cast<unsigned short>(
                std::stoi(std::string(arg.substr(llama_port_prefix.size()))));
        } else {
            std::cerr << "Unknown option: " << arg << "\n";
            print_usage(argv[0]);
            return ParseResult::Error;
        }
    }

    return ParseResult::Ok;
}

static nlohmann::json make_text_result(std::string_view text) {
    return nlohmann::json{{"content", nlohmann::json::array({nlohmann::json{
                                          {"type", "text"}, {"text", std::string(text)}}})}};
}

static nlohmann::json make_error_result(std::string_view msg) {
    return nlohmann::json{
        {"content",
         nlohmann::json::array({nlohmann::json{{"type", "text"}, {"text", std::string(msg)}}})},
        {"isError", true}};
}

static mcp::Task<std::string> call_llama(asio::any_io_executor executor, std::string host,
                                         unsigned short port, http::verb method, std::string target,
                                         std::string body) {
    tcp::resolver resolver(executor);
    beast::tcp_stream stream(executor);

    auto const endpoints =
        co_await resolver.async_resolve(host, std::to_string(port), asio::use_awaitable);
    co_await asio::async_connect(stream.socket(), endpoints, asio::use_awaitable);

    http::request<http::string_body> req{method, target, 11};
    req.set(http::field::host, host);
    req.set(http::field::content_type, "application/json");
    req.body() = std::move(body);
    req.prepare_payload();

    stream.expires_after(std::chrono::seconds(120));
    co_await http::async_write(stream, req, asio::use_awaitable);

    beast::flat_buffer buf;
    http::response<http::string_body> res;
    co_await http::async_read(stream, buf, res, asio::use_awaitable);

    beast::error_code ec;
    stream.socket().shutdown(tcp::socket::shutdown_both, ec);

    co_return res.body();
}

int main(int argc, char** argv) {
    using namespace mcp;

    AppOptions opts;
    const auto parse_result = parse_args(argc, argv, opts);
    if (parse_result == ParseResult::Help) {
        return EXIT_SUCCESS;
    }
    if (parse_result == ParseResult::Error) {
        return EXIT_FAILURE;
    }

    ServerCapabilities caps;
    caps.tools = ServerCapabilities::ToolsCapability{.listChanged = true};
    caps.resources = ServerCapabilities::ResourcesCapability{.listChanged = true};
    caps.prompts = ServerCapabilities::PromptsCapability{.listChanged = true};
    caps.logging = nlohmann::json::object();

    Implementation info;
    info.name = "llama-mcp-server";
    info.version = "1.0.0";
    info.description = "MCP adapter for llama.cpp llama-server";

    Server server(std::move(info), std::move(caps));

    boost::asio::io_context io_ctx;
    asio::any_io_executor io_executor = io_ctx.get_executor();

    nlohmann::json chat_schema = {{"type", "object"},
                                  {"properties",
                                   {{"messages", {{"type", "array"}}},
                                    {"temperature", {{"type", "number"}}},
                                    {"max_tokens", {{"type", "integer"}}}}},
                                  {"required", nlohmann::json::array({"messages"})}};
    server.add_tool<nlohmann::json, nlohmann::json>(
        "chat", "Send chat messages to the llama-server model", std::move(chat_schema),
        [io_executor, opts](nlohmann::json args) -> mcp::Task<nlohmann::json> {
            try {
                nlohmann::json body;
                body["messages"] = args.at("messages");
                if (args.contains("temperature")) {
                    body["temperature"] = args["temperature"];
                }
                if (args.contains("max_tokens")) {
                    body["max_tokens"] = args["max_tokens"];
                }

                auto resp_str =
                    co_await call_llama(io_executor, opts.llama_host, opts.llama_port, http::verb::post,
                                        "/v1/chat/completions", body.dump());

                auto resp_json = nlohmann::json::parse(resp_str);
                auto text =
                    resp_json.at("choices").at(0).at("message").at("content").get<std::string>();
                co_return make_text_result(text);
            } catch (const std::exception& e) {
                co_return make_error_result(e.what());
            }
        });

    nlohmann::json complete_schema = {{"type", "object"},
                                      {"properties",
                                       {{"prompt", {{"type", "string"}}},
                                        {"temperature", {{"type", "number"}}},
                                        {"max_tokens", {{"type", "integer"}}}}},
                                      {"required", nlohmann::json::array({"prompt"})}};
    server.add_tool<nlohmann::json, nlohmann::json>(
        "complete", "Generate text completion from the llama-server model", std::move(complete_schema),
        [io_executor, opts](nlohmann::json args) -> mcp::Task<nlohmann::json> {
            try {
                nlohmann::json body;
                body["prompt"] = args.at("prompt");
                if (args.contains("temperature")) {
                    body["temperature"] = args["temperature"];
                }
                if (args.contains("max_tokens")) {
                    body["max_tokens"] = args["max_tokens"];
                }

                auto resp_str = co_await call_llama(io_executor, opts.llama_host, opts.llama_port,
                                                    http::verb::post, "/v1/completions", body.dump());

                auto resp_json = nlohmann::json::parse(resp_str);
                auto text = resp_json.at("choices").at(0).at("text").get<std::string>();
                co_return make_text_result(text);
            } catch (const std::exception& e) {
                co_return make_error_result(e.what());
            }
        });

    nlohmann::json embed_schema = {{"type", "object"},
                                   {"properties", {{"input", {{"type", "string"}}}}},
                                   {"required", nlohmann::json::array({"input"})}};
    server.add_tool<nlohmann::json, nlohmann::json>(
        "embed", "Generate embedding vector from the llama-server model", std::move(embed_schema),
        [io_executor, opts](nlohmann::json args) -> mcp::Task<nlohmann::json> {
            try {
                nlohmann::json body;
                body["input"] = args.at("input");

                auto resp_str = co_await call_llama(io_executor, opts.llama_host, opts.llama_port,
                                                    http::verb::post, "/v1/embeddings", body.dump());

                auto resp_json = nlohmann::json::parse(resp_str);
                auto& embedding = resp_json.at("data").at(0).at("embedding");
                co_return make_text_result(embedding.dump());
            } catch (const std::exception& e) {
                co_return make_error_result(e.what());
            }
        });

    mcp::Resource models_resource;
    models_resource.uri = "llama://models";
    models_resource.name = "Loaded Models";
    models_resource.description = "Models currently loaded in llama-server";
    models_resource.mimeType = "application/json";

    server.add_resource<mcp::ReadResourceRequestParams, mcp::ReadResourceResult>(
        std::move(models_resource),
        [io_executor,
         opts](mcp::ReadResourceRequestParams params) -> mcp::Task<mcp::ReadResourceResult> {
            try {
                auto resp_str = co_await call_llama(io_executor, opts.llama_host, opts.llama_port,
                                                    http::verb::get, "/v1/models", "");

                mcp::TextResourceContents contents;
                contents.uri = params.uri;
                contents.text = std::move(resp_str);
                contents.mimeType = "application/json";

                mcp::ReadResourceResult result;
                result.contents.push_back(std::move(contents));
                co_return result;
            } catch (const std::exception& e) {
                mcp::TextResourceContents err_contents;
                err_contents.uri = params.uri;
                err_contents.text = std::string("Error fetching models: ") + e.what();
                err_contents.mimeType = "text/plain";

                mcp::ReadResourceResult result;
                result.contents.push_back(std::move(err_contents));
                co_return result;
            }
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
        std::move(summarize_prompt), [](mcp::GetPromptRequestParams params) -> mcp::GetPromptResult {
            std::string text;
            if (params.arguments) {
                if (auto it = params.arguments->find("text"); it != params.arguments->end()) {
                    text = it->second;
                }
            }

            mcp::PromptMessage msg;
            msg.role = mcp::Role::User;
            mcp::TextContent tc;
            tc.text = "Please provide a concise summary of the following text:\n\n" + text;
            msg.content = std::move(tc);

            mcp::GetPromptResult result;
            result.description = "Summarization prompt";
            result.messages.push_back(std::move(msg));
            return result;
        });

    if (opts.transport == "stdio") {
        auto transport = std::make_shared<StdioTransport>(io_ctx.get_executor());

        boost::asio::co_spawn(
            io_ctx,
            [&, transport = transport]() mutable -> Task<void> {
                co_await server.run(transport, io_ctx.get_executor());
            },
            boost::asio::detached);
    } else {
        std::cerr << "Listening on http://" << opts.host << ":" << std::to_string(opts.port)
                  << "/mcp\n";
        auto http_transport =
            std::make_shared<mcp::HttpServerTransport>(io_ctx.get_executor(), opts.host, opts.port);
        auto* http_ptr = http_transport.get();
        boost::asio::co_spawn(
            io_ctx,
            [&, transport = http_transport]() mutable -> mcp::Task<void> {
                boost::asio::co_spawn(io_ctx, http_ptr->listen(), boost::asio::detached);
                co_await server.run(transport, io_ctx.get_executor());
            },
            boost::asio::detached);
    }

    io_ctx.run();
    return EXIT_SUCCESS;
}

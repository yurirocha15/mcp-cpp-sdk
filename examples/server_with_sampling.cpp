/// @file server_with_sampling.cpp
/// @brief MCP server demonstrating reverse RPC via Context::sample_llm().

#include <mcp/server.hpp>
#include <mcp/transport/stdio.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <cstdlib>
#include <iostream>
#include <string>

int main() {
    using namespace mcp;

    ServerCapabilities caps;
    caps.tools = ServerCapabilities::ToolsCapability{.listChanged = true};

    Implementation info;
    info.name = "sampling-server";
    info.version = "1.0.0";

    Server server(std::move(info), std::move(caps));

    // Tool: summarize — async with context, calls ctx.sample_llm() for reverse RPC
    nlohmann::json schema = {{"type", "object"},
                             {"properties", {{"text", {{"type", "string"}}}}},
                             {"required", nlohmann::json::array({"text"})}};

    server.add_tool<nlohmann::json, nlohmann::json>(
        "summarize", "Summarize text by asking the client LLM via sampling", std::move(schema),
        [](Context& ctx, nlohmann::json in) -> Task<nlohmann::json> {
            auto text = in.at("text").get<std::string>();

            co_await ctx.log_info("Requesting LLM summary for " + std::to_string(text.size()) +
                                  " chars");

            SamplingMessage msg;
            msg.role = Role::User;
            TextContent text_content;
            text_content.text = "Please summarize the following text:\n\n" + text;
            msg.content = SamplingMessageContentBlock{std::move(text_content)};

            CreateMessageRequestParams sample_req;
            sample_req.messages.push_back(std::move(msg));
            sample_req.maxTokens = 256;
            sample_req.systemPrompt = "You are a concise summarizer.";
            sample_req.temperature = 0.3;

            auto result = co_await ctx.sample_llm(std::move(sample_req));

            std::string summary;
            if (auto* block = std::get_if<SamplingMessageContentBlock>(&result.content)) {
                if (auto* tc = std::get_if<TextContent>(block)) {
                    summary = tc->text;
                }
            }

            nlohmann::json out = {{"summary", summary}, {"model", result.model}};
            co_return out;
        });

    boost::asio::io_context io_ctx;
    auto transport = std::make_unique<StdioTransport>(io_ctx.get_executor());

    boost::asio::co_spawn(
        io_ctx,
        [&]() -> Task<void> { co_await server.run(std::move(transport), io_ctx.get_executor()); },
        boost::asio::detached);

    io_ctx.run();
    return EXIT_SUCCESS;
}

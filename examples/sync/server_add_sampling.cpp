// server_base.cpp plus a tool that asks the client's language model for text. The client needs a
// sampling handler, as client_add_sampling.cpp registers.

#include <mcp/core/async.hpp>
#include <mcp/mcp.hpp>

#include <string>
#include <variant>

int main() {
    mcp::Server server("greeter", "1.0.0");

    server.add_tool("greet", "Greets someone by name",
                    mcp::tool_params({{"name", "string", "Who to greet"}}),
                    [](const nlohmann::json& args) -> nlohmann::json {
                        return "Hello, " + args.at("name").get<std::string>() + "!";
                    });

    server.add_tool(
        "write_greeting", "Asks the client's model to write a greeting",
        mcp::tool_params({{"name", "string", "Who to greet"}}),
        [](mcp::Context& ctx, const nlohmann::json& args) -> mcp::Task<nlohmann::json> {
            mcp::CreateMessageRequestParams request;
            request.maxTokens = 100;
            request.messages.push_back(
                {mcp::Role::eUser,
                 mcp::TextContent{.text = "Write a greeting for " + args.at("name").get<std::string>()},
                 std::nullopt});
            const auto reply = co_await ctx.sample_llm(request);
            const auto* block = std::get_if<mcp::ContentBlock>(&reply.content);
            const auto* text = block != nullptr ? std::get_if<mcp::TextContent>(block) : nullptr;
            co_return text != nullptr ? text->text : std::string("(no text)");
        });

    server.run_stdio();
}

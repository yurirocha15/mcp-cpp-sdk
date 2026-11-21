// server_base.cpp plus a tool that asks the client which directories it may work in. The client
// needs a roots handler, as client_add_roots.cpp registers.

#include <mcp/core/async.hpp>
#include <mcp/mcp.hpp>

#include <string>

int main() {
    mcp::Server server("greeter", "1.0.0");

    server.add_tool("greet", "Greets someone by name",
                    mcp::tool_params({{"name", "string", "Who to greet"}}),
                    [](const nlohmann::json& args) -> nlohmann::json {
                        return "Hello, " + args.at("name").get<std::string>() + "!";
                    });

    server.add_tool("count_roots", "Counts the client's roots", nlohmann::json{{"type", "object"}},
                    [](mcp::Context& ctx, const nlohmann::json&) -> mcp::Task<nlohmann::json> {
                        const auto listed = co_await ctx.request_roots();
                        co_return "The client has " + std::to_string(listed.roots.size()) + " root(s)";
                    });

    server.run_stdio();
}

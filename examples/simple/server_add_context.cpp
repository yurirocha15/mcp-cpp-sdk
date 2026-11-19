// server_base.cpp whose tool logs to the client and reports its progress. Only that handler
// changes: it takes the request's Context and becomes a coroutine.

#include <mcp/core/async.hpp>
#include <mcp/mcp.hpp>

#include <string>

int main() {
    mcp::Server server("greeter", "1.0.0");

    server.add_tool("greet", "Greets someone by name",
                    mcp::tool_params({{"name", "string", "Who to greet"}}),
                    [](mcp::Context& ctx, const nlohmann::json& args) -> mcp::Task<nlohmann::json> {
                        co_await ctx.log_info("Composing a greeting");
                        co_await ctx.report_progress(1, 1);
                        co_return "Hello, " + args.at("name").get<std::string>() + "!";
                    });

    server.run_stdio();
}

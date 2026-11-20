// server_base.cpp plus middleware, which each request answered by an application handler, such as
// a tool call, runs through: here it counts those calls and writes the count to standard error.

#include <mcp/core/async.hpp>
#include <mcp/mcp.hpp>

#include <atomic>
#include <iostream>
#include <string>

int main() {
    mcp::Server server("greeter", "1.0.0");

    server.add_tool("greet", "Greets someone by name",
                    mcp::tool_params({{"name", "string", "Who to greet"}}),
                    [](const nlohmann::json& args) -> nlohmann::json {
                        return "Hello, " + args.at("name").get<std::string>() + "!";
                    });

    std::atomic<int> calls{0};
    server.use([&calls](mcp::Context& ctx, const nlohmann::json& params,
                        mcp::TypeErasedHandler next) -> mcp::Task<nlohmann::json> {
        std::cerr << "handler call " << ++calls << "\n";
        co_return co_await next(ctx, params);
    });

    server.run_stdio();
}

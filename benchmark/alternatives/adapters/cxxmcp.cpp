#include "benchmark_workload.hpp"

#include <cxxmcp/peer.hpp>
#include <cxxmcp/run.hpp>

#include <cstdlib>
#include <string>
#include <utility>

namespace {

int port() {
    const char* value = std::getenv("PORT");
    return value ? std::stoi(value) : 8080;
}

}  // namespace

int main() {
    using Json = mcp::protocol::Json;

    mcp_benchmark::Workload workload("cpp-sdk");
    auto server = mcp::ServerPeer::builder();
    server.name("benchmark-cpp-sdk").version("1.0.0").streamable_http("0.0.0.0", port(), "/mcp");

    const auto add_tool = [&](const std::string& name, const std::string& description) {
        mcp::protocol::ToolDefinition definition;
        definition.name = name;
        definition.description = description;
        definition.input_schema = Json::parse(mcp_benchmark::Workload::input_schema(name));
        server.tool<Json, mcp::protocol::ToolResult>(
            std::move(definition), [&workload, name](const Json& args) {
                return mcp::protocol::ToolResult::text(workload.invoke(name, args.dump()));
            });
    };
    add_tool("search_products", "Search products and merge popularity data");
    add_tool("get_user_cart", "Get a cart with recent order history");
    add_tool("checkout", "Calculate and record a checkout");

    return server.run();
}

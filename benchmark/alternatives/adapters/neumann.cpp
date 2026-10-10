#include "benchmark_workload.hpp"

#include "mcp/http_server_host.hpp"
#include "mcp/mcp.hpp"

#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>

namespace {

int port() {
    const char* value = std::getenv("PORT");
    return value ? std::stoi(value) : 8080;
}

mcp::CallToolResult text_result(std::string text) {
    return mcp::CallToolResult{
        .content = {mcp::TextContent{.text = std::move(text)}},
        .is_error = false,
    };
}

}  // namespace

int main() {
    mcp::set_log_level(mcp::LogLevel::off);
    mcp_benchmark::Workload workload("cpp-sdk");
    mcp::HttpServerHost::Options options;
    options.host = "0.0.0.0";
    options.port = port();
    options.path = "/mcp";

    mcp::HttpServerHost host(
        mcp::Implementation{.name = "benchmark-cpp-sdk", .version = "1.0.0"}, std::move(options),
        [&workload](mcp::Server& server) {
            const auto add_tool = [&](const std::string& name, const std::string& description) {
                server.tool(
                    name, nlohmann::json::parse(mcp_benchmark::Workload::input_schema(name)),
                    [&workload, name](const nlohmann::json& args) {
                        return text_result(workload.invoke(name, args.dump()));
                    },
                    std::nullopt, description);
            };
            add_tool("search_products", "Search products and merge popularity data");
            add_tool("get_user_cart", "Get a cart with recent order history");
            add_tool("checkout", "Calculate and record a checkout");
        });
    host.start();
    while (true) {
        std::this_thread::sleep_for(std::chrono::hours(24));
    }
}

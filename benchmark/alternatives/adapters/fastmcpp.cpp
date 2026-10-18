#include "benchmark_workload.hpp"

#include "fastmcpp/app.hpp"
#include "fastmcpp/mcp/handler.hpp"
#include "fastmcpp/server/streamable_http_server.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

namespace {

int port() {
    const char* value = std::getenv("PORT");
    return value ? std::stoi(value) : 8080;
}

}  // namespace

int main() {
    using fastmcpp::Json;
    mcp_benchmark::Workload workload("cpp-sdk");
    fastmcpp::FastMCP app("benchmark-cpp-sdk", "1.0.0");

    const auto add_tool = [&](const std::string& name, const std::string& description) {
        fastmcpp::FastMCP::ToolOptions options;
        options.description = description;
        app.tool(
            name, Json::parse(mcp_benchmark::Workload::input_schema(name)),
            [&workload, name](const Json& args) { return workload.invoke(name, args.dump()); },
            std::move(options));
    };
    add_tool("search_products", "Search products and merge popularity data");
    add_tool("get_user_cart", "Get a cart with recent order history");
    add_tool("checkout", "Calculate and record a checkout");

    auto handler = fastmcpp::mcp::make_mcp_handler(app);
    fastmcpp::server::StreamableHttpServerWrapper server(std::move(handler), "0.0.0.0", port(), "/mcp");
    if (!server.start()) {
        std::cerr << "failed to start FastMCPP benchmark server\n";
        return EXIT_FAILURE;
    }
    while (true) {
        std::this_thread::sleep_for(std::chrono::hours(24));
    }
}

#include "benchmark_workload.hpp"

#include "mcp_server.h"
#include "mcp_tool.h"

#include <cstdlib>
#include <string>

namespace {

int port() {
    const char* value = std::getenv("PORT");
    return value ? std::stoi(value) : 8080;
}

mcp::tool make_tool(const std::string& name, const std::string& description) {
    return mcp::tool{name, description, mcp::json::parse(mcp_benchmark::Workload::input_schema(name)),
                     mcp::json::object()};
}

}  // namespace

int main() {
    mcp::set_log_level(mcp::log_level::error);
    mcp_benchmark::Workload workload("cpp-sdk");
    mcp::server::configuration config;
    config.host = "0.0.0.0";
    config.port = port();
    config.mcp_endpoint = "/mcp";
    config.max_sessions = 0;
    config.session_timeout = 0;
    // This separate async pool serves legacy paths, not the synchronous
    // Streamable HTTP endpoint measured by this benchmark.
    config.threadpool_size = 2;
    mcp::server server(config);
    server.set_server_info("benchmark-cpp-sdk", "1.0.0");
    server.set_capabilities({{"tools", mcp::json::object()}});

    const auto register_tool = [&](const std::string& name, const std::string& description) {
        server.register_tool(make_tool(name, description), [&workload, name](const mcp::json& args,
                                                                             const std::string&) {
            return mcp::json::array({{{"type", "text"}, {"text", workload.invoke(name, args.dump())}}});
        });
    };
    register_tool("search_products", "Search products and merge popularity data");
    register_tool("get_user_cart", "Get a cart with recent order history");
    register_tool("checkout", "Calculate and record a checkout");
    return server.start(true) ? EXIT_SUCCESS : EXIT_FAILURE;
}

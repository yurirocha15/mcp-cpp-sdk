#include "benchmark_workload.hpp"

#include <mcp/mcp.hpp>
#include <mcp/transport/http_session_manager.hpp>

#include <cstddef>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>

// The shared workload served in sync mode: plain-function tool handlers on a handler pool, and
// run_http(), which runs the server's event loop on the calling thread.

namespace {

constexpr std::size_t kHandlerThreadCount = MCP_BENCHMARK_REQUEST_CONCURRENCY;

unsigned short port() {
    const char* value = std::getenv("PORT");
    return value ? static_cast<unsigned short>(std::stoi(value)) : 8080;
}

// MCP_SESSION_MODE=stateless serves older protocol revisions without sessions; anything else keeps
// the default, which issues a session per client.
bool stateless() {
    const char* value = std::getenv("MCP_SESSION_MODE");
    return value != nullptr && std::string_view(value) == "stateless";
}

}  // namespace

int main() {
    auto workload = std::make_shared<mcp_benchmark::Workload>("cpp-sdk");
    mcp::Server server("benchmark-cpp-sdk", "1.0.0");

    mcp::Runtime handler_pool;
    handler_pool.start(kHandlerThreadCount);
    server.set_handler_executor(handler_pool.executor());

    const auto add_tool = [&](const std::string& name, const std::string& description) {
        server.add_tool(name, description,
                        nlohmann::json::parse(mcp_benchmark::Workload::input_schema(name)),
                        [workload, name](const nlohmann::json& args) -> nlohmann::json {
                            return workload->invoke(name, args.dump());
                        });
    };
    add_tool("search_products", "Search products and merge popularity data");
    add_tool("get_user_cart", "Get a cart with recent order history");
    add_tool("checkout", "Calculate and record a checkout");

    server.run_http("0.0.0.0", port(), [](mcp::StreamableHttpSessionManager& http) {
        http.set_stateless_json_mode(stateless());
    });
}

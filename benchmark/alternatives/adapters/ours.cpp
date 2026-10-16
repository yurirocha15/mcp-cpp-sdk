#include "benchmark_workload.hpp"

#include <mcp/server/server.hpp>
#include <mcp/transport/http_session_manager.hpp>

#include <boost/asio.hpp>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr std::size_t kHandlerThreadCount = MCP_BENCHMARK_REQUEST_CONCURRENCY;
constexpr std::size_t kIoThreadCount = 2;

unsigned short port() {
    const char* value = std::getenv("PORT");
    return value ? static_cast<unsigned short>(std::stoi(value)) : 8080;
}

std::unique_ptr<mcp::Server> make_server(const std::shared_ptr<mcp_benchmark::Workload>& workload) {
    mcp::ServerCapabilities capabilities;
    capabilities.tools = mcp::ServerCapabilities::ToolsCapability{};
    auto server = std::make_unique<mcp::Server>(mcp::Implementation{"benchmark-cpp-sdk", "1.0.0"},
                                                std::move(capabilities));

    const auto add_tool = [&](const std::string& name, const std::string& description) {
        server->add_tool<nlohmann::json, mcp::CallToolResult>(
            name, description, nlohmann::json::parse(mcp_benchmark::Workload::input_schema(name)),
            [workload, name](const nlohmann::json& args) {
                return mcp::make_tool_text_result(workload->invoke(name, args.dump()));
            });
    };
    add_tool("search_products", "Search products and merge popularity data");
    add_tool("get_user_cart", "Get a cart with recent order history");
    add_tool("checkout", "Calculate and record a checkout");
    return server;
}

}  // namespace

int main() {
    namespace asio = boost::asio;

    auto workload = std::make_shared<mcp_benchmark::Workload>("cpp-sdk");
    asio::io_context io_context;
    asio::thread_pool handler_pool(kHandlerThreadCount);

    mcp::StreamableHttpSessionManager manager(
        io_context.get_executor(), "0.0.0.0", port(),
        [workload](const asio::any_io_executor&) { return make_server(workload); });
    manager.set_tool_executor(handler_pool.get_executor());

    const auto io_work = asio::make_work_guard(io_context);
    asio::co_spawn(io_context, manager.listen(), asio::detached);

    std::vector<std::thread> io_threads;
    io_threads.reserve(kIoThreadCount);
    for (std::size_t i = 0; i < kIoThreadCount; ++i) {
        io_threads.emplace_back([&io_context] { io_context.run(); });
    }
    for (auto& thread : io_threads) {
        thread.join();
    }
}

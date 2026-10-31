#include "async_workload.hpp"
#include "benchmark_schemas.hpp"

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

constexpr std::size_t kIoThreadCount = 2;

unsigned short port() {
    const char* value = std::getenv("PORT");
    return value ? static_cast<unsigned short>(std::stoi(value)) : 8080;
}

struct ToolSchemas {
    nlohmann::json search_products = parse("search_products");
    nlohmann::json get_user_cart = parse("get_user_cart");
    nlohmann::json checkout = parse("checkout");

    static nlohmann::json parse(const std::string& name) {
        return nlohmann::json::parse(mcp_benchmark::tool_input_schema(name));
    }
};

// The handler is a coroutine: it suspends on the workload's Redis and HTTP requests, so no thread
// waits for them and no separate handler pool is needed.
mcp::Task<mcp::CallToolResult> run_tool(std::shared_ptr<mcp_benchmark::AsyncWorkload> workload,
                                        std::string name, nlohmann::json args) {
    auto text = co_await workload->invoke(std::move(name), std::move(args));
    co_return mcp::make_tool_text_result(std::move(text));
}

// The session manager builds a Server per session, so the schemas are parsed once and copied.
std::unique_ptr<mcp::Server> make_server(const std::shared_ptr<mcp_benchmark::AsyncWorkload>& workload,
                                         const ToolSchemas& schemas) {
    mcp::ServerCapabilities capabilities;
    capabilities.tools = mcp::ServerCapabilities::ToolsCapability{};
    auto server = std::make_unique<mcp::Server>(mcp::Implementation{"benchmark-cpp-sdk", "1.0.0"},
                                                std::move(capabilities));

    const auto add_tool = [&](const std::string& name, const std::string& description,
                              const nlohmann::json& schema) {
        server->add_tool<nlohmann::json, mcp::CallToolResult>(
            name, description, schema, [workload, name](nlohmann::json args) {
                return run_tool(workload, name, std::move(args));
            });
    };
    add_tool("search_products", "Search products and merge popularity data", schemas.search_products);
    add_tool("get_user_cart", "Get a cart with recent order history", schemas.get_user_cart);
    add_tool("checkout", "Calculate and record a checkout", schemas.checkout);
    return server;
}

}  // namespace

int main() {
    namespace asio = boost::asio;

    auto workload = std::make_shared<mcp_benchmark::AsyncWorkload>("cpp-sdk");
    auto schemas = std::make_shared<const ToolSchemas>();
    asio::io_context io_context;

    mcp::StreamableHttpSessionManager manager(
        io_context.get_executor(), "0.0.0.0", port(),
        [workload, schemas](const asio::any_io_executor&) { return make_server(workload, *schemas); });

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

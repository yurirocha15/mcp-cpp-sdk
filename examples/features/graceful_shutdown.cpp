/// @file graceful_shutdown.cpp
/// @brief Demonstrates graceful shutdown patterns in MCP server.
///
/// This example shows:
/// - Calling mcp::graceful_shutdown() to close transport and stop io_context
/// - Manual transport pattern (not run_stdio convenience)
/// - Timer-based auto-exit for CI/testing

#include <mcp/server.hpp>
#include <mcp/signal.hpp>
#include <mcp/transport/stdio.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <streambuf>
#include <string>

namespace asio = boost::asio;

namespace {

class BlockingInputBuffer : public std::streambuf {
   public:
    void close() {
        {
            std::lock_guard lock(mutex_);
            closed_ = true;
        }
        cv_.notify_all();
    }

   protected:
    int_type underflow() override {
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [this] { return closed_ || !buffer_.empty(); });

        if (buffer_.empty()) {
            return traits_type::eof();
        }

        current_char_ = buffer_.front();
        buffer_.pop_front();
        setg(&current_char_, &current_char_, &current_char_ + 1);
        return traits_type::to_int_type(current_char_);
    }

   private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<char> buffer_;
    char current_char_ = '\0';
    bool closed_ = false;
};

}  // namespace

int main() {
    try {
        using namespace mcp;

        asio::io_context io_ctx;

        // ========== SERVER SETUP ==========
        ServerCapabilities server_caps;
        server_caps.tools = ServerCapabilities::ToolsCapability{};

        Implementation server_info;
        server_info.name = "graceful-shutdown-server";
        server_info.version = "1.0.0";

        Server server(server_info, server_caps);

        // ========== TOOL REGISTRATION ==========
        // Tool: quick_task — completes immediately
        nlohmann::json quick_schema = {{"type", "object"},
                                       {"properties", {{"value", {{"type", "integer"}}}}},
                                       {"required", nlohmann::json::array({"value"})}};

        server.add_tool("quick_task", "A quick task that completes immediately", quick_schema,
                        [](const nlohmann::json& args) -> nlohmann::json {
                            std::cout << "[Server] quick_task called\n";
                            int value = args.at("value").get<int>();
                            CallToolResult result;
                            TextContent content;
                            content.text = "Quick task result: " + std::to_string(value * 2);
                            result.content.emplace_back(std::move(content));
                            return nlohmann::json(result);
                        });

        // Tool: slow_task — simulates work with delay
        nlohmann::json slow_schema = {{"type", "object"},
                                      {"properties", {{"duration_ms", {{"type", "integer"}}}}},
                                      {"required", nlohmann::json::array({"duration_ms"})}};

        server.add_tool<nlohmann::json, nlohmann::json>(
            "slow_task", "A task that takes time to complete", slow_schema,
            [](nlohmann::json args) -> Task<nlohmann::json> {
                std::cout << "[Server] slow_task called\n";
                int duration_ms = args.at("duration_ms").get<int>();

                // Simulate work with async delay
                asio::steady_timer work_timer(
                    asio::get_associated_executor(co_await asio::this_coro::executor));
                work_timer.expires_after(std::chrono::milliseconds(duration_ms));
                co_await work_timer.async_wait(asio::use_awaitable);

                std::cout << "[Server] slow_task completed after " << duration_ms << "ms\n";
                CallToolResult result;
                TextContent content;
                content.text = "Slow task completed after " + std::to_string(duration_ms) + "ms";
                result.content.emplace_back(std::move(content));
                co_return nlohmann::json(result);
            });

        // ========== TRANSPORT SETUP ==========
        BlockingInputBuffer input_buffer;
        std::istream controlled_input(&input_buffer);
        auto transport =
            std::make_shared<StdioTransport>(io_ctx.get_executor(), controlled_input, std::cout);

        // ========== GRACEFUL SHUTDOWN (auto-triggered for CI/testing) ==========
        asio::co_spawn(
            io_ctx,
            [&, transport]() -> Task<void> {
                asio::steady_timer trigger(io_ctx.get_executor());
                trigger.expires_after(std::chrono::seconds(1));
                co_await trigger.async_wait(asio::use_awaitable);
                std::cout << "[Main] Initiating graceful shutdown\n";
                input_buffer.close();
                graceful_shutdown(io_ctx, transport);
                std::cout << "[Main] Graceful shutdown initiated (timeout: "
                          << constants::g_shutdown_timeout_ms << "ms)\n";
            },
            asio::detached);

        // ========== SERVER COROUTINE ==========
        asio::co_spawn(
            io_ctx,
            [&, transport]() -> Task<void> {
                std::cout << "[Server] Starting\n";
                try {
                    co_await server.run(transport, io_ctx.get_executor());
                    std::cout << "[Server] Stopped normally\n";
                } catch (const std::exception& e) {
                    std::cout << "[Server] Stopped with exception: " << e.what() << "\n";
                }
            },
            asio::detached);

        // ========== RUN IO CONTEXT ==========
        std::cout << "[Main] Starting io_context\n";
        io_ctx.run();

        std::cout << "[Main] io_context stopped, exiting normally\n";
        return EXIT_SUCCESS;

    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << '\n';
        return EXIT_FAILURE;
    }
}

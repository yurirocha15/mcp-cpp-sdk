/// @file graceful_shutdown.cpp
/// @brief Demonstrates graceful shutdown patterns in MCP server.
///
/// This example shows:
/// - Calling mcp::graceful_shutdown() to close transport and stop io_context
/// - Manual transport pattern (not run_stdio convenience)
/// - Timer-based auto-exit for CI/testing

#include <mcp/core/async.hpp>
#include <mcp/core/constants.hpp>
#include <mcp/core/signal.hpp>
#include <mcp/server/server.hpp>
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
            std::scoped_lock lock(mutex_);
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

        ServerCapabilities server_caps;
        server_caps.tools = ServerCapabilities::ToolsCapability{};

        Implementation server_info;
        server_info.name = "graceful-shutdown-server";
        server_info.version = "1.0.0";

        Server server(server_info, server_caps);

        // Tool: quick_task: completes immediately
        nlohmann::json quick_schema = {
            {"type", "object"},
            {"properties", {{"value", {{"type", "integer"}}}}},
            {"required", nlohmann::json::array({"value"})},
        };

        server.add_tool<nlohmann::json, CallToolResult>(
            "quick_task", "A quick task that completes immediately", quick_schema,
            [](const nlohmann::json& args) -> CallToolResult {
                std::cerr << "[Server] quick_task called\n";
                int value = args.at("value").get<int>();
                return make_tool_text_result("Quick task result: " + std::to_string(value * 2));
            });

        // Tool: slow_task: simulates work with delay
        nlohmann::json slow_schema = {
            {"type", "object"},
            {"properties", {{"duration_ms", {{"type", "integer"}}}}},
            {"required", nlohmann::json::array({"duration_ms"})},
        };

        server.add_tool<nlohmann::json, CallToolResult>(
            "slow_task", "A task that takes time to complete", slow_schema,
            [](nlohmann::json args) -> Task<CallToolResult> {
                std::cerr << "[Server] slow_task called\n";
                int duration_ms = args.at("duration_ms").get<int>();

                auto executor = co_await asio::this_coro::executor;
                asio::steady_timer work_timer(executor);
                work_timer.expires_after(std::chrono::milliseconds(duration_ms));
                co_await work_timer.async_wait(asio::use_awaitable);

                std::cerr << "[Server] slow_task completed after " << duration_ms << "ms\n";
                co_return make_tool_text_result("Slow task completed after " +
                                                std::to_string(duration_ms) + "ms");
            });

        // stdout is handed to the transport, so it is the protocol channel and
        // carries JSON-RPC only. Every diagnostic in this example goes to stderr.
        BlockingInputBuffer input_buffer;
        std::istream controlled_input(&input_buffer);
        auto transport =
            std::make_shared<StdioTransport>(io_ctx.get_executor(), controlled_input, std::cout);

        // ========== GRACEFUL SHUTDOWN (auto-triggered for CI/testing) ==========
        // graceful_shutdown() returns its watchdog timer. The server cancels it once it has
        // stopped; otherwise the pending timer keeps io_ctx.run() going until the timeout.
        std::shared_ptr<asio::steady_timer> watchdog;
        asio::co_spawn(
            io_ctx,
            [&, transport]() -> Task<void> {
                asio::steady_timer trigger(io_ctx.get_executor());
                trigger.expires_after(std::chrono::seconds(1));
                co_await trigger.async_wait(asio::use_awaitable);
                std::cerr << "[Main] Initiating graceful shutdown\n";
                input_buffer.close();
                watchdog = graceful_shutdown(io_ctx, transport);
                std::cerr << "[Main] Graceful shutdown initiated (timeout: "
                          << mcp::constants::shutdown::g_TIMEOUT_MS << "ms)\n";
            },
            asio::detached);

        // ========== SERVER COROUTINE ==========
        asio::co_spawn(
            io_ctx,
            [&, transport]() -> Task<void> {
                std::cerr << "[Server] Starting\n";
                try {
                    co_await server.run(transport, io_ctx.get_executor());
                    std::cerr << "[Server] Stopped normally\n";
                } catch (const std::exception& e) {
                    std::cerr << "[Server] Stopped with exception: " << e.what() << "\n";
                }
                if (watchdog) {
                    watchdog->cancel();
                }
            },
            asio::detached);

        // ========== RUN IO CONTEXT ==========
        std::cerr << "[Main] Starting io_context\n";
        io_ctx.run();

        std::cerr << "[Main] io_context stopped, exiting normally\n";
        return EXIT_SUCCESS;

    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << '\n';
        return EXIT_FAILURE;
    }
}

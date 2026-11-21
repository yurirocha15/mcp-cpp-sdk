#include "../support/hang_guard.hpp"
#include "../test_utils.hpp"

#include "../support/spawn.hpp"
#include "mcp/client/client.hpp"
#include "mcp/core/async.hpp"
#include "mcp/server/server.hpp"
#include "mcp/transport/http_client.hpp"
#include "mcp/transport/http_session_manager.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <chrono>
#include <mcp/core/constants.hpp>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace transport_http_tasks_test {

namespace jsonrpc = mcp::constants::jsonrpc;

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using nlohmann::json;
using Headers = std::vector<std::pair<std::string, std::string>>;

const std::string g_tasks = "io.modelcontextprotocol/tasks";

struct Reply {
    unsigned int status{0};
    json message;
};

// Everything is taken by value: this is a coroutine, and callers pass temporaries.
mcp::Task<Reply> send(asio::any_io_executor executor, unsigned short port, std::string body,
                      Headers headers) {
    beast::tcp_stream stream(executor);
    asio::ip::tcp::resolver resolver(executor);
    auto endpoints =
        co_await resolver.async_resolve("127.0.0.1", std::to_string(port), asio::use_awaitable);
    co_await stream.async_connect(*endpoints.begin(), asio::use_awaitable);

    http::request<http::string_body> request{http::verb::post, "/mcp", 11};
    request.set(http::field::host, "127.0.0.1");
    request.set(http::field::content_type, "application/json");
    request.set(http::field::accept, "application/json, text/event-stream");
    for (const auto& [name, value] : headers) {
        request.insert(name, value);
    }
    request.body() = std::move(body);
    request.prepare_payload();
    co_await http::async_write(stream, request, asio::use_awaitable);

    beast::flat_buffer buffer;
    http::response<http::string_body> response;
    co_await http::async_read(stream, buffer, response, asio::use_awaitable);
    Reply reply;
    reply.status = response.result_int();
    reply.message = json::parse(response.body(), nullptr, false);
    beast::error_code ignored;
    stream.socket().shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
    co_return reply;
}

json declaring_tasks(const json& id, std::string_view method, json params) {
    auto request = make_modern_request(id, method, std::move(params));
    request["params"]["_meta"]["io.modelcontextprotocol/clientCapabilities"] = {
        {"extensions", {{g_tasks, json::object()}}}};
    return request;
}

Headers headers_for(const json& body, std::optional<std::string> name = std::nullopt) {
    Headers headers{{"MCP-Protocol-Version", "2026-07-28"}, {"Mcp-Method", body["method"]}};
    if (name) {
        headers.emplace_back("Mcp-Name", *name);
    }
    return headers;
}

class HttpTasksTest : public ::testing::Test {
   protected:
    asio::io_context io_ctx_;

    static mcp::StreamableHttpSessionManager::ServerFactory factory() {
        return [](const mcp::Executor& executor) {
            mcp::ServerCapabilities caps;
            caps.tools = mcp::ServerCapabilities::ToolsCapability{};
            auto server = std::make_unique<mcp::Server>(mcp::Implementation{"tasks-http", "1.0"}, caps);
            server->add_tool<json, json>(
                "echo", "Echoes", json{{"type", "object"}},
                [](json args) -> mcp::Task<json> { co_return json{{"echo", std::move(args)}}; });
            mcp::TaskOptions options;
            options.poll_interval = std::chrono::milliseconds(5);
            server->enable_tasks(options);
            server->set_handler_executor(executor);
            server->set_tool_task_mode("echo", mcp::ToolTaskMode::eAuto);
            return server;
        };
    }

    template <typename Exchange>
    void run(mcp::StreamableHttpSessionManager& manager, Exchange exchange) {
        mcp_test::spawn_detached(io_ctx_, manager.listen());
        mcp_test::spawn_detached(
            io_ctx_, [&manager, exchange = std::move(exchange)]() mutable -> mcp::Task<void> {
                co_await exchange();
                manager.close();
            });
        io_ctx_.run();
    }

    mcp::Task<Reply> post(unsigned short port, const json& body, Headers headers) {
        return send(io_ctx_.get_executor(), port, body.dump(), std::move(headers));
    }
};

TEST_F(HttpTasksTest, TaskMethodsMirrorTheTaskIdInMcpName) {
    constexpr unsigned short g_port = 20680;
    mcp::StreamableHttpSessionManager manager(io_ctx_.get_executor(), "127.0.0.1", g_port, factory());
    Reply created;
    std::vector<Reply> mismatched;
    Reply matched;
    run(manager, [&]() -> mcp::Task<void> {
        const auto call =
            declaring_tasks(1, "tools/call", {{"name", "echo"}, {"arguments", json::object()}});
        created = co_await post(g_port, call, headers_for(call, "echo"));
        const auto id = created.message["result"]["taskId"].get<std::string>();
        for (const char* method : {"tasks/get", "tasks/update", "tasks/cancel"}) {
            json params = {{"taskId", id}};
            if (std::string_view(method) == "tasks/update") {
                params["inputResponses"] = json::object();
            }
            const auto request = declaring_tasks(2, method, std::move(params));
            mismatched.push_back(co_await post(g_port, request, headers_for(request)));
            mismatched.push_back(co_await post(g_port, request, headers_for(request, "another")));
        }
        const auto get = declaring_tasks(3, "tasks/get", {{"taskId", id}});
        matched = co_await post(g_port, get, headers_for(get, id));
    });

    ASSERT_EQ(created.status, 200u) << created.message;
    EXPECT_EQ(created.message["result"]["resultType"], "task");
    ASSERT_EQ(mismatched.size(), 6u);
    for (const auto& reply : mismatched) {
        EXPECT_EQ(reply.status, 400u) << reply.message;
        EXPECT_EQ(reply.message["error"]["code"], jsonrpc::g_HEADER_MISMATCH) << reply.message;
    }
    EXPECT_EQ(matched.status, 200u) << matched.message;
    EXPECT_EQ(matched.message["result"]["resultType"], "complete");
}

TEST_F(HttpTasksTest, ATaskMethodWithoutTheExtensionIsAMissingCapability) {
    constexpr unsigned short g_port = 20681;
    mcp::StreamableHttpSessionManager manager(io_ctx_.get_executor(), "127.0.0.1", g_port, factory());
    Reply reply;
    run(manager, [&]() -> mcp::Task<void> {
        const auto request =
            make_modern_request(1, "tasks/get", {{"taskId", "0123456789abcdef0123456789abcdef"}});
        reply =
            co_await post(g_port, request, headers_for(request, "0123456789abcdef0123456789abcdef"));
    });
    EXPECT_EQ(reply.status, 400u) << reply.message;
    EXPECT_EQ(reply.message["error"]["code"], jsonrpc::g_MISSING_REQUIRED_CLIENT_CAPABILITY);
}

TEST_F(HttpTasksTest, AnUnknownTaskIs400InvalidParams) {
    constexpr unsigned short g_port = 20682;
    mcp::StreamableHttpSessionManager manager(io_ctx_.get_executor(), "127.0.0.1", g_port, factory());
    Reply reply;
    run(manager, [&]() -> mcp::Task<void> {
        const auto request =
            declaring_tasks(1, "tasks/get", {{"taskId", "0123456789abcdef0123456789abcdef"}});
        reply =
            co_await post(g_port, request, headers_for(request, "0123456789abcdef0123456789abcdef"));
    });
    EXPECT_EQ(reply.status, 400u) << reply.message;
    EXPECT_EQ(reply.message["error"]["code"], jsonrpc::g_INVALID_PARAMS);
}

// The client sends the header the server checks: following a task needs both ends to agree.
TEST_F(HttpTasksTest, TheHttpClientFollowsATaskThroughTheServer) {
    constexpr unsigned short g_port = 20683;
    mcp::StreamableHttpSessionManager manager(io_ctx_.get_executor(), "127.0.0.1", g_port, factory());
    std::optional<mcp::CallToolResult> result;
    std::optional<mcp::DetailedTask> started;
    run(manager, [&]() -> mcp::Task<void> {
        auto transport = std::make_shared<mcp::HttpClientTransport>(
            io_ctx_.get_executor(), "http://127.0.0.1:" + std::to_string(g_port) + "/mcp");
        mcp::ClientOptions options;
        options.protocol = mcp::ClientProtocol::pinned("2026-07-28");
        options.tasks = mcp::ClientTaskMode::eAuto;
        options.task_poll_interval = std::chrono::milliseconds(5);
        mcp::Client client(transport, io_ctx_.get_executor(), options);
        try {
            co_await client.connect("tasks-client", "1.0");
            const json arguments = {{"a", 1}};
            result = co_await client.call_tool("echo", arguments);
            auto handle = co_await client.start_tool("echo", arguments);
            if (auto* task = std::get_if<mcp::TaskHandle>(&handle)) {
                started = co_await task->wait();
            }
        } catch (const std::exception& error) {
            ADD_FAILURE() << error.what();
        }
        client.close();
    });
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->structuredContent->at("echo").at("a"), 1);
    ASSERT_TRUE(started.has_value());
    EXPECT_EQ(started->task.status, mcp::TaskStatus::eCompleted);
}

}  // namespace

}  // namespace transport_http_tasks_test

#include "mcp/transport/http_session_manager.hpp"

#include <gtest/gtest.h>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// Helper: raw HTTP request/response over TCP
// ---------------------------------------------------------------------------

struct RawResponse {
    unsigned int status{0};
    std::string body;
    std::string session_id;
    std::string content_type;
};

/// Fire a single HTTP request and return the response.
/// Must be called inside a coroutine on the same io_context.
mcp::Task<RawResponse> raw_request(
    const asio::any_io_executor& executor, unsigned short port, http::verb method,
    const std::string& target, const std::string& body = {}, const std::string& session_id = {},
    std::optional<std::string> protocol_version = std::string(mcp::g_LATEST_PROTOCOL_VERSION)) {
    beast::tcp_stream stream(executor);
    auto resolver = asio::ip::tcp::resolver(executor);
    auto endpoints =
        co_await resolver.async_resolve("127.0.0.1", std::to_string(port), asio::use_awaitable);
    co_await stream.async_connect(*endpoints.begin(), asio::use_awaitable);

    http::request<http::string_body> request{method, target, 11};
    request.set(http::field::host, "127.0.0.1");
    request.set(http::field::content_type, "application/json");
    request.set(http::field::accept, "application/json");

    if (protocol_version.has_value()) {
        request.set("MCP-Protocol-Version", *protocol_version);
    }

    if (!session_id.empty()) {
        request.set("Mcp-Session-Id", session_id);
    }

    if (!body.empty()) {
        request.body() = body;
    }
    request.prepare_payload();

    co_await http::async_write(stream, request, asio::use_awaitable);

    beast::flat_buffer response_buffer;
    http::response<http::string_body> response;
    co_await http::async_read(stream, response_buffer, response, asio::use_awaitable);

    RawResponse result;
    result.status = response.result_int();
    result.body = response.body();
    result.content_type = std::string(response[http::field::content_type]);

    auto session_it = response.find("Mcp-Session-Id");
    if (session_it != response.end()) {
        result.session_id = std::string(session_it->value());
    }

    beast::error_code shutdown_error;
    stream.socket().shutdown(asio::ip::tcp::socket::shutdown_both, shutdown_error);

    co_return result;
}

// ---------------------------------------------------------------------------
// Helper: create a ServerFactory for the session manager
// ---------------------------------------------------------------------------

mcp::StreamableHttpSessionManager::ServerFactory make_echo_server_factory() {
    return [](const asio::any_io_executor&) -> std::unique_ptr<mcp::Server> {
        mcp::ServerCapabilities caps;
        caps.tools = mcp::ServerCapabilities::ToolsCapability{};
        auto server = std::make_unique<mcp::Server>(mcp::Implementation{"test-session-server", "1.0.0"},
                                                    std::move(caps));

        server->add_tool<json, json>(
            "echo", "Echoes input",
            json{{"type", "object"}, {"properties", {{"message", {{"type", "string"}}}}}},
            [](json params) -> mcp::Task<json> {
                json result;
                result["content"] = json::array();
                result["content"].push_back(
                    json{{"type", "text"}, {"text", params.value("message", "empty")}});
                co_return result;
            });

        return server;
    };
}

/// Send an initialize request and return the raw response (contains session ID).
mcp::Task<RawResponse> do_initialize(
    const asio::any_io_executor& executor, unsigned short port, int id = 1,
    std::string protocol_version = std::string(mcp::g_LATEST_PROTOCOL_VERSION),
    std::optional<std::string> header_protocol_version = std::string(mcp::g_LATEST_PROTOCOL_VERSION)) {
    json init_request = {{"jsonrpc", "2.0"},
                         {"method", "initialize"},
                         {"params",
                          {{"protocolVersion", std::move(protocol_version)},
                           {"clientInfo", {{"name", "test-client"}, {"version", "1.0.0"}}},
                           {"capabilities", json::object()}}},
                         {"id", id}};

    co_return co_await raw_request(executor, port, http::verb::post, "/mcp", init_request.dump(), {},
                                   std::move(header_protocol_version));
}

}  // namespace

// ===========================================================================
// Test fixture
// ===========================================================================

class SessionManagerTest : public ::testing::Test {
   protected:
    asio::io_context io_ctx_;
};

// ---------------------------------------------------------------------------
// 1. Create session on initialize (no session header)
// ---------------------------------------------------------------------------

TEST_F(SessionManagerTest, InitializeCreatesSession) {
    const unsigned short port = 19080;
    mcp::StreamableHttpSessionManager manager(io_ctx_.get_executor(), "127.0.0.1", port,
                                              make_echo_server_factory());

    asio::co_spawn(io_ctx_, manager.listen(), asio::detached);

    RawResponse init_response;
    asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            init_response = co_await do_initialize(io_ctx_.get_executor(), port);
            manager.close();
        },
        asio::detached);

    io_ctx_.run();

    EXPECT_EQ(init_response.status, 200);
    EXPECT_FALSE(init_response.session_id.empty());
    EXPECT_EQ(init_response.session_id.size(), 32);  // UUID hex, 32 chars

    auto body = json::parse(init_response.body);
    EXPECT_TRUE(body.contains("result"));
    EXPECT_EQ(body["result"]["protocolVersion"], std::string(mcp::g_LATEST_PROTOCOL_VERSION));
}

TEST_F(SessionManagerTest, InitializeNegotiatesOlderSupportedProtocolVersion) {
    const unsigned short port = 19092;
    mcp::StreamableHttpSessionManager manager(io_ctx_.get_executor(), "127.0.0.1", port,
                                              make_echo_server_factory());

    asio::co_spawn(io_ctx_, manager.listen(), asio::detached);

    RawResponse init_response;
    asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            init_response =
                co_await do_initialize(io_ctx_.get_executor(), port, 1, "2025-06-18", std::nullopt);
            manager.close();
        },
        asio::detached);

    io_ctx_.run();

    ASSERT_EQ(init_response.status, 200);
    auto body = json::parse(init_response.body);
    EXPECT_EQ(body["result"]["protocolVersion"], "2025-06-18");
}

TEST_F(SessionManagerTest, SubsequentRequestsUseNegotiatedProtocolVersion) {
    const unsigned short port = 19093;
    mcp::StreamableHttpSessionManager manager(io_ctx_.get_executor(), "127.0.0.1", port,
                                              make_echo_server_factory());

    asio::co_spawn(io_ctx_, manager.listen(), asio::detached);

    RawResponse init_response;
    RawResponse tool_response;
    asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            init_response =
                co_await do_initialize(io_ctx_.get_executor(), port, 1, "2025-06-18", std::nullopt);

            json request = {{"jsonrpc", "2.0"},
                            {"method", "tools/call"},
                            {"id", 2},
                            {"params", {{"name", "echo"}, {"arguments", {{"message", "test"}}}}}};
            tool_response =
                co_await raw_request(io_ctx_.get_executor(), port, http::verb::post, "/mcp",
                                     request.dump(), init_response.session_id, "2025-06-18");
            manager.close();
        },
        asio::detached);

    io_ctx_.run();

    EXPECT_EQ(init_response.status, 200);
    EXPECT_EQ(tool_response.status, 200);
}

TEST_F(SessionManagerTest, SubsequentRequestsAllowMissingNegotiatedProtocolHeader) {
    const unsigned short port = 19094;
    mcp::StreamableHttpSessionManager manager(io_ctx_.get_executor(), "127.0.0.1", port,
                                              make_echo_server_factory());

    asio::co_spawn(io_ctx_, manager.listen(), asio::detached);

    RawResponse init_response;
    RawResponse tool_response;
    RawResponse delete_response;
    asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            init_response =
                co_await do_initialize(io_ctx_.get_executor(), port, 1, "2025-06-18", std::nullopt);

            json request = {{"jsonrpc", "2.0"},
                            {"method", "tools/call"},
                            {"id", 2},
                            {"params", {{"name", "echo"}, {"arguments", {{"message", "test"}}}}}};
            tool_response =
                co_await raw_request(io_ctx_.get_executor(), port, http::verb::post, "/mcp",
                                     request.dump(), init_response.session_id, std::nullopt);
            delete_response = co_await raw_request(io_ctx_.get_executor(), port, http::verb::delete_,
                                                   "/mcp", {}, init_response.session_id, std::nullopt);
            manager.close();
        },
        asio::detached);

    io_ctx_.run();

    EXPECT_EQ(init_response.status, 200);
    EXPECT_EQ(tool_response.status, 200);
    EXPECT_EQ(delete_response.status, 200);
}

// ---------------------------------------------------------------------------
// 2. Route subsequent requests by Mcp-Session-Id header
// ---------------------------------------------------------------------------

TEST_F(SessionManagerTest, RouteBySessionId) {
    const unsigned short port = 19081;
    mcp::StreamableHttpSessionManager manager(io_ctx_.get_executor(), "127.0.0.1", port,
                                              make_echo_server_factory());

    asio::co_spawn(io_ctx_, manager.listen(), asio::detached);

    RawResponse tool_response;
    asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            auto init = co_await do_initialize(io_ctx_.get_executor(), port);
            EXPECT_EQ(init.status, 200);
            auto session_id = init.session_id;

            // Call a tool using the session ID
            json tool_call = {{"jsonrpc", "2.0"},
                              {"method", "tools/call"},
                              {"params", {{"name", "echo"}, {"arguments", {{"message", "hello"}}}}},
                              {"id", 2}};

            tool_response = co_await raw_request(io_ctx_.get_executor(), port, http::verb::post, "/mcp",
                                                 tool_call.dump(), session_id);
            manager.close();
        },
        asio::detached);

    io_ctx_.run();

    EXPECT_EQ(tool_response.status, 200);
    auto body = json::parse(tool_response.body);
    EXPECT_TRUE(body.contains("result"));
    EXPECT_EQ(body["result"]["content"][0]["text"], "hello");
}

// ---------------------------------------------------------------------------
// 3. Unknown session ID returns 404
// ---------------------------------------------------------------------------

TEST_F(SessionManagerTest, UnknownSessionReturns404) {
    const unsigned short port = 19082;
    mcp::StreamableHttpSessionManager manager(io_ctx_.get_executor(), "127.0.0.1", port,
                                              make_echo_server_factory());

    asio::co_spawn(io_ctx_, manager.listen(), asio::detached);

    RawResponse response;
    asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            json request = {{"jsonrpc", "2.0"}, {"method", "ping"}, {"id", 1}};
            response = co_await raw_request(io_ctx_.get_executor(), port, http::verb::post, "/mcp",
                                            request.dump(), "nonexistent-session-id-1234");
            manager.close();
        },
        asio::detached);

    io_ctx_.run();

    EXPECT_EQ(response.status, 404);
    auto body = json::parse(response.body);
    EXPECT_TRUE(body.contains("error"));
    EXPECT_EQ(body["error"]["message"], "Session not found");
}

// ---------------------------------------------------------------------------
// 4. DELETE destroys session; subsequent requests return 404
// ---------------------------------------------------------------------------

TEST_F(SessionManagerTest, DeleteDestroysSession) {
    const unsigned short port = 19083;
    mcp::StreamableHttpSessionManager manager(io_ctx_.get_executor(), "127.0.0.1", port,
                                              make_echo_server_factory());

    asio::co_spawn(io_ctx_, manager.listen(), asio::detached);

    RawResponse delete_response;
    RawResponse after_delete_response;
    asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            auto init = co_await do_initialize(io_ctx_.get_executor(), port);
            auto session_id = init.session_id;
            EXPECT_EQ(manager.session_count(), 1);

            // DELETE the session
            delete_response = co_await raw_request(io_ctx_.get_executor(), port, http::verb::delete_,
                                                   "/mcp", {}, session_id);

            // Try to use the session after deletion
            json request = {{"jsonrpc", "2.0"}, {"method", "ping"}, {"id", 2}};
            after_delete_response = co_await raw_request(io_ctx_.get_executor(), port, http::verb::post,
                                                         "/mcp", request.dump(), session_id);
            manager.close();
        },
        asio::detached);

    io_ctx_.run();

    EXPECT_EQ(delete_response.status, 200);
    EXPECT_EQ(after_delete_response.status, 404);
}

// ---------------------------------------------------------------------------
// 5. Multiple concurrent sessions with same JSON-RPC IDs (the key use case)
// ---------------------------------------------------------------------------

TEST_F(SessionManagerTest, MultipleConcurrentSessionsSameIds) {
    const unsigned short port = 19084;
    mcp::StreamableHttpSessionManager manager(io_ctx_.get_executor(), "127.0.0.1", port,
                                              make_echo_server_factory());

    asio::co_spawn(io_ctx_, manager.listen(), asio::detached);

    RawResponse resp_a;
    RawResponse resp_b;
    asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            // Create session A
            auto init_a = co_await do_initialize(io_ctx_.get_executor(), port, 1);
            auto session_a = init_a.session_id;

            // Create session B
            auto init_b = co_await do_initialize(io_ctx_.get_executor(), port, 1);
            auto session_b = init_b.session_id;

            EXPECT_NE(session_a, session_b);
            EXPECT_EQ(manager.session_count(), 2);

            // Both sessions use the SAME JSON-RPC id (2) — must not collide
            json call_a = {{"jsonrpc", "2.0"},
                           {"method", "tools/call"},
                           {"params", {{"name", "echo"}, {"arguments", {{"message", "from-A"}}}}},
                           {"id", 2}};
            json call_b = {{"jsonrpc", "2.0"},
                           {"method", "tools/call"},
                           {"params", {{"name", "echo"}, {"arguments", {{"message", "from-B"}}}}},
                           {"id", 2}};

            resp_a = co_await raw_request(io_ctx_.get_executor(), port, http::verb::post, "/mcp",
                                          call_a.dump(), session_a);
            resp_b = co_await raw_request(io_ctx_.get_executor(), port, http::verb::post, "/mcp",
                                          call_b.dump(), session_b);

            manager.close();
        },
        asio::detached);

    io_ctx_.run();

    EXPECT_EQ(resp_a.status, 200);
    EXPECT_EQ(resp_b.status, 200);

    auto body_a = json::parse(resp_a.body);
    auto body_b = json::parse(resp_b.body);

    EXPECT_EQ(body_a["result"]["content"][0]["text"], "from-A");
    EXPECT_EQ(body_b["result"]["content"][0]["text"], "from-B");
}

// ---------------------------------------------------------------------------
// 6. Custom request handler intercepts non-MCP requests
// ---------------------------------------------------------------------------

TEST_F(SessionManagerTest, CustomRequestHandler) {
    const unsigned short port = 19085;
    mcp::StreamableHttpSessionManager manager(io_ctx_.get_executor(), "127.0.0.1", port,
                                              make_echo_server_factory());

    manager.set_custom_request_handler([](const http::request<http::string_body>& request)
                                           -> std::optional<http::response<http::string_body>> {
        if (request.target() == "/health") {
            http::response<http::string_body> response{http::status::ok, request.version()};
            response.set(http::field::content_type, "application/json");
            response.body() = R"({"status":"healthy"})";
            response.prepare_payload();
            return response;
        }
        return std::nullopt;
    });

    asio::co_spawn(io_ctx_, manager.listen(), asio::detached);

    RawResponse health_response;
    asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            // Hit the /health endpoint — custom handler should intercept
            beast::tcp_stream stream(io_ctx_.get_executor());
            auto resolver = asio::ip::tcp::resolver(io_ctx_.get_executor());
            auto endpoints =
                co_await resolver.async_resolve("127.0.0.1", std::to_string(port), asio::use_awaitable);
            co_await stream.async_connect(*endpoints.begin(), asio::use_awaitable);

            http::request<http::string_body> request{http::verb::get, "/health", 11};
            request.set(http::field::host, "127.0.0.1");
            request.prepare_payload();

            co_await http::async_write(stream, request, asio::use_awaitable);

            beast::flat_buffer response_buffer;
            http::response<http::string_body> response;
            co_await http::async_read(stream, response_buffer, response, asio::use_awaitable);

            health_response.status = response.result_int();
            health_response.body = response.body();

            beast::error_code shutdown_error;
            stream.socket().shutdown(asio::ip::tcp::socket::shutdown_both, shutdown_error);
            manager.close();
        },
        asio::detached);

    io_ctx_.run();

    EXPECT_EQ(health_response.status, 200);
    auto body = json::parse(health_response.body);
    EXPECT_EQ(body["status"], "healthy");
}

// ---------------------------------------------------------------------------
// 7. Notification (no id) returns 202 Accepted
// ---------------------------------------------------------------------------

TEST_F(SessionManagerTest, NotificationReturns202) {
    const unsigned short port = 19086;
    mcp::StreamableHttpSessionManager manager(io_ctx_.get_executor(), "127.0.0.1", port,
                                              make_echo_server_factory());

    asio::co_spawn(io_ctx_, manager.listen(), asio::detached);

    RawResponse notification_response;
    asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            auto init = co_await do_initialize(io_ctx_.get_executor(), port);
            auto session_id = init.session_id;

            // Send a notification (has method, no id)
            json notification = {{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}};
            notification_response = co_await raw_request(io_ctx_.get_executor(), port, http::verb::post,
                                                         "/mcp", notification.dump(), session_id);

            manager.close();
        },
        asio::detached);

    io_ctx_.run();

    EXPECT_EQ(notification_response.status, 202);
}

// ---------------------------------------------------------------------------
// 8. Bad protocol version returns 400
// ---------------------------------------------------------------------------

TEST_F(SessionManagerTest, BadProtocolVersionReturns400) {
    const unsigned short port = 19087;
    mcp::StreamableHttpSessionManager manager(io_ctx_.get_executor(), "127.0.0.1", port,
                                              make_echo_server_factory());

    asio::co_spawn(io_ctx_, manager.listen(), asio::detached);

    RawResponse response;
    asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            json request = {{"jsonrpc", "2.0"},
                            {"method", "initialize"},
                            {"id", 1},
                            {"params", {{"protocolVersion", "0000-00-00"}}}};
            response = co_await raw_request(io_ctx_.get_executor(), port, http::verb::post, "/mcp",
                                            request.dump(), {}, "0000-00-00");
            manager.close();
        },
        asio::detached);

    io_ctx_.run();

    EXPECT_EQ(response.status, 400);
}

// ---------------------------------------------------------------------------
// 9. Non-initialize request without session header returns 400
// ---------------------------------------------------------------------------

TEST_F(SessionManagerTest, NonInitializeWithoutSessionReturns400) {
    const unsigned short port = 19088;
    mcp::StreamableHttpSessionManager manager(io_ctx_.get_executor(), "127.0.0.1", port,
                                              make_echo_server_factory());

    asio::co_spawn(io_ctx_, manager.listen(), asio::detached);

    RawResponse response;
    asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            // Send a tools/call without session header (not initialize)
            json request = {{"jsonrpc", "2.0"},
                            {"method", "tools/call"},
                            {"id", 1},
                            {"params", {{"name", "echo"}, {"arguments", {{"message", "test"}}}}}};
            response = co_await raw_request(io_ctx_.get_executor(), port, http::verb::post, "/mcp",
                                            request.dump());  // no session_id
            manager.close();
        },
        asio::detached);

    io_ctx_.run();

    EXPECT_EQ(response.status, 400);
    auto body = json::parse(response.body);
    EXPECT_TRUE(body.contains("error"));
}

// ---------------------------------------------------------------------------
// 10. DELETE on unknown session returns 404
// ---------------------------------------------------------------------------

TEST_F(SessionManagerTest, DeleteUnknownSessionReturns404) {
    const unsigned short port = 19089;
    mcp::StreamableHttpSessionManager manager(io_ctx_.get_executor(), "127.0.0.1", port,
                                              make_echo_server_factory());

    asio::co_spawn(io_ctx_, manager.listen(), asio::detached);

    RawResponse response;
    asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            response = co_await raw_request(io_ctx_.get_executor(), port, http::verb::delete_, "/mcp",
                                            {}, "nonexistent-session");
            manager.close();
        },
        asio::detached);

    io_ctx_.run();

    EXPECT_EQ(response.status, 404);
}

// ---------------------------------------------------------------------------
// 11. Session count tracks sessions correctly
// ---------------------------------------------------------------------------

TEST_F(SessionManagerTest, SessionCountTracksCorrectly) {
    const unsigned short port = 19090;
    mcp::StreamableHttpSessionManager manager(io_ctx_.get_executor(), "127.0.0.1", port,
                                              make_echo_server_factory());

    asio::co_spawn(io_ctx_, manager.listen(), asio::detached);

    std::size_t count_after_create = 0;
    std::size_t count_after_second = 0;
    std::size_t count_after_delete = 0;

    asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            auto init_a = co_await do_initialize(io_ctx_.get_executor(), port, 1);
            count_after_create = manager.session_count();

            auto init_b = co_await do_initialize(io_ctx_.get_executor(), port, 1);
            count_after_second = manager.session_count();

            // Delete the first session
            co_await raw_request(io_ctx_.get_executor(), port, http::verb::delete_, "/mcp", {},
                                 init_a.session_id);
            count_after_delete = manager.session_count();

            manager.close();
        },
        asio::detached);

    io_ctx_.run();

    EXPECT_EQ(count_after_create, 1);
    EXPECT_EQ(count_after_second, 2);
    EXPECT_EQ(count_after_delete, 1);
}

// ---------------------------------------------------------------------------
// 12. Method not allowed returns 405
// ---------------------------------------------------------------------------

TEST_F(SessionManagerTest, UnsupportedMethodReturns405) {
    const unsigned short port = 19091;
    mcp::StreamableHttpSessionManager manager(io_ctx_.get_executor(), "127.0.0.1", port,
                                              make_echo_server_factory());

    asio::co_spawn(io_ctx_, manager.listen(), asio::detached);

    RawResponse response;
    asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            response = co_await raw_request(io_ctx_.get_executor(), port, http::verb::put, "/mcp",
                                            R"({"test":"data"})");
            manager.close();
        },
        asio::detached);

    io_ctx_.run();

    EXPECT_EQ(response.status, 405);
}

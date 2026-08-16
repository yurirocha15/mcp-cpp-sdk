#include "mcp/transport/http_session_manager.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <chrono>
#include <exception>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

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
    std::string allow;
};

/// Fire a single HTTP request and return the response.
/// Must be called inside a coroutine on the same io_context.
mcp::Task<RawResponse> raw_request(
    const asio::any_io_executor& executor, unsigned short port, http::verb method,
    const std::string& target, const std::string& body = {}, const std::string& session_id = {},
    std::optional<std::string> protocol_version = std::string(mcp::g_LATEST_PROTOCOL_VERSION),
    std::optional<std::string> origin = std::nullopt,
    std::optional<std::string> bearer_token = std::nullopt, std::string accept = "application/json") {
    beast::tcp_stream stream(executor);
    auto resolver = asio::ip::tcp::resolver(executor);
    auto endpoints =
        co_await resolver.async_resolve("127.0.0.1", std::to_string(port), asio::use_awaitable);
    co_await stream.async_connect(*endpoints.begin(), asio::use_awaitable);

    http::request<http::string_body> request{method, target, 11};
    request.set(http::field::host, "127.0.0.1");
    request.set(http::field::content_type, "application/json");
    request.set(http::field::accept, accept);

    if (protocol_version.has_value()) {
        request.set("MCP-Protocol-Version", *protocol_version);
    }

    if (!session_id.empty()) {
        request.set("Mcp-Session-Id", session_id);
    }
    if (origin.has_value()) {
        request.set(http::field::origin, *origin);
    }
    if (bearer_token.has_value()) {
        request.set(http::field::authorization, "Bearer " + *bearer_token);
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
    result.allow = std::string(response[http::field::allow]);

    auto session_it = response.find("Mcp-Session-Id");
    if (session_it != response.end()) {
        result.session_id = std::string(session_it->value());
    }

    beast::error_code shutdown_error;
    stream.socket().shutdown(asio::ip::tcp::socket::shutdown_both, shutdown_error);

    co_return result;
}

std::vector<std::pair<std::string, json>> parse_sse_events(const std::string& body) {
    std::vector<std::pair<std::string, json>> events;
    std::istringstream stream(body);
    std::string event_id;
    std::string line;

    while (std::getline(stream, line)) {
        if (line.starts_with("id: ")) {
            event_id = line.substr(4);
        } else if (line.starts_with("data: ")) {
            events.emplace_back(event_id, json::parse(line.substr(6)));
            event_id.clear();
        }
    }

    return events;
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

        server->add_tool<json, mcp::CallToolResult>(
            "echo", "Echoes input",
            json{{"type", "object"}, {"properties", {{"message", {{"type", "string"}}}}}},
            [](json params) -> mcp::Task<mcp::CallToolResult> {
                co_return mcp::make_tool_text_result(params.value("message", "empty"));
            });

        return server;
    };
}

mcp::StreamableHttpSessionManager::ServerFactory make_notifying_server_factory() {
    return [](const asio::any_io_executor&) -> std::unique_ptr<mcp::Server> {
        mcp::ServerCapabilities caps;
        caps.tools = mcp::ServerCapabilities::ToolsCapability{};
        caps.logging = json::object();
        auto server = std::make_unique<mcp::Server>(
            mcp::Implementation{"test-notifying-server", "1.0.0"}, std::move(caps));

        server->add_tool<json, mcp::CallToolResult>(
            "notify", "Sends notifications before returning", json{{"type", "object"}},
            [](mcp::Context& context, json) -> mcp::Task<mcp::CallToolResult> {
                co_await context.log_info("tool started");
                co_await context.report_progress(0.5, 1.0, "halfway");
                co_return mcp::make_tool_text_result("done");
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

mcp::Task<RawResponse> send_initialized(
    const asio::any_io_executor& executor, unsigned short port, const std::string& session_id,
    std::optional<std::string> header_protocol_version = std::string(mcp::g_LATEST_PROTOCOL_VERSION)) {
    json notification = {{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}};

    co_return co_await raw_request(executor, port, http::verb::post, "/mcp", notification.dump(),
                                   session_id, std::move(header_protocol_version));
}

}  // namespace

// ===========================================================================
// Test fixture
// ===========================================================================

class SessionManagerTest : public ::testing::Test {
   protected:
    asio::io_context io_ctx_;
};

TEST_F(SessionManagerTest, SsePostReturnsNotificationsThenFinalResponseInOrder) {
    const unsigned short port = 19111;
    mcp::StreamableHttpSessionManager manager(io_ctx_.get_executor(), "127.0.0.1", port,
                                              make_notifying_server_factory());

    asio::co_spawn(io_ctx_, manager.listen(), asio::detached);

    RawResponse tool_response;
    asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            auto init = co_await do_initialize(io_ctx_.get_executor(), port);
            EXPECT_EQ(init.status, 200);
            auto initialized = co_await send_initialized(io_ctx_.get_executor(), port, init.session_id);
            EXPECT_EQ(initialized.status, 202);

            json request = {{"jsonrpc", "2.0"},
                            {"method", "tools/call"},
                            {"params",
                             {{"name", "notify"},
                              {"arguments", json::object()},
                              {"_meta", {{"progressToken", "progress-1"}}}}},
                            {"id", 2}};
            tool_response = co_await raw_request(
                io_ctx_.get_executor(), port, http::verb::post, "/mcp", request.dump(), init.session_id,
                std::string(mcp::g_LATEST_PROTOCOL_VERSION), std::nullopt, std::nullopt,
                "application/json, text/event-stream");
            manager.close();
        },
        asio::detached);

    io_ctx_.run();

    ASSERT_EQ(tool_response.status, 200);
    EXPECT_EQ(tool_response.content_type, "text/event-stream");

    const auto events = parse_sse_events(tool_response.body);
    ASSERT_EQ(events.size(), 3);
    EXPECT_EQ(events[0].second.at("method"), "notifications/message");
    EXPECT_EQ(events[0].second.at("params").at("data"), "tool started");
    EXPECT_EQ(events[1].second.at("method"), "notifications/progress");
    EXPECT_EQ(events[1].second.at("params").at("progressToken"), "progress-1");
    EXPECT_EQ(events[2].second.at("id"), 2);
    EXPECT_EQ(events[2].second.at("result").at("content").at(0).at("text"), "done");
    EXPECT_LT(std::stoull(events[0].first), std::stoull(events[1].first));
    EXPECT_LT(std::stoull(events[1].first), std::stoull(events[2].first));
}

TEST_F(SessionManagerTest, JsonOnlyPostSkipsNotificationsAndReturnsFinalResponse) {
    const unsigned short port = 19112;
    mcp::StreamableHttpSessionManager manager(io_ctx_.get_executor(), "127.0.0.1", port,
                                              make_notifying_server_factory());
    manager.set_json_only(true);

    asio::co_spawn(io_ctx_, manager.listen(), asio::detached);

    RawResponse tool_response;
    asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            auto init = co_await do_initialize(io_ctx_.get_executor(), port);
            EXPECT_EQ(init.status, 200);
            auto initialized = co_await send_initialized(io_ctx_.get_executor(), port, init.session_id);
            EXPECT_EQ(initialized.status, 202);

            json request = {{"jsonrpc", "2.0"},
                            {"method", "tools/call"},
                            {"params",
                             {{"name", "notify"},
                              {"arguments", json::object()},
                              {"_meta", {{"progressToken", "progress-1"}}}}},
                            {"id", 2}};
            tool_response = co_await raw_request(
                io_ctx_.get_executor(), port, http::verb::post, "/mcp", request.dump(), init.session_id,
                std::string(mcp::g_LATEST_PROTOCOL_VERSION), std::nullopt, std::nullopt,
                "application/json, text/event-stream");
            manager.close();
        },
        asio::detached);

    io_ctx_.run();

    ASSERT_EQ(tool_response.status, 200);
    EXPECT_EQ(tool_response.content_type, "application/json");
    const auto response = json::parse(tool_response.body);
    EXPECT_EQ(response.at("id"), 2);
    EXPECT_EQ(response.at("result").at("content").at(0).at("text"), "done");
}

TEST_F(SessionManagerTest, OriginHeadersAreDeniedUntilExplicitlyAllowed) {
    const unsigned short port = 19094;
    mcp::StreamableHttpSessionManager manager(io_ctx_.get_executor(), "127.0.0.1", port,
                                              make_echo_server_factory());
    manager.set_allowed_origins({"https://trusted.example"});

    asio::co_spawn(io_ctx_, manager.listen(), asio::detached);

    RawResponse denied_response;
    RawResponse allowed_response;
    asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            const auto init_body =
                json{{"jsonrpc", "2.0"},
                     {"method", "initialize"},
                     {"params",
                      {{"protocolVersion", std::string(mcp::g_LATEST_PROTOCOL_VERSION)},
                       {"clientInfo", {{"name", "test"}, {"version", "1"}}},
                       {"capabilities", json::object()}}},
                     {"id", 1}}
                    .dump();
            denied_response = co_await raw_request(
                io_ctx_.get_executor(), port, http::verb::post, "/mcp", init_body, {},
                std::string(mcp::g_LATEST_PROTOCOL_VERSION), "https://untrusted.example");
            allowed_response = co_await raw_request(
                io_ctx_.get_executor(), port, http::verb::post, "/mcp", init_body, {},
                std::string(mcp::g_LATEST_PROTOCOL_VERSION), "https://trusted.example");
            manager.close();
        },
        asio::detached);

    io_ctx_.run();

    EXPECT_EQ(denied_response.status, 403);
    EXPECT_EQ(allowed_response.status, 200);
}

TEST_F(SessionManagerTest, BearerTokensAreValidatedAtHttpBoundary) {
    const unsigned short port = 19095;
    mcp::StreamableHttpSessionManager manager(io_ctx_.get_executor(), "127.0.0.1", port,
                                              make_echo_server_factory());
    manager.set_bearer_token_validator([](std::string_view token) { return token == "valid-token"; });

    asio::co_spawn(io_ctx_, manager.listen(), asio::detached);

    RawResponse missing_response;
    RawResponse invalid_response;
    RawResponse valid_response;
    asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            const auto init_body =
                json{{"jsonrpc", "2.0"},
                     {"method", "initialize"},
                     {"params",
                      {{"protocolVersion", std::string(mcp::g_LATEST_PROTOCOL_VERSION)},
                       {"clientInfo", {{"name", "test"}, {"version", "1"}}},
                       {"capabilities", json::object()}}},
                     {"id", 1}}
                    .dump();
            missing_response =
                co_await raw_request(io_ctx_.get_executor(), port, http::verb::post, "/mcp", init_body);
            invalid_response = co_await raw_request(
                io_ctx_.get_executor(), port, http::verb::post, "/mcp", init_body, {},
                std::string(mcp::g_LATEST_PROTOCOL_VERSION), std::nullopt, "invalid-token");
            valid_response = co_await raw_request(
                io_ctx_.get_executor(), port, http::verb::post, "/mcp", init_body, {},
                std::string(mcp::g_LATEST_PROTOCOL_VERSION), std::nullopt, "valid-token");
            manager.close();
        },
        asio::detached);

    io_ctx_.run();

    EXPECT_EQ(missing_response.status, 401);
    EXPECT_EQ(invalid_response.status, 401);
    EXPECT_EQ(valid_response.status, 200);
}

TEST_F(SessionManagerTest, SecurityChecksRunBeforeCustomHttpHandlers) {
    const unsigned short port = 19110;
    mcp::StreamableHttpSessionManager manager(io_ctx_.get_executor(), "127.0.0.1", port,
                                              make_echo_server_factory());
    manager.set_bearer_token_validator([](std::string_view token) { return token == "valid-token"; });
    manager.set_custom_request_handler(
        [](const mcp::StringRequest& request) -> std::optional<mcp::StringResponse> {
            if (request.target() != "/health") {
                return std::nullopt;
            }
            mcp::StringResponse response{http::status::ok, request.version()};
            response.body() = "healthy";
            response.prepare_payload();
            return response;
        });

    asio::co_spawn(io_ctx_, manager.listen(), asio::detached);

    RawResponse missing_response;
    RawResponse valid_response;
    asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            missing_response =
                co_await raw_request(io_ctx_.get_executor(), port, http::verb::get, "/health");
            valid_response =
                co_await raw_request(io_ctx_.get_executor(), port, http::verb::get, "/health", {}, {},
                                     std::nullopt, std::nullopt, "valid-token");
            manager.close();
        },
        asio::detached);

    io_ctx_.run();

    EXPECT_EQ(missing_response.status, 401);
    EXPECT_EQ(valid_response.status, 200);
    EXPECT_EQ(valid_response.body, "healthy");
}

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
            auto initialized = co_await send_initialized(io_ctx_.get_executor(), port,
                                                         init_response.session_id, "2025-06-18");
            EXPECT_EQ(initialized.status, 202);

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

TEST_F(SessionManagerTest, ProtocolVersionStateIsSafeAcrossConnectionStrands) {
    constexpr unsigned short port = 19115;
    constexpr std::size_t request_count = 32;
    mcp::StreamableHttpSessionManager manager(io_ctx_.get_executor(), "127.0.0.1", port,
                                              make_echo_server_factory());

    asio::co_spawn(io_ctx_, manager.listen(), asio::detached);

    std::vector<RawResponse> responses(request_count);
    std::vector<std::exception_ptr> errors(request_count);
    asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            auto init = co_await do_initialize(io_ctx_.get_executor(), port);
            EXPECT_EQ(init.status, 200);

            auto remaining = std::make_shared<std::atomic<std::size_t>>(request_count);
            auto all_done = std::make_shared<asio::steady_timer>(io_ctx_);
            all_done->expires_at(std::chrono::steady_clock::time_point::max());

            for (std::size_t index = 0; index < request_count; ++index) {
                asio::co_spawn(
                    io_ctx_,
                    [&, index, session_id = init.session_id]() -> mcp::Task<void> {
                        if (index % 2 == 0) {
                            json reinitialize = {
                                {"jsonrpc", "2.0"},
                                {"method", "initialize"},
                                {"params",
                                 {{"protocolVersion", "2025-06-18"},
                                  {"clientInfo", {{"name", "race-test"}, {"version", "1"}}},
                                  {"capabilities", json::object()}}},
                                {"id", static_cast<int>(100 + index)}};
                            responses[index] = co_await raw_request(
                                io_ctx_.get_executor(), port, http::verb::post, "/mcp",
                                reinitialize.dump(), session_id, "2025-06-18");
                        } else {
                            const auto version = index % 4 == 1
                                                     ? std::string("2025-06-18")
                                                     : std::string(mcp::g_LATEST_PROTOCOL_VERSION);
                            responses[index] =
                                co_await raw_request(io_ctx_.get_executor(), port, http::verb::get,
                                                     "/mcp", {}, session_id, version);
                        }
                    },
                    [&, index, remaining, all_done](std::exception_ptr error) {
                        errors[index] = std::move(error);
                        if (remaining->fetch_sub(1, std::memory_order_acq_rel) == 1) {
                            asio::post(all_done->get_executor(), [all_done]() {
                                all_done->expires_at(std::chrono::steady_clock::now());
                            });
                        }
                    });
            }

            boost::system::error_code wait_error;
            co_await all_done->async_wait(asio::redirect_error(asio::use_awaitable, wait_error));
            manager.close();
        },
        asio::detached);

    std::vector<std::thread> workers;
    workers.reserve(4);
    for (int index = 0; index < 4; ++index) {
        workers.emplace_back([this]() { io_ctx_.run(); });
    }
    for (auto& worker : workers) {
        worker.join();
    }

    for (std::size_t index = 0; index < request_count; ++index) {
        EXPECT_EQ(errors[index], nullptr) << "request " << index;
        EXPECT_TRUE(responses[index].status == 200 || responses[index].status == 400)
            << "request " << index << " returned " << responses[index].status;
    }
}

TEST_F(SessionManagerTest, SubsequentRequestsAllowMissingNegotiatedProtocolHeader) {
    const unsigned short port = 19113;
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
            auto initialized = co_await send_initialized(io_ctx_.get_executor(), port,
                                                         init_response.session_id, std::nullopt);
            EXPECT_EQ(initialized.status, 202);

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
            auto initialized = co_await send_initialized(io_ctx_.get_executor(), port, session_id);
            EXPECT_EQ(initialized.status, 202);

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
            auto initialized_a = co_await send_initialized(io_ctx_.get_executor(), port, session_a);
            EXPECT_EQ(initialized_a.status, 202);

            // Create session B
            auto init_b = co_await do_initialize(io_ctx_.get_executor(), port, 1);
            auto session_b = init_b.session_id;
            auto initialized_b = co_await send_initialized(io_ctx_.get_executor(), port, session_b);
            EXPECT_EQ(initialized_b.status, 202);

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

TEST_F(SessionManagerTest, StatelessInitializeDoesNotCreateSessionHeader) {
    const unsigned short port = 19114;
    mcp::StreamableHttpSessionManager manager(io_ctx_.get_executor(), "127.0.0.1", port,
                                              make_echo_server_factory());
    manager.set_stateless_json_mode(true);

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
    EXPECT_TRUE(init_response.session_id.empty());
    EXPECT_EQ(manager.session_count(), 0);

    auto body = json::parse(init_response.body);
    EXPECT_TRUE(body.contains("result"));
    EXPECT_EQ(body["result"]["protocolVersion"], std::string(mcp::g_LATEST_PROTOCOL_VERSION));
}

TEST_F(SessionManagerTest, StatelessToolsCallAndToolsListWorkWithoutSession) {
    const unsigned short port = 19096;
    mcp::StreamableHttpSessionManager manager(io_ctx_.get_executor(), "127.0.0.1", port,
                                              make_echo_server_factory());
    manager.set_stateless_json_mode(true);

    asio::co_spawn(io_ctx_, manager.listen(), asio::detached);

    RawResponse tool_response;
    RawResponse list_response;
    asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            json tool_call = {{"jsonrpc", "2.0"},
                              {"method", "tools/call"},
                              {"params", {{"name", "echo"}, {"arguments", {{"message", "hello"}}}}},
                              {"id", 2}};
            tool_response = co_await raw_request(io_ctx_.get_executor(), port, http::verb::post, "/mcp",
                                                 tool_call.dump());

            json list_call = {
                {"jsonrpc", "2.0"}, {"method", "tools/list"}, {"params", json::object()}, {"id", 3}};
            list_response = co_await raw_request(io_ctx_.get_executor(), port, http::verb::post, "/mcp",
                                                 list_call.dump());
            manager.close();
        },
        asio::detached);

    io_ctx_.run();

    EXPECT_EQ(tool_response.status, 200);
    EXPECT_TRUE(tool_response.session_id.empty());
    auto tool_body = json::parse(tool_response.body);
    EXPECT_EQ(tool_body["result"]["content"][0]["text"], "hello");

    EXPECT_EQ(list_response.status, 200);
    EXPECT_TRUE(list_response.session_id.empty());
    auto list_body = json::parse(list_response.body);
    ASSERT_TRUE(list_body["result"].contains("tools"));
    ASSERT_EQ(list_body["result"]["tools"].size(), 1);
    EXPECT_EQ(list_body["result"]["tools"][0]["name"], "echo");
}

TEST_F(SessionManagerTest, StatelessNotificationReturns202WithoutSession) {
    const unsigned short port = 19097;
    mcp::StreamableHttpSessionManager manager(io_ctx_.get_executor(), "127.0.0.1", port,
                                              make_echo_server_factory());
    manager.set_stateless_json_mode(true);

    asio::co_spawn(io_ctx_, manager.listen(), asio::detached);

    RawResponse notification_response;
    asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            json notification = {{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}};
            notification_response = co_await raw_request(io_ctx_.get_executor(), port, http::verb::post,
                                                         "/mcp", notification.dump());
            manager.close();
        },
        asio::detached);

    io_ctx_.run();

    EXPECT_EQ(notification_response.status, 202);
    EXPECT_TRUE(notification_response.session_id.empty());
}

TEST_F(SessionManagerTest, StatelessRejectsGetAndDeleteWithPostOnlyAllowHeader) {
    const unsigned short port = 19098;
    mcp::StreamableHttpSessionManager manager(io_ctx_.get_executor(), "127.0.0.1", port,
                                              make_echo_server_factory());
    manager.set_stateless_json_mode(true);

    asio::co_spawn(io_ctx_, manager.listen(), asio::detached);

    RawResponse get_response;
    RawResponse delete_response;
    asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            get_response = co_await raw_request(io_ctx_.get_executor(), port, http::verb::get, "/mcp");
            delete_response =
                co_await raw_request(io_ctx_.get_executor(), port, http::verb::delete_, "/mcp");
            manager.close();
        },
        asio::detached);

    io_ctx_.run();

    EXPECT_EQ(get_response.status, 405);
    EXPECT_EQ(get_response.allow, "POST");
    EXPECT_EQ(delete_response.status, 405);
    EXPECT_EQ(delete_response.allow, "POST");
}

TEST_F(SessionManagerTest, StatelessBadProtocolVersionReturns400) {
    const unsigned short port = 19099;
    mcp::StreamableHttpSessionManager manager(io_ctx_.get_executor(), "127.0.0.1", port,
                                              make_echo_server_factory());
    manager.set_stateless_json_mode(true);

    asio::co_spawn(io_ctx_, manager.listen(), asio::detached);

    RawResponse response;
    asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            json request = {{"jsonrpc", "2.0"}, {"method", "tools/list"}, {"id", 1}};
            response = co_await raw_request(io_ctx_.get_executor(), port, http::verb::post, "/mcp",
                                            request.dump(), {}, "0000-00-00");
            manager.close();
        },
        asio::detached);

    io_ctx_.run();

    EXPECT_EQ(response.status, 400);
    EXPECT_TRUE(response.session_id.empty());
}

TEST_F(SessionManagerTest, CloseCancelsIdleKeepAliveAndCannotCreatePostCloseSession) {
    const unsigned short port = 19115;
    std::atomic<int> factory_calls{0};
    std::atomic<int> custom_handler_calls{0};
    auto echo_factory = make_echo_server_factory();
    mcp::StreamableHttpSessionManager manager(
        io_ctx_.get_executor(), "127.0.0.1", port,
        [&factory_calls, echo_factory](const asio::any_io_executor& executor) mutable {
            factory_calls.fetch_add(1, std::memory_order_relaxed);
            return echo_factory(executor);
        });
    manager.set_custom_request_handler([&custom_handler_calls](const mcp::StringRequest& request)
                                           -> std::optional<mcp::StringResponse> {
        custom_handler_calls.fetch_add(1, std::memory_order_relaxed);
        if (request.target() != "/health") {
            return std::nullopt;
        }
        mcp::StringResponse response{http::status::ok, request.version()};
        response.set(http::field::content_type, "application/json");
        response.keep_alive(request.keep_alive());
        response.body() = R"({"status":"ok"})";
        response.prepare_payload();
        return response;
    });

    auto client = std::make_shared<beast::tcp_stream>(io_ctx_.get_executor());
    auto deadline = std::make_shared<asio::steady_timer>(io_ctx_.get_executor());
    deadline->expires_after(std::chrono::seconds(2));
    bool first_request_completed = false;
    bool connection_cancelled = false;
    bool post_close_response_received = false;
    bool timed_out = false;
    std::exception_ptr client_error;

    asio::co_spawn(io_ctx_, manager.listen(), asio::detached);
    asio::co_spawn(
        io_ctx_,
        [&manager, client, deadline, port, &first_request_completed, &connection_cancelled,
         &post_close_response_received]() -> mcp::Task<void> {
            asio::ip::tcp::resolver resolver(client->get_executor());
            auto endpoints =
                co_await resolver.async_resolve("127.0.0.1", std::to_string(port), asio::use_awaitable);
            co_await client->async_connect(*endpoints.begin(), asio::use_awaitable);

            http::request<http::string_body> health_request{http::verb::get, "/health", 11};
            health_request.set(http::field::host, "127.0.0.1");
            health_request.keep_alive(true);
            co_await http::async_write(*client, health_request, asio::use_awaitable);

            beast::flat_buffer response_buffer;
            http::response<http::string_body> health_response;
            co_await http::async_read(*client, response_buffer, health_response, asio::use_awaitable);
            first_request_completed = health_response.result() == http::status::ok;

            manager.close();

            http::request<http::string_body> initialize_request{http::verb::post, "/mcp", 11};
            initialize_request.set(http::field::host, "127.0.0.1");
            initialize_request.set(http::field::content_type, "application/json");
            initialize_request.set("MCP-Protocol-Version", mcp::g_LATEST_PROTOCOL_VERSION);
            initialize_request.keep_alive(true);
            initialize_request.body() =
                R"({"jsonrpc":"2.0","method":"initialize","params":{"protocolVersion":"2025-06-18","clientInfo":{"name":"post-close","version":"1.0"},"capabilities":{}},"id":1})";
            initialize_request.prepare_payload();

            try {
                co_await http::async_write(*client, initialize_request, asio::use_awaitable);
                http::response<http::string_body> initialize_response;
                co_await http::async_read(*client, response_buffer, initialize_response,
                                          asio::use_awaitable);
                post_close_response_received = true;
            } catch (const boost::system::system_error&) {
                connection_cancelled = true;
            }
            deadline->cancel();
        },
        [&client_error, deadline, &manager](std::exception_ptr error) {
            client_error = std::move(error);
            deadline->cancel();
            manager.close();
        });
    deadline->async_wait([client, &manager, &timed_out](const boost::system::error_code& error) {
        if (error) {
            return;
        }
        timed_out = true;
        manager.close();
        boost::system::error_code ignored;
        (void)client->socket().close(ignored);
    });

    io_ctx_.run();

    EXPECT_EQ(client_error, nullptr);
    EXPECT_FALSE(timed_out);
    EXPECT_TRUE(first_request_completed);
    EXPECT_TRUE(connection_cancelled);
    EXPECT_FALSE(post_close_response_received);
    EXPECT_EQ(custom_handler_calls.load(std::memory_order_relaxed), 1);
    EXPECT_EQ(factory_calls.load(std::memory_order_relaxed), 0);
    EXPECT_EQ(manager.session_count(), 0);
}

TEST_F(SessionManagerTest, NonAtomicConfigurationLocksWhenListeningStarts) {
    const unsigned short port = 19116;
    mcp::StreamableHttpSessionManager manager(io_ctx_.get_executor(), "127.0.0.1", port,
                                              make_echo_server_factory());
    auto listener = manager.listen();

    EXPECT_NO_THROW(manager.set_json_only(true));
    EXPECT_THROW(manager.set_custom_request_handler({}), std::logic_error);
    EXPECT_THROW(manager.set_allowed_origins({"https://trusted.example"}), std::logic_error);
    EXPECT_THROW(manager.set_allow_all_origins(true), std::logic_error);
    EXPECT_THROW(manager.set_bearer_token_validator({}), std::logic_error);
    EXPECT_THROW(manager.set_stateless_json_mode(true), std::logic_error);
    EXPECT_THROW(manager.set_tool_executor(io_ctx_.get_executor()), std::logic_error);

    manager.close();
    asio::co_spawn(io_ctx_, std::move(listener), asio::detached);
    io_ctx_.run();
}

TEST_F(SessionManagerTest, StatelessDispatchUsesConfiguredToolExecutor) {
    const unsigned short port = 19117;
    const auto http_thread = std::this_thread::get_id();
    std::atomic<bool> handler_ran{false};
    std::atomic<bool> handler_ran_on_http_thread{true};
    asio::thread_pool tool_pool(1);

    mcp::StreamableHttpSessionManager manager(
        io_ctx_.get_executor(), "127.0.0.1", port,
        [&handler_ran, &handler_ran_on_http_thread,
         http_thread](const asio::any_io_executor&) -> std::unique_ptr<mcp::Server> {
            mcp::ServerCapabilities capabilities;
            capabilities.tools = mcp::ServerCapabilities::ToolsCapability{};
            auto server = std::make_unique<mcp::Server>(
                mcp::Implementation{"executor-test-server", "1.0.0"}, std::move(capabilities));
            server->add_tool<json, mcp::CallToolResult>(
                "executor", "Reports executor affinity", json{{"type", "object"}},
                [&handler_ran, &handler_ran_on_http_thread,
                 http_thread](json) -> mcp::Task<mcp::CallToolResult> {
                    handler_ran_on_http_thread.store(std::this_thread::get_id() == http_thread,
                                                     std::memory_order_release);
                    handler_ran.store(true, std::memory_order_release);
                    co_return mcp::make_tool_text_result("tool executor");
                });
            return server;
        });
    manager.set_stateless_json_mode(true);
    manager.set_tool_executor(tool_pool.get_executor());

    asio::co_spawn(io_ctx_, manager.listen(), asio::detached);
    RawResponse response;
    asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            const json request = {{"jsonrpc", "2.0"},
                                  {"method", "tools/call"},
                                  {"params", {{"name", "executor"}, {"arguments", json::object()}}},
                                  {"id", 1}};
            response = co_await raw_request(io_ctx_.get_executor(), port, http::verb::post, "/mcp",
                                            request.dump());
            manager.close();
        },
        asio::detached);

    io_ctx_.run();
    tool_pool.join();

    EXPECT_EQ(response.status, 200);
    EXPECT_TRUE(handler_ran.load(std::memory_order_acquire));
    EXPECT_FALSE(handler_ran_on_http_thread.load(std::memory_order_acquire));
    EXPECT_EQ(json::parse(response.body)["result"]["content"][0]["text"], "tool executor");
}

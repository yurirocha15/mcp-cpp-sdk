#include "../test_utils.hpp"

#include "mcp/server/server.hpp"
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
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using nlohmann::json;
using Headers = std::vector<std::pair<std::string, std::string>>;

struct Reply {
    unsigned int status{0};
    json message;
};

/// The JSON-RPC message of a reply body: the body itself, or the data line of an SSE event.
json message_of(const std::string& body) {
    std::string_view text = body;
    if (const auto data = text.find("data: "); data != std::string_view::npos) {
        text.remove_prefix(data + 6);
        text = text.substr(0, text.find('\n'));
    }
    return json::parse(text, nullptr, false);
}

/// The headers a 2026-07-28 client sends with a tools/call of @p tool.
Headers call_headers(const std::string& tool, std::optional<std::string> authorization) {
    Headers headers{
        {"MCP-Protocol-Version", "2026-07-28"}, {"Mcp-Method", "tools/call"}, {"Mcp-Name", tool}};
    if (authorization) {
        headers.emplace_back("Authorization", std::move(*authorization));
    }
    return headers;
}

http::request<http::string_body> make_post(std::string body, const Headers& headers) {
    http::request<http::string_body> request{http::verb::post, "/mcp", 11};
    request.set(http::field::host, "127.0.0.1");
    request.set(http::field::content_type, "application/json");
    request.set(http::field::accept, "application/json, text/event-stream");
    for (const auto& [name, value] : headers) {
        request.insert(name, value);
    }
    request.body() = std::move(body);
    request.prepare_payload();
    return request;
}

// Everything is taken by value: this is a coroutine, and callers pass temporaries.
mcp::Task<Reply> send(asio::any_io_executor executor, unsigned short port, std::string body,
                      Headers headers) {
    beast::tcp_stream stream(executor);
    asio::ip::tcp::resolver resolver(executor);
    auto endpoints =
        co_await resolver.async_resolve("127.0.0.1", std::to_string(port), asio::use_awaitable);
    co_await stream.async_connect(*endpoints.begin(), asio::use_awaitable);
    auto request = make_post(std::move(body), headers);
    co_await http::async_write(stream, request, asio::use_awaitable);

    beast::flat_buffer buffer;
    http::response<http::string_body> response;
    co_await http::async_read(stream, buffer, response, asio::use_awaitable);
    Reply reply;
    reply.status = response.result_int();
    reply.message = message_of(response.body());
    beast::error_code ignored;
    stream.socket().shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
    co_return reply;
}

/// Waits, a millisecond at a time, until @p condition holds. Gives up after 30 s so a broken
/// rendezvous fails the test instead of hanging the run.
mcp::Task<bool> wait_until(std::function<bool()> condition) {
    auto executor = co_await asio::this_coro::executor;
    const auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!condition() && std::chrono::steady_clock::now() < give_up) {
        asio::steady_timer timer(executor, std::chrono::milliseconds(1));
        co_await timer.async_wait(asio::use_awaitable);
    }
    co_return condition();
}

json form_elicitation(const std::string& message) {
    mcp::ElicitRequestFormParams params;
    params.message = message;
    params.requestedSchema = {{"type", "object"}, {"properties", {{"name", {{"type", "string"}}}}}};
    return params;
}

/// A tools/call of @p tool from a client that declares form elicitation.
json call_tool(const json& id, const std::string& tool) {
    auto request =
        make_modern_request(id, "tools/call", {{"name", tool}, {"arguments", json::object()}});
    request["params"]["_meta"]["io.modelcontextprotocol/clientCapabilities"] = {
        {"elicitation", json::object()}};
    return request;
}

/// @p request retried under @p id with the state and an accepted answer to the only question of
/// @p first.
json retry_of(json request, const json& id, const json& first, const std::string& name) {
    const auto& result = first["result"];
    request["id"] = id;
    request["params"]["requestState"] = result["requestState"];
    json responses = json::object();
    if (!result["inputRequests"].empty()) {
        responses[result["inputRequests"].begin().key()] = {{"action", "accept"},
                                                            {"content", {{"name", name}}}};
    }
    request["params"]["inputResponses"] = std::move(responses);
    return request;
}

class HttpMrtrTest : public ::testing::Test {
   protected:
    asio::io_context io_ctx_;
    std::shared_ptr<std::atomic<bool>> resumed_ = std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<std::atomic<bool>> cancelled_ = std::make_shared<std::atomic<bool>>(false);

    mcp::StreamableHttpSessionManager::ServerFactory factory() {
        return [resumed = resumed_, cancelled = cancelled_](const asio::any_io_executor&) {
            mcp::ServerCapabilities caps;
            caps.tools = mcp::ServerCapabilities::ToolsCapability{};
            auto server = std::make_unique<mcp::Server>(mcp::Implementation{"mrtr-http", "1.0"}, caps);
            server->add_tool<json, json>(
                "ask", "Asks for a name", json{{"type", "object"}},
                [](mcp::Context& ctx, json /*args*/) -> mcp::Task<json> {
                    const auto answer = co_await ctx.elicit(form_elicitation("Your name?"));
                    co_return json{{"name", answer.content.value_or(json::object()).value("name", "")}};
                });
            server->add_tool<json, json>(
                "ask_then_wait", "Asks, then waits for cancellation", json{{"type", "object"}},
                [resumed, cancelled](mcp::Context& ctx, json /*args*/) -> mcp::Task<json> {
                    co_await ctx.elicit(form_elicitation("Name?"));
                    resumed->store(true);
                    const bool seen = co_await wait_until([&ctx] { return ctx.is_cancelled(); });
                    cancelled->store(seen);
                    co_return json{{"cancelled", seen}};
                });
            return server;
        };
    }

    template <typename Exchange>
    void run(mcp::StreamableHttpSessionManager& manager, Exchange exchange) {
        asio::co_spawn(io_ctx_, manager.listen(), asio::detached);
        asio::co_spawn(
            io_ctx_,
            [&manager, exchange = std::move(exchange)]() mutable -> mcp::Task<void> {
                co_await exchange();
                manager.close();
            },
            asio::detached);
        io_ctx_.run();
    }
};

TEST_F(HttpMrtrTest, RoundTripOverTheSessionManager) {
    constexpr unsigned short g_port = 20600;
    mcp::StreamableHttpSessionManager manager(io_ctx_.get_executor(), "127.0.0.1", g_port, factory());
    Reply first;
    Reply second;
    run(manager, [&]() -> mcp::Task<void> {
        const auto request = call_tool(1, "ask");
        first = co_await send(io_ctx_.get_executor(), g_port, request.dump(),
                              call_headers("ask", std::nullopt));
        second = co_await send(io_ctx_.get_executor(), g_port,
                               retry_of(request, 2, first.message, "octocat").dump(),
                               call_headers("ask", std::nullopt));
    });

    EXPECT_EQ(first.status, 200u) << first.message;
    ASSERT_EQ(first.message["result"]["resultType"], "input_required") << first.message;
    EXPECT_EQ(second.status, 200u) << second.message;
    EXPECT_EQ(second.message["id"], 2);
    EXPECT_EQ(second.message["result"]["structuredContent"]["name"], "octocat") << second.message;
}

TEST_F(HttpMrtrTest, TokenRefreshBetweenRoundsStillResumesWhenPrincipalIdReturned) {
    constexpr unsigned short g_port = 20601;
    mcp::StreamableHttpSessionManager manager(io_ctx_.get_executor(), "127.0.0.1", g_port, factory());
    manager.set_bearer_token_validator([](std::string_view token) -> mcp::TokenValidation {
        return {token == "alice-old" || token == "alice-new", "alice"};
    });
    Reply first;
    Reply second;
    run(manager, [&]() -> mcp::Task<void> {
        const auto request = call_tool(1, "ask");
        first = co_await send(io_ctx_.get_executor(), g_port, request.dump(),
                              call_headers("ask", "Bearer alice-old"));
        second = co_await send(io_ctx_.get_executor(), g_port,
                               retry_of(request, 2, first.message, "alice").dump(),
                               call_headers("ask", "Bearer alice-new"));
    });

    ASSERT_EQ(first.message["result"]["resultType"], "input_required") << first.message;
    EXPECT_EQ(second.status, 200u) << second.message;
    EXPECT_EQ(second.message["result"]["structuredContent"]["name"], "alice") << second.message;
}

TEST_F(HttpMrtrTest, TokenRefreshWithoutPrincipalIdFailsAsUnknown) {
    constexpr unsigned short g_port = 20602;
    mcp::StreamableHttpSessionManager manager(io_ctx_.get_executor(), "127.0.0.1", g_port, factory());
    manager.set_bearer_token_validator([](std::string_view /*token*/) { return true; });
    Reply first;
    Reply second;
    run(manager, [&]() -> mcp::Task<void> {
        const auto request = call_tool(1, "ask");
        first = co_await send(io_ctx_.get_executor(), g_port, request.dump(),
                              call_headers("ask", "Bearer alice-old"));
        second = co_await send(io_ctx_.get_executor(), g_port,
                               retry_of(request, 2, first.message, "alice").dump(),
                               call_headers("ask", "Bearer alice-new"));
    });

    ASSERT_EQ(first.message["result"]["resultType"], "input_required") << first.message;
    // Without a principal id the request is bound to its Authorization header, which changed.
    EXPECT_EQ(second.status, 400u) << second.message;
    EXPECT_EQ(second.message["error"]["code"], mcp::g_INVALID_PARAMS) << second.message;
    EXPECT_EQ(second.message["error"]["message"], "Unknown or expired requestState") << second.message;
}

TEST_F(HttpMrtrTest, UndeclaredCapability400With32021) {
    constexpr unsigned short g_port = 20603;
    mcp::StreamableHttpSessionManager manager(io_ctx_.get_executor(), "127.0.0.1", g_port, factory());
    Reply reply;
    run(manager, [&]() -> mcp::Task<void> {
        auto request = call_tool(1, "ask");
        request["params"]["_meta"]["io.modelcontextprotocol/clientCapabilities"] = json::object();
        reply = co_await send(io_ctx_.get_executor(), g_port, request.dump(),
                              call_headers("ask", std::nullopt));
    });

    EXPECT_EQ(reply.status, 400u) << reply.message;
    EXPECT_EQ(reply.message["error"]["code"], mcp::g_MISSING_REQUIRED_CLIENT_CAPABILITY)
        << reply.message;
    EXPECT_EQ(reply.message["error"]["data"]["requiredCapabilities"],
              (json{{"elicitation", {{"form", json::object()}}}}))
        << reply.message;
}

TEST_F(HttpMrtrTest, DisconnectOfTheRetryCancelsTheResumedHandler) {
    constexpr unsigned short g_port = 20604;
    mcp::StreamableHttpSessionManager manager(io_ctx_.get_executor(), "127.0.0.1", g_port, factory());
    Reply first;
    bool resumed = false;
    bool cancelled = false;
    run(manager, [&]() -> mcp::Task<void> {
        const auto request = call_tool(1, "ask_then_wait");
        first = co_await send(io_ctx_.get_executor(), g_port, request.dump(),
                              call_headers("ask_then_wait", std::nullopt));

        // The retry is written and its connection dropped while the handler still runs.
        auto executor = io_ctx_.get_executor();
        beast::tcp_stream stream(executor);
        asio::ip::tcp::resolver resolver(executor);
        auto endpoints =
            co_await resolver.async_resolve("127.0.0.1", std::to_string(g_port), asio::use_awaitable);
        co_await stream.async_connect(*endpoints.begin(), asio::use_awaitable);
        auto retry = make_post(retry_of(request, 2, first.message, "x").dump(),
                               call_headers("ask_then_wait", std::nullopt));
        co_await http::async_write(stream, retry, asio::use_awaitable);
        resumed = co_await wait_until([this] { return resumed_->load(); });
        beast::error_code ignored;
        stream.socket().shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
        stream.socket().close(ignored);
        cancelled = co_await wait_until([this] { return cancelled_->load(); });
    });

    ASSERT_EQ(first.message["result"]["resultType"], "input_required") << first.message;
    EXPECT_TRUE(resumed);
    EXPECT_TRUE(cancelled);
}

TEST_F(HttpMrtrTest, CloseGivesUpWaitingHandlers) {
    constexpr unsigned short g_port = 20605;
    mcp::StreamableHttpSessionManager manager(io_ctx_.get_executor(), "127.0.0.1", g_port, factory());
    Reply first;
    std::uint64_t parked_before_close = 0;
    run(manager, [&]() -> mcp::Task<void> {
        first = co_await send(io_ctx_.get_executor(), g_port, call_tool(1, "ask").dump(),
                              call_headers("ask", std::nullopt));
        parked_before_close = manager.modern_server()->mrtr_stats().parked;
    });

    // run() returned, so no waiting handler kept the io_context busy until its expiry.
    ASSERT_EQ(first.message["result"]["resultType"], "input_required") << first.message;
    EXPECT_EQ(parked_before_close, 1u);
    EXPECT_EQ(manager.modern_server()->mrtr_stats().parked, 0u);
}

}  // namespace

#include "../test_utils.hpp"

#include "mcp/server/server.hpp"
#include "mcp/transport/http_server.hpp"
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
#include <memory>
#include <optional>
#include <string>
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
    std::string body;
    std::optional<std::string> session_id;
};

mcp::Task<Reply> send(asio::any_io_executor executor, unsigned short port, std::string body,
                      Headers headers) {
    beast::tcp_stream stream(executor);
    co_await stream.async_connect(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), port),
                                  asio::use_awaitable);
    http::request<http::string_body> request{http::verb::post, "/mcp", 11};
    request.set(http::field::host, "127.0.0.1");
    request.set(http::field::content_type, "application/json");
    request.set(http::field::accept, "application/json, text/event-stream");
    for (const auto& [name, value] : headers) {
        request.insert(name, value);
    }
    request.keep_alive(false);
    request.body() = std::move(body);
    request.prepare_payload();
    co_await http::async_write(stream, request, asio::use_awaitable);
    beast::flat_buffer buffer;
    http::response<http::string_body> response;
    co_await http::async_read(stream, buffer, response, asio::use_awaitable);
    Reply reply;
    reply.status = response.result_int();
    reply.body = response.body();
    if (auto session = response.find("Mcp-Session-Id"); session != response.end()) {
        reply.session_id = std::string(session->value());
    }
    co_return reply;
}

template <typename Condition>
mcp::Task<bool> wait_until(Condition condition) {
    auto executor = co_await asio::this_coro::executor;
    const auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!condition() && std::chrono::steady_clock::now() < give_up) {
        asio::steady_timer timer(executor, std::chrono::milliseconds(1));
        co_await timer.async_wait(asio::use_awaitable);
    }
    co_return condition();
}

mcp::Task<void> pause(std::chrono::milliseconds time) {
    asio::steady_timer timer(co_await asio::this_coro::executor, time);
    co_await timer.async_wait(asio::use_awaitable);
}

std::unique_ptr<mcp::Server> make_server() {
    mcp::ServerCapabilities caps;
    caps.tools = mcp::ServerCapabilities::ToolsCapability{};
    return std::make_unique<mcp::Server>(mcp::Implementation{"limits", "1.0"}, caps);
}

unsigned short free_port() {
    asio::io_context io;
    asio::ip::tcp::acceptor acceptor(io,
                                     asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    return acceptor.local_endpoint().port();
}

class SessionLimitsTest : public ::testing::Test {
   protected:
    asio::io_context io_;
    unsigned short port_{free_port()};
    mcp::StreamableHttpSessionManager manager_{
        io_.get_executor(), "127.0.0.1", port_,
        [](const asio::any_io_executor&) { return make_server(); }};

    template <typename Exchange>
    void run(Exchange exchange) {
        asio::co_spawn(io_, manager_.listen(), asio::detached);
        asio::co_spawn(
            io_,
            [this, exchange = std::move(exchange)]() mutable -> mcp::Task<void> {
                co_await exchange();
                manager_.close();
            },
            asio::detached);
        io_.run();
    }

    mcp::Task<Reply> post(json body, Headers headers = {}) {
        return send(io_.get_executor(), port_, body.dump(), std::move(headers));
    }

    mcp::Task<void> post_only(json body, Headers headers = {}) {
        const auto reply = co_await post(std::move(body), std::move(headers));
        static_cast<void>(reply);
    }
};

Headers session_header(const Reply& initialized) {
    return {{"Mcp-Session-Id", initialized.session_id.value_or("")}};
}

Headers established_headers(const Reply& initialized) {
    return {{"MCP-Session-Id", initialized.session_id.value_or("")},
            {"MCP-Protocol-Version", json::parse(initialized.body)["result"]["protocolVersion"]}};
}

json bad_initialize() { return {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"}}; }
json initialize_notification() { return {{"jsonrpc", "2.0"}, {"method", "initialize"}}; }
json ping() { return {{"jsonrpc", "2.0"}, {"id", 2}, {"method", "ping"}}; }

}  // namespace

TEST_F(SessionLimitsTest, InitializePastTheLimitIsRefused) {
    manager_.set_session_limits({2, std::chrono::milliseconds::zero()});
    Reply third;
    run([&]() -> mcp::Task<void> {
        co_await post_only(make_initialize_request("1"));
        co_await post_only(make_initialize_request("2"));
        third = co_await post(make_initialize_request("3"));
    });
    EXPECT_EQ(third.status, 503u);
    EXPECT_FALSE(third.session_id.has_value());
}

TEST_F(SessionLimitsTest, InitializeThatFailsKeepsNoSession) {
    std::size_t after_bad_params = 1;
    std::size_t after_notification = 1;
    Reply notification;
    run([&]() -> mcp::Task<void> {
        co_await post_only(bad_initialize());
        after_bad_params = manager_.session_count();
        notification = co_await post(initialize_notification());
        after_notification = manager_.session_count();
    });
    EXPECT_EQ(after_bad_params, 0u);
    EXPECT_EQ(after_notification, 0u);
    EXPECT_EQ(notification.status, 400u);
}

TEST_F(SessionLimitsTest, IdleSessionEnds) {
    manager_.set_session_limits({10, std::chrono::milliseconds(40)});
    std::size_t before = 0;
    bool ended = false;
    Reply later;
    run([&]() -> mcp::Task<void> {
        const auto first = co_await post(make_initialize_request("1"));
        before = manager_.session_count();
        ended = co_await wait_until([this] { return manager_.session_count() == 0; });
        later = co_await post(ping(), session_header(first));
    });
    EXPECT_EQ(before, 1u);
    EXPECT_TRUE(ended);
    EXPECT_EQ(later.status, 404u);
}

TEST_F(SessionLimitsTest, UsedSessionStays) {
    manager_.set_session_limits({10, std::chrono::milliseconds(2000)});
    std::size_t after_use = 0;
    run([&]() -> mcp::Task<void> {
        const auto first = co_await post(make_initialize_request("1"));
        const auto session = session_header(first);
        co_await post_only(ping(), session);
        after_use = manager_.session_count();
    });
    EXPECT_EQ(after_use, 1u);
}

TEST_F(SessionLimitsTest, SessionLimitMustAllowASession) {
    EXPECT_THROW(manager_.set_session_limits({0, std::chrono::milliseconds::zero()}),
                 std::invalid_argument);
}

TEST(EventStoreLimits, OldestEventsGoPastTheByteLimit) {
    mcp::EventStore store;
    const std::string event(mcp::constants::g_event_store_max_bytes / 2, 'x');
    const auto first = store.append(event);
    const auto second = store.append(event);
    EXPECT_EQ(store.events_after(first)->size(), 1u);
    const auto third = store.append(event);
    EXPECT_FALSE(store.events_after(first).has_value());
    EXPECT_EQ(store.events_after(second)->size(), 1u);
    // The newest is kept whatever its size.
    const auto last = store.append(std::string(mcp::constants::g_event_store_max_bytes * 2, 'y'));
    EXPECT_FALSE(store.events_after(third).has_value());
    EXPECT_TRUE(store.events_after(last).has_value());
}

namespace {

class ReverseBindingTest : public ::testing::Test {
   protected:
    asio::io_context io_;
    std::shared_ptr<mcp::HttpServerTransport> transport_ =
        std::make_shared<mcp::HttpServerTransport>(io_.get_executor(), "127.0.0.1", 0);
    std::unique_ptr<mcp::Server> server_ = make_server();
    std::atomic<bool> handler_started_{false};
    std::optional<std::string> handler_saw_;

    void SetUp() override {
        server_->add_tool<json, json>("ask", "Asks the client", json{{"type", "object"}},
                                      [this](json /*args*/) -> mcp::Task<json> {
                                          handler_started_.store(true);
                                          try {
                                              auto answer = co_await server_->send_request(
                                                  "roots/list", std::nullopt);
                                              handler_saw_ = answer.dump();
                                          } catch (const std::exception& error) {
                                              handler_saw_ = std::string("failed: ") + error.what();
                                          }
                                          co_return json::object();
                                      });
        server_->set_reverse_request_timeout(std::chrono::milliseconds(300));
        transport_->set_json_only(true);
    }

    template <typename Exchange>
    void run(Exchange exchange) {
        asio::co_spawn(io_, transport_->listen(), asio::detached);
        asio::co_spawn(
            io_, [this]() -> mcp::Task<void> { co_await server_->run(transport_, io_.get_executor()); },
            asio::detached);
        asio::co_spawn(
            io_,
            [this, exchange = std::move(exchange)]() mutable -> mcp::Task<void> {
                co_await exchange();
                transport_->close();
            },
            asio::detached);
        io_.run();
    }

    mcp::Task<Reply> post(json body, Headers headers = {}) {
        return send(io_.get_executor(), transport_->port(), body.dump(), std::move(headers));
    }

    mcp::Task<void> post_only(json body, Headers headers = {}) {
        const auto reply = co_await post(std::move(body), std::move(headers));
        static_cast<void>(reply);
    }

    mcp::Task<void> end_session(std::string session_id) {
        beast::tcp_stream stream(io_.get_executor());
        co_await stream.async_connect(
            asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), transport_->port()),
            asio::use_awaitable);
        http::request<http::string_body> request{http::verb::delete_, "/mcp", 11};
        request.set(http::field::host, "127.0.0.1");
        request.set("MCP-Session-Id", session_id);
        request.prepare_payload();
        co_await http::async_write(stream, request, asio::use_awaitable);
        beast::flat_buffer buffer;
        http::response<http::string_body> response;
        co_await http::async_read(stream, buffer, response, asio::use_awaitable);
    }

    /// A client that initializes and starts the tool, then leaves it waiting for its answer.
    mcp::Task<std::string> session_waiting_for_an_answer() {
        const auto initialized = co_await post(make_initialize_request("i"));
        const auto id = initialized.session_id.value_or("");
        const Headers session{
            {"MCP-Session-Id", id},
            {"MCP-Protocol-Version", json::parse(initialized.body)["result"]["protocolVersion"]}};
        co_await post_only(make_initialized_notification(), session);
        asio::co_spawn(
            io_,
            [this, session]() -> mcp::Task<void> {
                try {
                    co_await post_only(make_tool_call_request("c", "ask", json::object()), session);
                } catch (const std::exception&) {
                    // The session may end first.
                }
            },
            asio::detached);
        co_await wait_until([this] { return handler_started_.load(); });
        co_await pause(std::chrono::milliseconds(20));
        co_return id;
    }

    static json answer(int id) {
        return {{"jsonrpc", "2.0"},
                {"id", std::to_string(id)},
                {"result", {{"roots", json::array({{{"uri", "file:///injected"}}})}}}};
    }
};

}  // namespace

// Reverse request ids are small sequential numbers, so anyone could guess the one waiting.
TEST_F(ReverseBindingTest, StrangerCannotAnswerAfterTheSessionEnded) {
    Reply stranger;
    run([&]() -> mcp::Task<void> {
        const auto id = co_await session_waiting_for_an_answer();
        co_await end_session(id);
        for (int guess = 1; guess <= 3; ++guess) {
            stranger = co_await post(answer(guess));
        }
        co_await wait_until([this] { return handler_saw_.has_value(); });
    });
    EXPECT_EQ(stranger.status, 400u);
    ASSERT_TRUE(handler_saw_.has_value());
    EXPECT_EQ(handler_saw_->find("injected"), std::string::npos) << *handler_saw_;
}

TEST_F(ReverseBindingTest, NextSessionCannotAnswerForTheOneBefore) {
    Reply next_client;
    run([&]() -> mcp::Task<void> {
        const auto id = co_await session_waiting_for_an_answer();
        co_await end_session(id);
        const auto initialized = co_await post(make_initialize_request("next"));
        const Headers session{
            {"MCP-Session-Id", initialized.session_id.value_or("")},
            {"MCP-Protocol-Version", json::parse(initialized.body)["result"]["protocolVersion"]}};
        for (int guess = 1; guess <= 3; ++guess) {
            next_client = co_await post(answer(guess), session);
        }
        co_await wait_until([this] { return handler_saw_.has_value(); });
    });
    ASSERT_TRUE(handler_saw_.has_value());
    EXPECT_EQ(handler_saw_->find("injected"), std::string::npos) << *handler_saw_;
}

TEST_F(ReverseBindingTest, ClientThatLeftIdleLetsTheNextOneIn) {
    transport_->set_session_idle_timeout(std::chrono::milliseconds(20));
    Reply first;
    Reply second;
    run([&]() -> mcp::Task<void> {
        first = co_await post(make_initialize_request("first"));
        co_await pause(std::chrono::milliseconds(40));
        second = co_await post(make_initialize_request("second"));
    });
    EXPECT_EQ(first.status, 200u);
    EXPECT_EQ(second.status, 200u) << second.body;
    EXPECT_TRUE(second.session_id.has_value());
    EXPECT_NE(second.session_id, first.session_id);
}

TEST_F(ReverseBindingTest, ActiveSessionKeepsNextClientOut) {
    transport_->set_session_idle_timeout(std::chrono::hours(1));
    Reply second;
    run([&]() -> mcp::Task<void> {
        co_await post_only(make_initialize_request("first"));
        second = co_await post(make_initialize_request("second"));
    });
    EXPECT_EQ(second.status, 400u);
}

#include "../test_utils.hpp"

#include "mcp/server/server.hpp"
#include "mcp/transport/http_server.hpp"
#include "mcp/transport/http_session_manager.hpp"

#include <gtest/gtest.h>

#include <array>
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
    // A bound for reporting a server that never answers, not a measure of speed.
    stream.expires_after(std::chrono::minutes(2));
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

/// Writes @p text on a new connection and returns what comes back until the server closes it, or
/// what came before @p wait ran out.
mcp::Task<std::string> raw_exchange(asio::any_io_executor executor, unsigned short port,
                                    std::string text, std::chrono::milliseconds wait) {
    beast::tcp_stream stream(executor);
    stream.expires_after(wait);
    std::string received;
    try {
        co_await stream.async_connect(
            asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), port), asio::use_awaitable);
        if (!text.empty()) {
            co_await asio::async_write(stream, asio::buffer(text), asio::use_awaitable);
        }
        std::array<char, 4096> chunk{};
        for (;;) {
            const auto count =
                co_await stream.async_read_some(asio::buffer(chunk), asio::use_awaitable);
            received.append(chunk.data(), count);
        }
    } catch (const boost::system::system_error& error) {
        if (error.code() == beast::error::timeout) {
            received += "[timed out]";
        }
    }
    co_return received;
}

/// A POST whose headers promise @p body_bytes of body, none of which is sent.
std::string header_only_post(std::size_t body_bytes, const std::string& extra = "") {
    return "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\n"
           "Accept: application/json, text/event-stream\r\n" +
           extra + "Content-Length: " + std::to_string(body_bytes) + "\r\n\r\n";
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
                // A failed exchange still closes the server, or the test would never return.
                try {
                    co_await exchange();
                } catch (const std::exception& error) {
                    ADD_FAILURE() << error.what();
                }
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

// Requests keep a session alive past the idle timeout, which counts from the last one.
TEST_F(SessionLimitsTest, UsedSessionStays) {
    constexpr auto idle = std::chrono::milliseconds(300);
    manager_.set_session_limits({10, idle});
    std::size_t after_use = 0;
    run([&]() -> mcp::Task<void> {
        const auto first = co_await post(make_initialize_request("1"));
        const auto session = session_header(first);
        for (int i = 0; i < 8; ++i) {
            co_await pause(idle / 3);
            co_await post_only(ping(), session);
        }
        after_use = manager_.session_count();
    });
    EXPECT_EQ(after_use, 1u);
}

// An id the server could not echo unchanged would leave the request waiting for good, and the
// session busy with it.
TEST_F(SessionLimitsTest, RequestIdBeyondInt64IsRefused) {
    manager_.set_session_limits({10, std::chrono::milliseconds(40)});
    Reply refused;
    bool ended = false;
    run([&]() -> mcp::Task<void> {
        const auto first = co_await post(make_initialize_request("1"));
        refused =
            co_await post(json::parse(R"({"jsonrpc":"2.0","id":18446744073709551615,"method":"ping"})"),
                          session_header(first));
        ended = co_await wait_until([this] { return manager_.session_count() == 0; });
    });
    EXPECT_EQ(refused.status, 400u);
    EXPECT_EQ(json::parse(refused.body)["error"]["code"], mcp::g_INVALID_REQUEST);
    EXPECT_TRUE(ended);
}

// A request without a valid token is refused on its headers: its body is never waited for.
TEST_F(SessionLimitsTest, UnauthorizedBodyIsNotRead) {
    manager_.set_bearer_token_validator([](std::string_view token) { return token == "good"; });
    std::string answer;
    run([&]() -> mcp::Task<void> {
        answer = co_await raw_exchange(io_.get_executor(), port_, header_only_post(4 * 1024 * 1024),
                                       std::chrono::seconds(20));
    });
    EXPECT_EQ(answer.rfind("HTTP/1.1 401", 0), 0u) << answer;
    EXPECT_EQ(answer.find("[timed out]"), std::string::npos) << answer;
}

TEST_F(SessionLimitsTest, ConnectionThatSendsNothingIsClosed) {
    manager_.set_connection_limits({0, std::chrono::milliseconds(100), 4096, 1U << 30});
    std::string answer;
    run([&]() -> mcp::Task<void> {
        answer = co_await raw_exchange(io_.get_executor(), port_, "", std::chrono::seconds(20));
    });
    EXPECT_EQ(answer, "");
}

TEST_F(SessionLimitsTest, ConnectionsPastTheLimitAreClosed) {
    manager_.set_connection_limits({2, std::chrono::seconds(30), 4096, 1U << 30});
    std::string third;
    Reply after;
    run([&]() -> mcp::Task<void> {
        auto executor = io_.get_executor();
        auto idle_one = std::make_shared<beast::tcp_stream>(executor);
        auto idle_two = std::make_shared<beast::tcp_stream>(executor);
        const asio::ip::tcp::endpoint endpoint(asio::ip::make_address("127.0.0.1"), port_);
        co_await idle_one->async_connect(endpoint, asio::use_awaitable);
        co_await idle_two->async_connect(endpoint, asio::use_awaitable);
        co_await pause(std::chrono::milliseconds(50));
        third = co_await raw_exchange(executor, port_, header_only_post(2), std::chrono::seconds(20));
        idle_one->close();
        // The server frees the place once it has seen the connection end.
        for (int attempt = 0; attempt < 3000 && after.status != 200; ++attempt) {
            bool refused = false;
            try {
                after = co_await post(make_initialize_request("1"));
            } catch (const std::exception&) {
                refused = true;
            }
            if (refused) {
                co_await pause(std::chrono::milliseconds(10));
            }
        }
        idle_two->close();
    });
    EXPECT_EQ(third, "");
    EXPECT_EQ(after.status, 200u);
}

TEST_F(SessionLimitsTest, RequestsPastTheBudgetAreRefused) {
    manager_.set_connection_limits({0, std::chrono::seconds(30), 4096, 64 * 1024});
    Reply large;
    Reply small;
    run([&]() -> mcp::Task<void> {
        auto request = make_initialize_request("1");
        request["params"]["padding"] = std::string(100 * 1024, 'x');
        large = co_await post(request);
        small = co_await post(make_initialize_request("2"));
    });
    EXPECT_EQ(large.status, 503u) << large.body;
    EXPECT_EQ(small.status, 200u) << small.body;
}

TEST_F(SessionLimitsTest, ConnectionLimitsMustAllowARequest) {
    EXPECT_THROW(manager_.set_connection_limits({0, std::chrono::seconds(1), 0, 1}),
                 std::invalid_argument);
    EXPECT_THROW(manager_.set_connection_limits({0, std::chrono::seconds(1), 1, 0}),
                 std::invalid_argument);
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
                try {
                    co_await exchange();
                } catch (const std::exception& error) {
                    ADD_FAILURE() << error.what();
                }
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

TEST_F(ReverseBindingTest, RequestIdBeyondInt64IsRefused) {
    Reply refused;
    run([&]() -> mcp::Task<void> {
        const auto initialized = co_await post(make_initialize_request("i"));
        const Headers session{
            {"MCP-Session-Id", initialized.session_id.value_or("")},
            {"MCP-Protocol-Version", json::parse(initialized.body)["result"]["protocolVersion"]}};
        refused = co_await post(
            json::parse(R"({"jsonrpc":"2.0","id":9223372036854775808,"method":"ping"})"), session);
    });
    EXPECT_EQ(refused.status, 400u);
    EXPECT_EQ(json::parse(refused.body)["error"]["code"], mcp::g_INVALID_REQUEST);
}

// Subscriptions of a session that ended are not the next client's: they must not use up its limit.
TEST_F(ReverseBindingTest, SubscriptionsOfAnEndedSessionLeaveRoomForTheNext) {
    mcp::ServerCapabilities caps;
    caps.resources = mcp::ServerCapabilities::ResourcesCapability{};
    caps.resources->subscribe = true;
    server_ = std::make_unique<mcp::Server>(mcp::Implementation{"limits", "1.0"}, caps);
    const auto subscribe = [](int id) {
        return json{{"jsonrpc", "2.0"},
                    {"id", id},
                    {"method", "resources/subscribe"},
                    {"params", {{"uri", "mem://" + std::to_string(id) + std::string(1048576, 'u')}}}};
    };
    const auto open_session = [this](std::string id) -> mcp::Task<Headers> {
        const auto initialized = co_await post(make_initialize_request(std::move(id)));
        const Headers session{
            {"MCP-Session-Id", initialized.session_id.value_or("")},
            {"MCP-Protocol-Version", json::parse(initialized.body)["result"]["protocolVersion"]}};
        co_await post_only(make_initialized_notification(), session);
        co_return session;
    };
    Reply full;
    Reply next;
    run([&]() -> mcp::Task<void> {
        const auto first = co_await open_session("a");
        for (int id = 1; id <= 8; ++id) {
            full = co_await post(subscribe(id), first);
        }
        co_await end_session(first.front().second);
        const auto second = co_await open_session("b");
        next = co_await post(subscribe(100), second);
    });
    EXPECT_NE(full.body.find("Too many resource subscriptions"), std::string::npos) << full.body;
    EXPECT_NE(json::parse(next.body).value("result", json()), json()) << next.body;
}

TEST_F(ReverseBindingTest, UnauthorizedBodyIsNotRead) {
    transport_->set_bearer_token_validator([](std::string_view token) { return token == "good"; });
    std::string answer;
    run([&]() -> mcp::Task<void> {
        answer = co_await raw_exchange(io_.get_executor(), transport_->port(),
                                       header_only_post(4 * 1024 * 1024), std::chrono::seconds(20));
    });
    EXPECT_EQ(answer.rfind("HTTP/1.1 401", 0), 0u) << answer;
}

TEST_F(ReverseBindingTest, ConnectionsPastTheLimitAreClosed) {
    transport_->set_connection_limits({1, std::chrono::seconds(30), 4096, 1U << 30});
    std::string second;
    run([&]() -> mcp::Task<void> {
        auto idle = std::make_shared<beast::tcp_stream>(io_.get_executor());
        co_await idle->async_connect(
            asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), transport_->port()),
            asio::use_awaitable);
        co_await pause(std::chrono::milliseconds(50));
        second = co_await raw_exchange(io_.get_executor(), transport_->port(), header_only_post(2),
                                       std::chrono::seconds(20));
        idle->close();
    });
    EXPECT_EQ(second, "");
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

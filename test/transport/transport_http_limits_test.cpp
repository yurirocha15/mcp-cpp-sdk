#include "../test_utils.hpp"

#include "mcp/client/client.hpp"
#include "mcp/server/server.hpp"
#include "mcp/transport/http_client.hpp"
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
#include <boost/asio/write.hpp>
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
                      Headers headers, http::verb verb = http::verb::post) {
    beast::tcp_stream stream(executor);
    // A bound for reporting a server that never answers, not a measure of speed.
    stream.expires_after(std::chrono::minutes(2));
    co_await stream.async_connect(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), port),
                                  asio::use_awaitable);
    http::request<http::string_body> request{verb, "/mcp", 11};
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

/// Set by the "hold" tool when it starts; it answers once g_hold_released is set.
std::atomic<bool> g_hold_started{false};
std::atomic<bool> g_hold_released{false};

std::unique_ptr<mcp::Server> make_server() {
    mcp::ServerCapabilities caps;
    caps.tools = mcp::ServerCapabilities::ToolsCapability{};
    auto server = std::make_unique<mcp::Server>(mcp::Implementation{"limits", "1.0"}, caps);
    server->add_tool<json, json>("hold", "Answers once released", json{{"type", "object"}},
                                 [](json /*args*/) -> mcp::Task<json> {
                                     g_hold_started.store(true);
                                     auto executor = co_await asio::this_coro::executor;
                                     while (!g_hold_released.load()) {
                                         asio::steady_timer tick(executor,
                                                                 std::chrono::milliseconds(1));
                                         co_await tick.async_wait(asio::use_awaitable);
                                     }
                                     co_return json::object();
                                 });
    server->add_tool<json, json>(
        "progress", "Reports progress, then answers", json{{"type", "object"}},
        [](mcp::Context& ctx, json args) -> mcp::Task<json> {
            co_await ctx.report_progress(1);
            co_return json{{"pad", std::string(args.value("pad", std::size_t{0}), 'r')}};
        });
    return server;
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
// A session the server ended is forgotten; the client starts a new one and its call goes through.
TEST_F(SessionLimitsTest, ClientStartsANewSessionWhenTheOldOneExpired) {
    manager_.set_session_limits({10, std::chrono::milliseconds(40)});
    bool answered = false;
    std::string first_session;
    std::string second_session;
    run([&]() -> mcp::Task<void> {
        auto transport = std::make_shared<mcp::HttpClientTransport>(
            io_.get_executor(), "http://127.0.0.1:" + std::to_string(port_) + "/mcp");
        mcp::Client client(transport, io_.get_executor());
        co_await client.connect("expired", "1.0");
        first_session = transport->session_id();
        co_await wait_until([this] { return manager_.session_count() == 0; });
        co_await client.ping();
        answered = true;
        second_session = transport->session_id();
        client.close();
    });
    EXPECT_TRUE(answered);
    EXPECT_FALSE(second_session.empty());
    EXPECT_NE(first_session, second_session);
}

// A new session that could not start is started again before the client's next request.
TEST_F(SessionLimitsTest, ClientStartsANewSessionAfterAFailedStart) {
    manager_.set_session_limits({1, std::chrono::minutes(5)});
    bool refused = false;
    bool answered = false;
    run([&]() -> mcp::Task<void> {
        auto transport = std::make_shared<mcp::HttpClientTransport>(
            io_.get_executor(), "http://127.0.0.1:" + std::to_string(port_) + "/mcp");
        mcp::Client client(transport, io_.get_executor());
        co_await client.connect("retry", "1.0");
        // Named: GCC mishandles a temporary in a co_await operand.
        const Headers own{{"Mcp-Session-Id", transport->session_id()}};
        const auto ended = co_await send(io_.get_executor(), port_, "", own, http::verb::delete_);
        EXPECT_EQ(ended.status, 200u);
        // Another client takes the only session, so the new one this client starts is refused.
        const auto other = co_await post(make_initialize_request("2"));
        try {
            co_await client.ping();
        } catch (const std::exception&) {
            refused = true;
        }
        const auto others = session_header(other);
        const auto freed = co_await send(io_.get_executor(), port_, "", others, http::verb::delete_);
        EXPECT_EQ(freed.status, 200u);
        co_await client.ping();
        answered = true;
        client.close();
    });
    EXPECT_TRUE(refused);
    EXPECT_TRUE(answered);
}

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

// A session whose request is still being answered is in use, however long the answer takes: only
// the session that is idle ends.
TEST_F(SessionLimitsTest, SessionWithARequestWaitingIsNotIdle) {
    g_hold_started = false;
    g_hold_released = false;
    manager_.set_session_limits({10, std::chrono::milliseconds(40)});
    Reply held;
    bool idle_one_ended = false;
    std::size_t left = 0;
    run([&]() -> mcp::Task<void> {
        const auto busy = co_await post(make_initialize_request("busy"));
        const auto session = session_header(busy);
        co_await post_only(make_initialized_notification(), session);
        asio::co_spawn(
            io_,
            [&, session]() -> mcp::Task<void> {
                held = co_await post(make_tool_call_request("h", "hold", json::object()), session);
            },
            asio::detached);
        co_await wait_until([] { return g_hold_started.load(); });
        co_await post_only(make_initialize_request("idle"));
        idle_one_ended = co_await wait_until([this] { return manager_.session_count() < 2; });
        left = manager_.session_count();
        g_hold_released = true;
        co_await wait_until([&] { return held.status != 0; });
    });
    EXPECT_TRUE(idle_one_ended);
    EXPECT_EQ(left, 1u);
    EXPECT_EQ(held.status, 200u);
    EXPECT_NE(held.body.find("\"result\""), std::string::npos) << held.body;
}

// A client that resumes a stream with nothing after its last event is answered with an empty
// stream, not refused.
TEST_F(SessionLimitsTest, ResumeWithNothingToReplayIsAnEmptyStream) {
    std::string resumed;
    std::string event_id;
    run([&]() -> mcp::Task<void> {
        const auto initialized = co_await post(make_initialize_request("1"));
        const auto session = session_header(initialized);
        co_await post_only(make_initialized_notification(), session);
        const auto pinged = co_await post(ping(), session);
        const auto id_at = pinged.body.find("id: ");
        if (id_at == std::string::npos) {
            ADD_FAILURE() << "the answer carries no event id: " << pinged.body;
            co_return;
        }
        event_id = pinged.body.substr(id_at + 4, pinged.body.find('\n', id_at) - id_at - 4);
        resumed = co_await raw_exchange(
            io_.get_executor(), port_,
            "GET /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\nAccept: text/event-stream\r\nMCP-Session-Id: " +
                initialized.session_id.value_or("") + "\r\nLast-Event-ID: " + event_id +
                "\r\nConnection: close\r\n\r\n",
            std::chrono::seconds(20));
    });
    EXPECT_EQ(resumed.rfind("HTTP/1.1 200", 0), 0u) << resumed;
    EXPECT_NE(resumed.find("text/event-stream"), std::string::npos) << resumed;
}

// A resume replays the later events of the stream it names, and nothing another POST's stream
// carried.
TEST_F(SessionLimitsTest, ResumeReplaysOnlyTheStreamItNames) {
    std::string progressed;
    std::string resumed;
    run([&]() -> mcp::Task<void> {
        const auto initialized = co_await post(make_initialize_request("1"));
        const auto session = session_header(initialized);
        co_await post_only(make_initialized_notification(), session);
        auto call = make_tool_call_request("p", "progress", json::object());
        call["params"]["_meta"] = {{"progressToken", "t"}};
        progressed = (co_await post(call, session)).body;
        co_await post(ping(), session);
        const auto id_at = progressed.find("id: ");
        if (id_at == std::string::npos) {
            ADD_FAILURE() << "the answer carries no event id: " << progressed;
            co_return;
        }
        const auto event_id = progressed.substr(id_at + 4, progressed.find('\n', id_at) - id_at - 4);
        resumed = co_await raw_exchange(
            io_.get_executor(), port_,
            "GET /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\nAccept: text/event-stream\r\nMCP-Session-Id: " +
                initialized.session_id.value_or("") + "\r\nLast-Event-ID: " + event_id +
                "\r\nConnection: close\r\n\r\n",
            std::chrono::seconds(20));
    });
    ASSERT_NE(progressed.find("notifications/progress"), std::string::npos) << progressed;
    EXPECT_EQ(resumed.rfind("HTTP/1.1 200", 0), 0U) << resumed;
    EXPECT_NE(resumed.find(R"("id":"p")"), std::string::npos) << resumed;
    EXPECT_EQ(resumed.find(R"("id":2)"), std::string::npos) << resumed;
}

// A replay holds its own copy of the events until it is read, so it counts against what requests may
// hold; slow readers cannot each take a copy of the whole store.
TEST_F(SessionLimitsTest, ReplayCountsAgainstTheRequestBudget) {
    manager_.set_connection_limits({0, std::chrono::seconds(30), 4096, 128 * 1024});
    std::string resumed;
    run([&]() -> mcp::Task<void> {
        const auto initialized = co_await post(make_initialize_request("1"));
        const auto session = session_header(initialized);
        co_await post_only(make_initialized_notification(), session);
        auto call = make_tool_call_request("p", "progress", json{{"pad", 100000}});
        call["params"]["_meta"] = {{"progressToken", "t"}};
        const auto progressed = (co_await post(call, session)).body;
        const auto id_at = progressed.find("id: ");
        if (id_at == std::string::npos) {
            ADD_FAILURE() << "the answer carries no event id: " << progressed.substr(0, 200);
            co_return;
        }
        const auto event_id = progressed.substr(id_at + 4, progressed.find('\n', id_at) - id_at - 4);
        resumed = co_await raw_exchange(
            io_.get_executor(), port_,
            "GET /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\nAccept: text/event-stream\r\nMCP-Session-Id: " +
                initialized.session_id.value_or("") + "\r\nLast-Event-ID: " + event_id +
                "\r\nConnection: close\r\n\r\n",
            std::chrono::seconds(20));
    });
    EXPECT_EQ(resumed.rfind("HTTP/1.1 503", 0), 0U) << resumed.substr(0, 200);
}

// A body counts by what has arrived, so headers that announce a large body hold nothing, and the
// requests of other clients are still served.
TEST_F(SessionLimitsTest, AnnouncedBodyHoldsNothingUntilItArrives) {
    manager_.set_connection_limits({0, std::chrono::seconds(30), 4096, 64 * 1024});
    Reply initialized;
    run([&]() -> mcp::Task<void> {
        asio::ip::tcp::socket holder(io_);
        co_await holder.async_connect({asio::ip::make_address("127.0.0.1"), port_},
                                      asio::use_awaitable);
        const auto announced = header_only_post(64 * 1024 - 16);
        co_await asio::async_write(holder, asio::buffer(announced), asio::use_awaitable);
        co_await pause(std::chrono::milliseconds(50));
        initialized = co_await post(make_initialize_request("1"));
        boost::system::error_code ignored;
        (void)holder.close(ignored);
    });
    EXPECT_EQ(initialized.status, 200U) << initialized.body;
}

// The answer must be read within the time a request may take to arrive, so a peer that never reads
// a replay gives back what it holds once that time is up.
TEST_F(SessionLimitsTest, UnreadReplayGivesBackWhatItHolds) {
    manager_.set_connection_limits({0, std::chrono::seconds(5), 4096, 16U * 1024 * 1024});
    std::string second;
    run([&]() -> mcp::Task<void> {
        const auto initialized = co_await post(make_initialize_request("1"));
        const auto session = session_header(initialized);
        co_await post_only(make_initialized_notification(), session);
        auto call = make_tool_call_request("p", "progress", json{{"pad", 3000000}});
        call["params"]["_meta"] = {{"progressToken", "t"}};
        const auto progressed = (co_await post(call, session)).body;
        const auto id_at = progressed.find("id: ");
        if (id_at == std::string::npos) {
            ADD_FAILURE() << "the answer carries no event id: " << progressed.substr(0, 200);
            co_return;
        }
        const auto event_id = progressed.substr(id_at + 4, progressed.find('\n', id_at) - id_at - 4);
        const auto resume =
            "GET /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\nAccept: text/event-stream\r\n"
            "MCP-Session-Id: " +
            initialized.session_id.value_or("") + "\r\nLast-Event-ID: " + event_id + "\r\n\r\n";
        // Asks for the replay and never reads it.
        asio::ip::tcp::socket holder(io_);
        holder.open(asio::ip::tcp::v4());
        holder.set_option(asio::socket_base::receive_buffer_size(4096));
        co_await holder.async_connect({asio::ip::make_address("127.0.0.1"), port_},
                                      asio::use_awaitable);
        co_await asio::async_write(holder, asio::buffer(resume), asio::use_awaitable);
        co_await pause(std::chrono::seconds(6));
        second =
            co_await raw_exchange(io_.get_executor(), port_,
                                  resume.substr(0, resume.size() - 2) + "Connection: close\r\n\r\n",
                                  std::chrono::seconds(20));
        boost::system::error_code ignored;
        (void)holder.close(ignored);
    });
    EXPECT_EQ(second.rfind("HTTP/1.1 200", 0), 0U) << second.substr(0, 200);
}

// A read timeout too long to add to the current time sets no deadline.
TEST_F(SessionLimitsTest, ReadTimeoutTooLongForTheClockSetsNoDeadline) {
    manager_.set_connection_limits({0, std::chrono::milliseconds::max(), 4096, 1U << 30});
    Reply initialized;
    run([&]() -> mcp::Task<void> { initialized = co_await post(make_initialize_request("1")); });
    EXPECT_EQ(initialized.status, 200U) << initialized.body;
}

// A token check that outlasts the read deadline ends that connection, and the server goes on
// serving others.
TEST_F(SessionLimitsTest, TokenCheckPastTheReadDeadlineEndsOnlyThatConnection) {
    manager_.set_connection_limits({0, std::chrono::milliseconds(200), 4096, 1U << 30});
    manager_.set_async_bearer_token_validator([](std::string token) -> mcp::Task<bool> {
        if (token == "slow") {
            co_await pause(std::chrono::milliseconds(400));
        }
        co_return true;
    });
    std::string late;
    Reply initialized;
    run([&]() -> mcp::Task<void> {
        const auto body = make_initialize_request("1").dump();
        const auto head =
            "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\n"
            "Accept: application/json, text/event-stream\r\nAuthorization: Bearer slow\r\n"
            "Content-Length: " +
            std::to_string(body.size()) + "\r\n\r\n";
        asio::ip::tcp::socket slow(io_);
        co_await slow.async_connect({asio::ip::make_address("127.0.0.1"), port_}, asio::use_awaitable);
        co_await asio::async_write(slow, asio::buffer(head), asio::use_awaitable);
        // The body follows once the check has outlasted the deadline.
        co_await pause(std::chrono::milliseconds(500));
        boost::system::error_code ignored;
        asio::write(slow, asio::buffer(body), ignored);
        co_await pause(std::chrono::milliseconds(100));
        (void)slow.close(ignored);
        const Headers token{{"Authorization", "Bearer t"}};
        initialized = co_await post(make_initialize_request("2"), token);
    });
    EXPECT_EQ(initialized.status, 200U) << initialized.body;
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

// The session of a client that left idle and was taken over is gone, so its id is not found, as
// for a session that ended: a client then starts a new session instead of failing.
TEST_F(ReverseBindingTest, SessionTakenOverIsNotFoundForItsOldClient) {
    transport_->set_session_idle_timeout(std::chrono::milliseconds(20));
    Reply post_reply;
    std::string get_reply;
    std::string delete_reply;
    run([&]() -> mcp::Task<void> {
        const auto first = co_await post(make_initialize_request("first"));
        const auto version = json::parse(first.body)["result"]["protocolVersion"].get<std::string>();
        co_await pause(std::chrono::milliseconds(40));
        const auto second = co_await post(make_initialize_request("second"));
        EXPECT_EQ(second.status, 200u) << second.body;
        const Headers old_session{{"MCP-Session-Id", first.session_id.value_or("")},
                                  {"MCP-Protocol-Version", version}};
        post_reply = co_await post(ping(), old_session);
        const auto request_line = [&](const char* method) {
            return std::string(method) + " /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\nMCP-Session-Id: " +
                   first.session_id.value_or("") + "\r\nMCP-Protocol-Version: " + version +
                   "\r\nAccept: text/event-stream\r\nConnection: close\r\n\r\n";
        };
        get_reply = co_await raw_exchange(io_.get_executor(), transport_->port(), request_line("GET"),
                                          std::chrono::seconds(20));
        delete_reply = co_await raw_exchange(io_.get_executor(), transport_->port(),
                                             request_line("DELETE"), std::chrono::seconds(20));
    });
    EXPECT_EQ(post_reply.status, 404u) << post_reply.body;
    EXPECT_EQ(get_reply.rfind("HTTP/1.1 404", 0), 0u) << get_reply;
    EXPECT_EQ(delete_reply.rfind("HTTP/1.1 404", 0), 0u) << delete_reply;
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
    Reply held_again;
    Reply next;
    run([&]() -> mcp::Task<void> {
        const auto first = co_await open_session("a");
        for (int id = 1; id <= 8; ++id) {
            full = co_await post(subscribe(id), first);
        }
        held_again = co_await post(subscribe(1), first);
        co_await end_session(first.front().second);
        const auto second = co_await open_session("b");
        next = co_await post(subscribe(100), second);
    });
    EXPECT_NE(full.body.find("Too many resource subscriptions"), std::string::npos) << full.body;
    // A URI the session already holds takes nothing more, so it may be subscribed again at the
    // limit.
    EXPECT_NE(json::parse(held_again.body).value("result", json()), json()) << held_again.body;
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

// A session waiting for the answer to a request is in use, so another client's initialize does not
// take it over, however long the session has been quiet.
TEST_F(ReverseBindingTest, SessionWithARequestWaitingIsNotTakenOver) {
    server_->set_reverse_request_timeout(std::chrono::seconds(30));
    transport_->set_session_idle_timeout(std::chrono::milliseconds(20));
    Reply second;
    run([&]() -> mcp::Task<void> {
        const auto id = co_await session_waiting_for_an_answer();
        co_await pause(std::chrono::milliseconds(40));
        second = co_await post(make_initialize_request("second"));
        co_await end_session(id);
    });
    EXPECT_EQ(second.status, 400u) << second.body;
}

// Only an initialize without a session id takes over an idle session; one that names the session
// is a request of that session.
TEST_F(ReverseBindingTest, InitializeNamingTheSessionDoesNotTakeItOver) {
    transport_->set_session_idle_timeout(std::chrono::milliseconds(20));
    Reply later;
    run([&]() -> mcp::Task<void> {
        const auto first = co_await post(make_initialize_request("first"));
        const Headers session{
            {"MCP-Session-Id", first.session_id.value_or("")},
            {"MCP-Protocol-Version", json::parse(first.body)["result"]["protocolVersion"]}};
        co_await post_only(make_initialized_notification(), session);
        co_await pause(std::chrono::milliseconds(40));
        co_await post_only(make_initialize_request("again"), session);
        later = co_await post(ping(), session);
    });
    EXPECT_EQ(later.status, 200u) << later.body;
}

// Requests keep a session in use: the idle timeout counts from the last one.
TEST_F(ReverseBindingTest, UsedSessionIsNotTakenOver) {
    constexpr auto idle = std::chrono::milliseconds(1000);
    transport_->set_session_idle_timeout(idle);
    Reply second;
    run([&]() -> mcp::Task<void> {
        const auto first = co_await post(make_initialize_request("first"));
        const Headers session{
            {"MCP-Session-Id", first.session_id.value_or("")},
            {"MCP-Protocol-Version", json::parse(first.body)["result"]["protocolVersion"]}};
        co_await post_only(make_initialized_notification(), session);
        for (int i = 0; i < 4; ++i) {
            co_await pause(idle / 4);
            co_await post_only(ping(), session);
        }
        co_await pause(idle / 4);
        second = co_await post(make_initialize_request("second"));
    });
    EXPECT_EQ(second.status, 400u) << second.body;
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

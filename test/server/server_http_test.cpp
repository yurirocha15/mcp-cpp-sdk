#include "mcp/client/client.hpp"
#include "mcp/core/async.hpp"
#include "mcp/detail/signal.hpp"
#include "mcp/server/server.hpp"
#include "mcp/transport/http_session_manager.hpp"

#include <gtest/gtest.h>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <mcp/core/constants.hpp>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace server_http_test {

namespace protocol = mcp::constants::protocol;

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;

nlohmann::json make_initialize_request(std::string_view id) {
    return {{"jsonrpc", "2.0"},
            {"id", id},
            {"method", "initialize"},
            {"params",
             {{"protocolVersion", protocol::g_LATEST_LEGACY_PROTOCOL_VERSION},
              {"clientInfo", {{"name", "test-client"}, {"version", "0.1"}}},
              {"capabilities", nlohmann::json::object()}}}};
}

nlohmann::json make_tool_call_request(std::string_view id, const std::string& tool_name,
                                      const nlohmann::json& args) {
    return {{"jsonrpc", "2.0"},
            {"id", id},
            {"method", "tools/call"},
            {"params", {{"name", tool_name}, {"arguments", args}}}};
}

nlohmann::json make_initialized_notification() {
    return {{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}};
}

nlohmann::json greet_schema() {
    return {
        {"type", "object"},
        {"properties", {{"name", {{"type", "string"}}}}},
        {"required", nlohmann::json::array({"name"})},
    };
}

http::response<http::string_body> send_json_rpc(beast::tcp_stream& stream,
                                                const nlohmann::json& payload,
                                                const std::string& session_id = "") {
    http::request<http::string_body> req{http::verb::post, "/mcp", 11};
    req.set(http::field::host, "127.0.0.1");
    req.set(http::field::content_type, "application/json");
    req.set("MCP-Protocol-Version", std::string(protocol::g_LATEST_LEGACY_PROTOCOL_VERSION));
    if (!session_id.empty()) {
        req.set("MCP-Session-Id", session_id);
    }
    req.body() = payload.dump();
    req.prepare_payload();

    http::write(stream, req);

    beast::flat_buffer buf;
    http::response<http::string_body> resp;
    http::read(stream, buf, resp);
    return resp;
}

// Connects once the server accepts connections; the per-test timeout ends a test whose server
// never does.
void connect_when_listening(beast::tcp_stream& stream, uint16_t port) {
    const asio::ip::tcp::endpoint endpoint(asio::ip::make_address("127.0.0.1"), port);
    while (true) {
        beast::error_code ec;
        stream.socket().connect(endpoint, ec);
        if (!ec) {
            return;
        }
        stream.socket().close(ec);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

// Stops the server and joins its thread when a test throws before its own join, so the joinable
// std::thread is never destroyed.
class ServerThreadGuard {
   public:
    explicit ServerThreadGuard(std::thread& thread) : thread_(thread) {}
    ServerThreadGuard(const ServerThreadGuard&) = delete;
    ServerThreadGuard& operator=(const ServerThreadGuard&) = delete;
    ~ServerThreadGuard() {
        if (thread_.joinable()) {
            mcp::detail::trigger_shutdown_signal();
            thread_.join();
        }
    }

   private:
    std::thread& thread_;
};

/// The port @p server listens on once run_http() has bound it; the per-test timeout ends a test
/// whose server never does.
uint16_t wait_for_port(const mcp::Server& server) {
    while (server.bound_port() == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return server.bound_port();
}

/// The HTTP status of an initialize request sent with the Origin header @p origin.
unsigned status_with_origin(uint16_t port, const std::string& origin) {
    asio::io_context client_ctx;
    beast::tcp_stream stream(client_ctx);
    connect_when_listening(stream, port);
    http::request<http::string_body> req{http::verb::post, "/mcp", 11};
    req.set(http::field::host, "127.0.0.1");
    req.set(http::field::content_type, "application/json");
    req.set(http::field::origin, origin);
    req.body() = make_initialize_request("1").dump();
    req.prepare_payload();
    http::write(stream, req);
    beast::flat_buffer buf;
    http::response<http::string_body> resp;
    http::read(stream, buf, resp);
    beast::error_code ec;
    stream.socket().shutdown(asio::ip::tcp::socket::shutdown_both, ec);
    return resp.result_int();
}

// Stops the server with stop() and joins its thread when a test ends before its own join. stop()
// does nothing until the run has started, so it is repeated until the thread has finished.
class StopGuard {
   public:
    StopGuard(mcp::Server& server, std::thread& thread, const std::atomic<bool>& finished)
        : server_(server), thread_(thread), finished_(finished) {}
    StopGuard(const StopGuard&) = delete;
    StopGuard& operator=(const StopGuard&) = delete;
    ~StopGuard() {
        if (!thread_.joinable()) {
            return;
        }
        while (!finished_.load()) {
            server_.stop();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        thread_.join();
    }

   private:
    mcp::Server& server_;
    std::thread& thread_;
    const std::atomic<bool>& finished_;
};

}  // namespace

class ServerHttpTest : public ::testing::Test {
   protected:
    ServerHttpTest() {
        mcp::Implementation info;
        info.name = "test-http-server";
        info.version = "1.0";

        mcp::ServerCapabilities caps;
        caps.tools = mcp::ServerCapabilities::ToolsCapability{};

        server_ = std::make_unique<mcp::Server>(std::move(info), std::move(caps));

        server_->add_tool<nlohmann::json, nlohmann::json>(
            "greet", "Greets the user", greet_schema(),
            [](const nlohmann::json& args) -> mcp::Task<nlohmann::json> {
                co_return nlohmann::json{{"greeting", "Hello, " + args.at("name").get<std::string>()}};
            });
    }

    std::unique_ptr<mcp::Server> server_;
};

TEST_F(ServerHttpTest, RunHttpInitializesAndResponds) {
    constexpr uint16_t port = 18070;

    std::thread server_thread([&] { server_->run_http("127.0.0.1", port); });
    const ServerThreadGuard guard(server_thread);

    asio::io_context client_ctx;
    beast::tcp_stream stream(client_ctx);
    connect_when_listening(stream, port);

    auto init_resp = send_json_rpc(stream, make_initialize_request("1"));
    auto init_json = nlohmann::json::parse(init_resp.body());

    std::string session_id;
    auto session_it = init_resp.find("MCP-Session-Id");
    if (session_it != init_resp.end()) {
        session_id = std::string(session_it->value());
    }

    static_cast<void>(send_json_rpc(stream, make_initialized_notification(), session_id));
    auto tool_resp =
        send_json_rpc(stream, make_tool_call_request("2", "greet", {{"name", "HTTP"}}), session_id);
    auto tool_json = nlohmann::json::parse(tool_resp.body());

    beast::error_code ec;
    stream.socket().shutdown(asio::ip::tcp::socket::shutdown_both, ec);

    mcp::detail::trigger_shutdown_signal();
    server_thread.join();

    EXPECT_EQ(init_json["id"], "1");
    ASSERT_TRUE(init_json.contains("result"));
    EXPECT_EQ(init_json["result"]["serverInfo"]["name"], "test-http-server");

    EXPECT_EQ(tool_json["id"], "2");
    ASSERT_TRUE(tool_json.contains("result"));
    EXPECT_EQ(tool_json["result"]["structuredContent"]["greeting"], "Hello, HTTP");
}

TEST_F(ServerHttpTest, RunHttpInvalidAddressThrows) {
    EXPECT_THROW(server_->run_http("not-a-valid-address", 18071), std::runtime_error);
}

TEST_F(ServerHttpTest, RunHttpShutdownOnSignal) {
    constexpr uint16_t port = 18072;

    std::thread server_thread([&] { server_->run_http("127.0.0.1", port); });
    const ServerThreadGuard guard(server_thread);

    // An answered request shows that run_http reached its event loop, which it starts after
    // installing its SIGINT handler, so the signal below cannot end the test program.
    asio::io_context client_ctx;
    beast::tcp_stream stream(client_ctx);
    connect_when_listening(stream, port);
    static_cast<void>(send_json_rpc(stream, make_initialize_request("1")));
    beast::error_code ec;
    stream.socket().shutdown(asio::ip::tcp::socket::shutdown_both, ec);

    mcp::detail::trigger_shutdown_signal();

    server_thread.join();
}

// Each older-protocol client gets a session of its own, served with the one server's tools.
TEST_F(ServerHttpTest, RunHttpServesSeveralLegacySessions) {
    constexpr uint16_t port = 18073;

    std::thread server_thread([&] { server_->run_http("127.0.0.1", port); });
    const ServerThreadGuard guard(server_thread);

    asio::io_context client_ctx;
    beast::tcp_stream first(client_ctx);
    beast::tcp_stream second(client_ctx);
    connect_when_listening(first, port);
    connect_when_listening(second, port);

    std::vector<std::string> sessions;
    for (auto* stream : {&first, &second}) {
        auto init_resp = send_json_rpc(*stream, make_initialize_request("1"));
        ASSERT_TRUE(nlohmann::json::parse(init_resp.body()).contains("result")) << init_resp.body();
        const auto session_it = init_resp.find("MCP-Session-Id");
        ASSERT_NE(session_it, init_resp.end());
        sessions.emplace_back(session_it->value());
        static_cast<void>(send_json_rpc(*stream, make_initialized_notification(), sessions.back()));
    }
    EXPECT_NE(sessions[0], sessions[1]);

    const std::vector<std::string> names = {"first", "second"};
    for (std::size_t i = 0; i < names.size(); ++i) {
        auto& stream = i == 0 ? first : second;
        auto tool_resp = send_json_rpc(
            stream, make_tool_call_request("2", "greet", {{"name", names[i]}}), sessions[i]);
        const auto tool_json = nlohmann::json::parse(tool_resp.body());
        ASSERT_TRUE(tool_json.contains("result")) << tool_resp.body();
        EXPECT_EQ(tool_json["result"]["structuredContent"]["greeting"], "Hello, " + names[i]);
    }

    beast::error_code ec;
    first.socket().shutdown(asio::ip::tcp::socket::shutdown_both, ec);
    second.socket().shutdown(asio::ip::tcp::socket::shutdown_both, ec);
    mcp::detail::trigger_shutdown_signal();
    server_thread.join();
}

// A notification from the server reaches the session of every older-protocol client.
TEST_F(ServerHttpTest, RunHttpNotifiesEveryLegacySession) {
    constexpr uint16_t port = 18074;

    std::thread server_thread([&] { server_->run_http("127.0.0.1", port); });
    const ServerThreadGuard guard(server_thread);
    {
        asio::io_context probe_ctx;
        beast::tcp_stream stream(probe_ctx);
        connect_when_listening(stream, port);
    }

    mcp::ClientOptions options;
    options.protocol = mcp::ClientProtocol::legacy();
    const auto endpoint = mcp::Endpoint::http("http://127.0.0.1:" + std::to_string(port) + "/mcp");
    mcp::Client first(endpoint, options);
    mcp::Client second(endpoint, options);
    std::atomic<int> first_seen{0};
    std::atomic<int> second_seen{0};
    first.on_notification("notifications/tools/list_changed",
                          [&](const nlohmann::json&) { ++first_seen; });
    second.on_notification("notifications/tools/list_changed",
                           [&](const nlohmann::json&) { ++second_seen; });
    first.sync().connect("first", "1.0");
    second.sync().connect("second", "1.0");

    // Each client opens the stream that carries such notifications after connecting, so the
    // notification is sent until both have one.
    while (first_seen.load() == 0 || second_seen.load() == 0) {
        asio::io_context notify_ctx;
        auto sent = asio::co_spawn(notify_ctx, server_->notify_tools_list_changed(), asio::use_future);
        notify_ctx.run();
        sent.get();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    EXPECT_GT(first_seen.load(), 0);
    EXPECT_GT(second_seen.load(), 0);

    first.close();
    second.close();
    mcp::detail::trigger_shutdown_signal();
    server_thread.join();
}

// run_http() on port 0 reports the port the system chose, applies configure before it listens, and
// returns on stop().
TEST_F(ServerHttpTest, ConfigureAndStopRunHttpOnTheBoundPort) {
    EXPECT_EQ(server_->bound_port(), 0);
    std::atomic<bool> configured{false};
    std::atomic<bool> finished{false};
    std::thread server_thread([&] {
        server_->run_http("127.0.0.1", 0, [&configured](mcp::StreamableHttpSessionManager& http) {
            http.set_allowed_origins({"https://app.example"});
            configured = true;
        });
        finished = true;
    });
    const StopGuard guard(*server_, server_thread, finished);

    const auto port = wait_for_port(*server_);
    EXPECT_TRUE(configured.load());
    EXPECT_EQ(status_with_origin(port, "https://app.example"), 200U);
    EXPECT_EQ(status_with_origin(port, "https://other.example"), 403U);

    server_->stop();
    server_thread.join();
    EXPECT_EQ(server_->bound_port(), 0);
}

// stop() ends run_http() after the shutdown timeout while a handler that never finishes still runs.
TEST_F(ServerHttpTest, StopEndsRunHttpWhileAHandlerStillRuns) {
    std::atomic<bool> handler_started{false};
    std::atomic<bool> release{false};
    server_->add_tool("stuck", "Runs until released", nlohmann::json{{"type", "object"}},
                      [&](const nlohmann::json&) -> mcp::Task<nlohmann::json> {
                          handler_started = true;
                          auto executor = co_await asio::this_coro::executor;
                          // Cancellation does not end it; only release does.
                          while (!release.load()) {
                              asio::steady_timer timer(executor, std::chrono::milliseconds(20));
                              boost::system::error_code ec;
                              co_await timer.async_wait(asio::redirect_error(asio::use_awaitable, ec));
                          }
                          co_return nlohmann::json("released");
                      });

    std::atomic<bool> finished{false};
    std::thread server_thread([&] {
        server_->run_http("127.0.0.1", 0);
        finished = true;
    });
    const StopGuard guard(*server_, server_thread, finished);
    const auto port = wait_for_port(*server_);

    std::thread client_thread([port] {
        asio::io_context client_ctx;
        beast::tcp_stream stream(client_ctx);
        connect_when_listening(stream, port);
        try {
            const auto init_resp = send_json_rpc(stream, make_initialize_request("1"));
            const std::string session_id(init_resp["MCP-Session-Id"]);
            static_cast<void>(send_json_rpc(stream, make_initialized_notification(), session_id));
            static_cast<void>(send_json_rpc(
                stream, make_tool_call_request("2", "stuck", nlohmann::json::object()), session_id));
        } catch (const std::exception&) {
            // The server closes the connection before it answers the call.
        }
    });

    while (!handler_started.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    server_->stop();
    server_thread.join();
    EXPECT_TRUE(finished.load());
    EXPECT_FALSE(release.load());
    release = true;
    client_thread.join();
}

TEST_F(ServerHttpTest, ConfigureThatThrowsEndsRunHttpBeforeItListens) {
    EXPECT_THROW(server_->run_http("127.0.0.1", 0,
                                   [](mcp::StreamableHttpSessionManager&) {
                                       throw std::runtime_error("bad settings");
                                   }),
                 std::runtime_error);
    EXPECT_EQ(server_->bound_port(), 0);
}

// server.sync() notifies every session from a thread of the application, and refuses to wait on
// the event loop's own thread, where a synchronous handler runs.
TEST_F(ServerHttpTest, SyncNotifiesTheSessionsOfRunHttpFromAnotherThread) {
    server_->add_tool("notify_from_handler", "Notifies from its handler",
                      nlohmann::json{{"type", "object"}},
                      [this](const nlohmann::json&) -> nlohmann::json {
                          try {
                              server_->sync().notify_tools_list_changed();
                          } catch (const std::logic_error&) {
                              return "refused";
                          }
                          return "sent";
                      });
    std::atomic<bool> finished{false};
    std::thread server_thread([&] {
        server_->run_http("127.0.0.1", 0);
        finished = true;
    });
    const StopGuard guard(*server_, server_thread, finished);
    const auto port = wait_for_port(*server_);

    mcp::ClientOptions options;
    options.protocol = mcp::ClientProtocol::legacy();
    mcp::Client client(mcp::Endpoint::http("http://127.0.0.1:" + std::to_string(port) + "/mcp"),
                       options);
    std::atomic<int> seen{0};
    client.on_notification("notifications/tools/list_changed",
                           [&seen](const nlohmann::json&) { ++seen; });
    client.sync().connect("sync-notify", "1.0");

    EXPECT_EQ(
        mcp::result_text(client.sync().call_tool("notify_from_handler", nlohmann::json::object())),
        "refused");
    // The client opens the stream that carries such notifications after connecting, so the
    // notification is sent until it has one.
    while (seen.load() == 0) {
        server_->sync().notify_tools_list_changed();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    EXPECT_GT(seen.load(), 0);

    client.close();
    server_->stop();
    server_thread.join();
}

}  // namespace server_http_test

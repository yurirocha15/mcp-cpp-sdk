#include "mcp/client/client.hpp"
#include "mcp/core/async.hpp"
#include "mcp/core/runtime.hpp"
#include "mcp/detail/signal.hpp"
#include "mcp/server/server.hpp"
#include "mcp/transport/http_client.hpp"
#include "mcp/transport/http_session_manager.hpp"

#include <gtest/gtest.h>

#include "support/hang_guard.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <exception>
#include <future>
#include <mcp/core/constants.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
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

// Connects once the server accepts connections.
void connect_when_listening(beast::tcp_stream& stream, uint16_t port) {
    const asio::ip::tcp::endpoint endpoint(asio::ip::make_address("127.0.0.1"), port);
    EXPECT_TRUE(eventually(
        [&] {
            beast::error_code ec;
            stream.socket().connect(endpoint, ec);
            if (ec) {
                stream.socket().close(ec);
                return false;
            }
            return true;
        },
        g_hang_guard, std::chrono::milliseconds(10)))
        << "the server never accepted a connection on port " << port;
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

/// The port @p server listens on once run_http() has bound it.
uint16_t wait_for_port(const mcp::Server& server) {
    EXPECT_TRUE(eventually([&] { return server.bound_port() != 0; }))
        << "run_http() never bound its port";
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
        EXPECT_TRUE(eventually(
            [this] {
                if (finished_.load()) {
                    return true;
                }
                server_.stop();
                return false;
            },
            g_hang_guard, std::chrono::milliseconds(10)))
            << "the server did not stop";
        thread_.join();
    }

   private:
    mcp::Server& server_;
    std::thread& thread_;
    const std::atomic<bool>& finished_;
};

/// Connects a client of @p client_protocol to @p url and calls greet: the protocol version the
/// session uses, or "failed" when the client cannot connect or call.
std::string connect_and_greet(const std::string& url, mcp::ClientProtocol client_protocol) {
    mcp::ClientOptions options;
    options.protocol = std::move(client_protocol);
    try {
        mcp::Client client(mcp::Endpoint::http(url), options);
        const auto connected = client.sync().connect("policy-test", "1.0");
        static_cast<void>(client.sync().call_tool("greet", nlohmann::json{{"name", "Ada"}}));
        return connected.protocolVersion;
    } catch (const std::exception&) {
        return "failed";
    }
}

/// A transport that delivers initialize and notifications/initialized, then waits until closed,
/// and whose writes throw once fail_writes is set.
class FailingWritesTransport final : public mcp::ITransport {
   public:
    explicit FailingWritesTransport(const asio::any_io_executor& executor) : wake_(executor) {
        incoming_.push_back(make_initialize_request("1").dump());
        incoming_.push_back(make_initialized_notification().dump());
    }

    mcp::Task<std::string> read_message() override {
        if (!incoming_.empty()) {
            auto message = std::move(incoming_.front());
            incoming_.erase(incoming_.begin());
            co_return message;
        }
        wake_.expires_at(asio::steady_timer::time_point::max());
        boost::system::error_code ignored;
        co_await wake_.async_wait(asio::redirect_error(asio::use_awaitable, ignored));
        throw std::runtime_error("transport closed");
    }

    // [gcc11-sso: string_view]
    mcp::Task<void> write_message(std::string_view /*message*/) override {
        if (fail_writes.load()) {
            throw std::runtime_error("transport closed");
        }
        written.fetch_add(1);
        co_return;
    }

    void close() override {
        asio::post(wake_.get_executor(), [this] { wake_.cancel(); });
    }

    std::atomic<bool> fail_writes{false};
    std::atomic<int> written{0};

   private:
    std::vector<std::string> incoming_;
    asio::steady_timer wake_;
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
    eventually(
        [&] {
            if (first_seen.load() != 0 && second_seen.load() != 0) {
                return true;
            }
            asio::io_context notify_ctx;
            auto sent =
                asio::co_spawn(notify_ctx, server_->notify_tools_list_changed(), asio::use_future);
            notify_ctx.run();
            sent.get();
            return false;
        },
        g_hang_guard, std::chrono::milliseconds(20));
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

    EXPECT_TRUE(eventually([&] { return handler_started.load(); }));
    server_->stop();
    server_thread.join();
    EXPECT_TRUE(finished.load());
    EXPECT_FALSE(release.load());
    release = true;
    client_thread.join();
}

// stop() ends run_http() after the shutdown timeout while a handler on the handler executor still
// runs too; released afterwards, that handler can no longer reach the client.
TEST_F(ServerHttpTest, StopEndsRunHttpWhileAHandlerOnTheHandlerExecutorStillRuns) {
    asio::thread_pool pool(1);
    server_->set_handler_executor(pool.get_executor());
    std::promise<void> started;
    std::promise<void> release;
    const std::shared_future<void> released = release.get_future().share();
    std::promise<std::string> outcome;
    server_->add_tool("stuck", "Runs until released", nlohmann::json{{"type", "object"}},
                      [&started, released, &outcome](
                          mcp::Context& ctx, const nlohmann::json&) -> mcp::Task<nlohmann::json> {
                          started.set_value();
                          released.wait();
                          try {
                              co_await ctx.log_info("after the run ended");
                              outcome.set_value("sent");
                          } catch (const std::runtime_error& error) {
                              outcome.set_value(error.what());
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

    started.get_future().wait();
    server_->stop();
    server_thread.join();
    EXPECT_TRUE(finished.load());
    release.set_value();
    EXPECT_EQ(outcome.get_future().get(), "The server has stopped");
    client_thread.join();
    pool.join();
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
    eventually(
        [&] {
            if (seen.load() != 0) {
                return true;
            }
            server_->sync().notify_tools_list_changed();
            return false;
        },
        g_hang_guard, std::chrono::milliseconds(20));
    EXPECT_GT(seen.load(), 0);

    client.close();
    server_->stop();
    server_thread.join();
}

// A manager constructed with a server serves it on the application's loop as run_http() does: the
// server answers 2026-07-28 requests, and each legacy session gets a server with its registrations.
TEST_F(ServerHttpTest, ManagerServesTheServerItIsGiven) {
    asio::io_context io_ctx;
    mcp::StreamableHttpSessionManager manager(io_ctx.get_executor(), "127.0.0.1", 0, *server_);
    EXPECT_EQ(manager.modern_server().get(), server_.get());
    const std::weak_ptr<mcp::Server> held_weakly = manager.modern_server();
    EXPECT_FALSE(held_weakly.expired());
    EXPECT_EQ(held_weakly.lock().get(), server_.get());
    const auto url = "http://127.0.0.1:" + std::to_string(manager.port()) + "/mcp";

    mcp::ClientOptions legacy_options;
    legacy_options.protocol = mcp::ClientProtocol::legacy();
    mcp::Client modern(std::make_shared<mcp::HttpClientTransport>(io_ctx.get_executor(), url),
                       io_ctx.get_executor());
    mcp::Client legacy(std::make_shared<mcp::HttpClientTransport>(io_ctx.get_executor(), url),
                       io_ctx.get_executor(), legacy_options);

    std::vector<std::string> protocols;
    std::vector<nlohmann::json> greetings;
    std::exception_ptr failure;
    asio::co_spawn(io_ctx, manager.listen(), asio::detached);
    asio::co_spawn(
        io_ctx,
        [&]() -> mcp::Task<void> {
            for (auto* client : {&modern, &legacy}) {
                const auto connected = co_await client->connect("manager-test", "1.0");
                protocols.push_back(connected.protocolVersion);
                const nlohmann::json args{{"name", "Ada"}};
                const auto result = co_await client->call_tool("greet", args);
                greetings.push_back(result.structuredContent.value_or(nlohmann::json()));
                client->close();
            }
        },
        [&](std::exception_ptr error) {
            failure = std::move(error);
            manager.close();
        });
    io_ctx.run();

    if (failure) {
        std::rethrow_exception(failure);
    }
    ASSERT_EQ(protocols.size(), 2U);
    EXPECT_EQ(protocols[0], protocol::g_LATEST_PROTOCOL_VERSION);
    EXPECT_EQ(protocols[1], protocol::g_LATEST_LEGACY_PROTOCOL_VERSION);
    ASSERT_EQ(greetings.size(), 2U);
    EXPECT_EQ(greetings[0].value("greeting", ""), "Hello, Ada");
    EXPECT_EQ(greetings[1].value("greeting", ""), "Hello, Ada");
}

// A policy set on the Server before run_http() holds for its HTTP endpoint, as one set through
// configure does: without legacy clients, initialize is answered 404 and GET 405.
TEST_F(ServerHttpTest, APolicySetOnTheServerHoldsForTheEndpointOfRunHttp) {
    mcp::ServerProtocolPolicy policy;
    policy.serve_legacy = false;
    server_->set_protocol_policy(policy);
    std::atomic<bool> finished{false};
    std::thread server_thread([&] {
        server_->run_http("127.0.0.1", 0);
        finished = true;
    });
    const StopGuard guard(*server_, server_thread, finished);
    const auto port = wait_for_port(*server_);

    asio::io_context io;
    beast::tcp_stream initializing(io);
    connect_when_listening(initializing, port);
    const auto initialized = send_json_rpc(initializing, make_initialize_request("1"));
    beast::tcp_stream listening(io);
    connect_when_listening(listening, port);
    http::request<http::string_body> listen{http::verb::get, "/mcp", 11};
    listen.set(http::field::host, "127.0.0.1");
    listen.set(http::field::accept, "text/event-stream");
    http::write(listening, listen);
    beast::flat_buffer buffer;
    http::response<http::string_body> listened;
    http::read(listening, buffer, listened);
    server_->stop();
    server_thread.join();

    EXPECT_EQ(initialized.result(), http::status::not_found) << initialized.body();
    EXPECT_EQ(listened.result(), http::status::method_not_allowed) << listened.body();
}

// The manager's setters configure the server it serves, which its session servers share, so a
// policy given to run_http() holds for every session and every run.
TEST_F(ServerHttpTest, APolicyFromConfigureHoldsForEverySessionAndRun) {
    mcp::ServerProtocolPolicy policy;
    policy.legacy_versions = std::vector<std::string>{"2025-06-18"};
    for (int run = 0; run < 2; ++run) {
        std::atomic<bool> finished{false};
        std::thread server_thread([&] {
            server_->run_http("127.0.0.1", 0, [&policy](mcp::StreamableHttpSessionManager& http) {
                http.set_protocol_policy(policy);
            });
            finished = true;
        });
        const StopGuard guard(*server_, server_thread, finished);
        const auto url = "http://127.0.0.1:" + std::to_string(wait_for_port(*server_)) + "/mcp";
        for (int session = 0; session < 3; ++session) {
            EXPECT_EQ(connect_and_greet(url, mcp::ClientProtocol::legacy()), "2025-06-18")
                << "run " << run << ", session " << session;
        }
        server_->stop();
        server_thread.join();
    }
}

TEST_F(ServerHttpTest, APolicyFromConfigureHoldsBeforeAnyLegacySession) {
    mcp::ServerProtocolPolicy policy;
    policy.serve_modern = false;
    std::atomic<bool> finished{false};
    std::thread server_thread([&] {
        server_->run_http("127.0.0.1", 0, [&policy](mcp::StreamableHttpSessionManager& http) {
            http.set_protocol_policy(policy);
        });
        finished = true;
    });
    const StopGuard guard(*server_, server_thread, finished);
    const auto url = "http://127.0.0.1:" + std::to_string(wait_for_port(*server_)) + "/mcp";
    const auto modern = mcp::ClientProtocol::pinned(std::string(protocol::g_LATEST_PROTOCOL_VERSION));
    EXPECT_EQ(connect_and_greet(url, modern), "failed");
    EXPECT_EQ(connect_and_greet(url, mcp::ClientProtocol::legacy()),
              protocol::g_LATEST_LEGACY_PROTOCOL_VERSION);
    EXPECT_EQ(connect_and_greet(url, modern), "failed");
}

TEST_F(ServerHttpTest, AToolExecutorFromConfigureRunsTheToolsOfEveryClient) {
    mcp::Runtime pool;
    pool.start(1);
    std::promise<std::thread::id> pool_thread;
    asio::post(pool.executor().get(),
               [&pool_thread] { pool_thread.set_value(std::this_thread::get_id()); });
    const auto pool_thread_id = pool_thread.get_future().get();

    std::mutex threads_mutex;
    std::vector<std::thread::id> handler_threads;
    server_->add_tool("where", "Reports its thread", nlohmann::json{{"type", "object"}},
                      [&](const nlohmann::json&) -> nlohmann::json {
                          const std::lock_guard lock(threads_mutex);
                          handler_threads.push_back(std::this_thread::get_id());
                          return "here";
                      });
    std::atomic<bool> finished{false};
    std::thread server_thread([&] {
        server_->run_http("127.0.0.1", 0, [&pool](mcp::StreamableHttpSessionManager& http) {
            http.set_tool_executor(pool.executor());
        });
        finished = true;
    });
    const StopGuard guard(*server_, server_thread, finished);
    const auto url = "http://127.0.0.1:" + std::to_string(wait_for_port(*server_)) + "/mcp";
    for (const auto& client_protocol :
         {mcp::ClientProtocol::pinned(std::string(protocol::g_LATEST_PROTOCOL_VERSION)),
          mcp::ClientProtocol::legacy(),
          mcp::ClientProtocol::pinned(std::string(protocol::g_LATEST_PROTOCOL_VERSION))}) {
        mcp::ClientOptions options;
        options.protocol = client_protocol;
        mcp::Client client(mcp::Endpoint::http(url), options);
        static_cast<void>(client.sync().connect("executor-test", "1.0"));
        EXPECT_EQ(mcp::result_text(client.sync().call_tool("where", nlohmann::json::object())), "here");
    }
    server_->stop();
    server_thread.join();

    const std::lock_guard lock(threads_mutex);
    EXPECT_EQ(handler_threads, std::vector<std::thread::id>(3, pool_thread_id));
}

// A server served through a manager on the application's loop is used through its coroutines:
// a blocking notification is refused, as for run(), and stop() closes the manager.
TEST_F(ServerHttpTest, AServerServedByAManagerIsStoppedAndRefusesBlockingCalls) {
    asio::io_context io_ctx;
    mcp::StreamableHttpSessionManager manager(io_ctx.get_executor(), "127.0.0.1", 0, *server_);
    std::atomic<bool> listened{false};
    asio::co_spawn(io_ctx, manager.listen(),
                   [&listened](const std::exception_ptr&) { listened = true; });
    std::thread loop([&io_ctx] { io_ctx.run(); });

    // listen() joins the server's runs once its coroutine starts on the loop.
    const bool refused = eventually(
        [&] {
            try {
                server_->sync().notify_tools_list_changed();
                return false;
            } catch (const std::logic_error&) {
                return true;
            }
        },
        std::chrono::seconds(10));
    EXPECT_TRUE(refused);
    server_->stop();
    loop.join();
    EXPECT_TRUE(listened.load());
    // The run has ended, so the call returns, as for a server that is not running.
    EXPECT_NO_THROW(server_->sync().notify_tools_list_changed());
}

// A notification goes to every session sharing the server's registrations: one whose transport
// cannot take it is skipped, and the sessions after it still get it.
TEST_F(ServerHttpTest, ANotificationReachesTheSessionsAfterOneThatCannotTakeIt) {
    asio::io_context io_ctx;
    // The server's own session comes before the manager's legacy sessions.
    auto failing = std::make_shared<FailingWritesTransport>(io_ctx.get_executor());
    asio::co_spawn(io_ctx, server_->run(failing, io_ctx.get_executor()), asio::detached);
    mcp::StreamableHttpSessionManager manager(io_ctx.get_executor(), "127.0.0.1", 0, *server_);
    asio::co_spawn(io_ctx, manager.listen(), asio::detached);
    std::thread loop([&io_ctx] { io_ctx.run(); });
    EXPECT_TRUE(eventually([&] { return failing->written.load() != 0; }));

    std::mutex received_mutex;
    std::condition_variable received_changed;
    int received = 0;
    {
        mcp::ClientOptions options;
        options.protocol = mcp::ClientProtocol::legacy();
        mcp::Client client(
            mcp::Endpoint::http("http://127.0.0.1:" + std::to_string(manager.port()) + "/mcp"),
            options);
        client.on_notification("notifications/tools/list_changed", [&](const nlohmann::json&) {
            {
                const std::lock_guard lock(received_mutex);
                ++received;
            }
            received_changed.notify_all();
        });
        static_cast<void>(client.sync().connect("notify-test", "1.0"));
        const auto wait_for = [&](int count) {
            std::unique_lock lock(received_mutex);
            return received_changed.wait_for(lock, std::chrono::seconds(10),
                                             [&] { return received >= count; });
        };
        // The first one shows that the client's stream is open.
        bool delivered = false;
        for (int attempt = 0; attempt < 100 && !delivered; ++attempt) {
            asio::co_spawn(io_ctx, server_->notify_tools_list_changed(), asio::use_future).get();
            std::unique_lock lock(received_mutex);
            delivered = received_changed.wait_for(lock, std::chrono::milliseconds(100),
                                                  [&] { return received >= 1; });
        }
        ASSERT_TRUE(delivered);
        int before = 0;
        {
            const std::lock_guard lock(received_mutex);
            before = received;
        }

        failing->fail_writes = true;
        EXPECT_NO_THROW(
            asio::co_spawn(io_ctx, server_->notify_tools_list_changed(), asio::use_future).get());
        EXPECT_TRUE(wait_for(before + 1));
    }
    failing->close();
    manager.close();
    loop.join();
}

// ---------------------------------------------------------------------------
// Graceful shutdown of run_http()
// ---------------------------------------------------------------------------

namespace {

/// What the slow tool of a test reports and waits for.
struct Gate {
    std::atomic<bool> started{false};
    std::atomic<bool> release{false};
};

/// Waits until the test releases @p gate, or until the release guard runs out.
mcp::Task<void> wait_for_release(std::shared_ptr<Gate> gate) {
    const auto executor = co_await asio::this_coro::executor;
    const auto give_up = std::chrono::steady_clock::now() + g_release_guard;
    while (!gate->release.load() && std::chrono::steady_clock::now() < give_up) {
        asio::steady_timer timer(executor, std::chrono::milliseconds(10));
        boost::system::error_code ignored;
        co_await timer.async_wait(asio::redirect_error(asio::use_awaitable, ignored));
    }
}

/// The answer to one request of a raw client.
struct Answer {
    int status{-1};
    std::string body;
    bool keep_alive{true};
};

/// What a raw client of the slow tool read: the answer to the call, and to the request that was
/// sent behind it on the same connection, when one was.
struct Answers {
    Answer first;
    std::optional<Answer> second;
};

http::request<http::string_body> make_post(const nlohmann::json& payload,
                                           const std::string& session_id) {
    http::request<http::string_body> req{http::verb::post, "/mcp", 11};
    req.set(http::field::host, "127.0.0.1");
    req.set(http::field::content_type, "application/json");
    req.set("MCP-Protocol-Version", std::string(protocol::g_LATEST_LEGACY_PROTOCOL_VERSION));
    req.set("MCP-Session-Id", session_id);
    req.body() = payload.dump();
    req.prepare_payload();
    return req;
}

Answer read_answer(beast::tcp_stream& stream, beast::flat_buffer& buffer) {
    http::response<http::string_body> response;
    http::read(stream, buffer, response);
    return {.status = static_cast<int>(response.result_int()),
            .body = response.body(),
            .keep_alive = response.keep_alive()};
}

/// Calls the slow tool in a session of its own over a raw connection. With @p pipelined, a second
/// request is written in the same send, which the server reads once it has answered the call.
/// The first answer's status is -1 when the connection ends before it.
std::future<Answers> start_slow_call(uint16_t port, bool pipelined) {
    return std::async(std::launch::async, [port, pipelined] {
        Answers answers;
        asio::io_context client_ctx;
        beast::tcp_stream stream(client_ctx);
        connect_when_listening(stream, port);
        try {
            const auto init_resp = send_json_rpc(stream, make_initialize_request("1"));
            const std::string session_id(init_resp["MCP-Session-Id"]);
            static_cast<void>(send_json_rpc(stream, make_initialized_notification(), session_id));
            std::ostringstream wire;
            wire << make_post(make_tool_call_request("2", "slow", nlohmann::json::object()),
                              session_id);
            if (pipelined) {
                wire << make_post(make_tool_call_request("3", "greet", {{"name", "late"}}), session_id);
            }
            const auto text = wire.str();
            asio::write(stream, asio::buffer(text));
            beast::flat_buffer buffer;
            answers.first = read_answer(stream, buffer);
            if (pipelined) {
                answers.second = read_answer(stream, buffer);
            }
        } catch (const std::exception&) {
            // The server closed the connection before it answered.
        }
        return answers;
    });
}

}  // namespace

class ServerHttpDrainTest : public ServerHttpTest {
   protected:
    ServerHttpDrainTest() {
        server_->add_tool("slow", "Runs until released", nlohmann::json{{"type", "object"}},
                          [gate = gate_](const nlohmann::json&) -> mcp::Task<nlohmann::json> {
                              gate->started = true;
                              co_await wait_for_release(gate);
                              co_return nlohmann::json("done");
                          });
    }

    void TearDown() override { gate_->release = true; }

    /// Runs run_http() on a thread of its own with @p options, on a port the system chooses.
    uint16_t start(const mcp::HttpRunOptions& options) {
        thread_ = std::thread([this, options] {
            try {
                server_->run_http("127.0.0.1", 0, options);
            } catch (...) {
                run_error_ = std::current_exception();
            }
            finished_ = true;
        });
        guard_.emplace(*server_, thread_, finished_);
        return wait_for_port(*server_);
    }

    void join() {
        thread_.join();
        guard_.reset();
    }

    void note(const std::string& event) {
        const std::scoped_lock lock(events_mutex_);
        events_.push_back(event);
    }

    std::vector<std::string> events() {
        const std::scoped_lock lock(events_mutex_);
        return events_;
    }

    /// An in-flight request is answered when the run is ended by @p end.
    void expect_answer_in_flight(const std::function<void()>& end) {
        mcp::HttpRunOptions options;
        options.on_drain_start = [this] { note("drain"); };
        const auto port = start(options);

        auto call = start_slow_call(port, false);
        ASSERT_TRUE(eventually([&] { return gate_->started.load(); }));
        end();
        ASSERT_TRUE(eventually([&] { return !events().empty(); }));
        EXPECT_FALSE(finished_.load());
        gate_->release = true;

        const auto answers = call.get();
        EXPECT_EQ(answers.first.status, 200);
        EXPECT_NE(answers.first.body.find("done"), std::string::npos) << answers.first.body;
        join();
        EXPECT_FALSE(run_error_);
    }

    std::shared_ptr<Gate> gate_ = std::make_shared<Gate>();
    std::thread thread_;
    std::atomic<bool> finished_{false};
    std::exception_ptr run_error_;
    std::optional<StopGuard> guard_;
    std::mutex events_mutex_;
    std::vector<std::string> events_;
};

TEST_F(ServerHttpDrainTest, StopAnswersARequestInFlight) {
    expect_answer_in_flight([this] { server_->stop(); });
}

TEST_F(ServerHttpDrainTest, ASignalAnswersARequestInFlight) {
    expect_answer_in_flight([] { mcp::detail::trigger_shutdown_signal(); });
}

// A request that begins during the drain, on a connection that stayed open since its last answer,
// is refused, and the connection closes; the one in flight is still answered.
TEST_F(ServerHttpDrainTest, ARequestThatBeginsDuringTheDrainGets503AndTheConnectionCloses) {
    mcp::HttpRunOptions options;
    options.on_drain_start = [this] { note("drain"); };
    const auto port = start(options);

    asio::io_context client_ctx;
    beast::tcp_stream idle(client_ctx);
    connect_when_listening(idle, port);
    const auto init_resp = send_json_rpc(idle, make_initialize_request("1"));
    const std::string session_id(init_resp["MCP-Session-Id"]);
    static_cast<void>(send_json_rpc(idle, make_initialized_notification(), session_id));

    auto call = start_slow_call(port, false);
    ASSERT_TRUE(eventually([&] { return gate_->started.load(); }));
    server_->stop();
    ASSERT_TRUE(eventually([&] { return !events().empty(); }));

    http::write(idle, make_post(make_tool_call_request("3", "greet", {{"name", "late"}}), session_id));
    beast::flat_buffer buffer;
    const auto refused = read_answer(idle, buffer);
    EXPECT_EQ(refused.status, 503);
    EXPECT_FALSE(refused.keep_alive);

    gate_->release = true;
    EXPECT_EQ(call.get().first.status, 200);
    join();
}

// An answer written during the drain says Connection: close, and the server serves nothing behind
// it on that connection, so a client knows to send a request it pipelined again.
TEST_F(ServerHttpDrainTest, AnAnswerWrittenDuringTheDrainClosesItsConnection) {
    mcp::HttpRunOptions options;
    options.on_drain_start = [this] { note("drain"); };
    const auto port = start(options);

    auto call = start_slow_call(port, true);
    ASSERT_TRUE(eventually([&] { return gate_->started.load(); }));
    server_->stop();
    ASSERT_TRUE(eventually([&] { return !events().empty(); }));
    gate_->release = true;

    const auto answers = call.get();
    EXPECT_EQ(answers.first.status, 200);
    EXPECT_FALSE(answers.first.keep_alive);
    EXPECT_FALSE(answers.second.has_value());
    join();
}

// A legacy client's answer to the question of a request in flight is served during the drain, so
// the request is answered.
TEST_F(ServerHttpDrainTest, ALegacyClientsAnswerToAQuestionIsServedDuringTheDrain) {
    server_->add_tool<nlohmann::json, mcp::CallToolResult>(
        "ask", "Asks for a name once released", nlohmann::json{{"type", "object"}},
        [gate = gate_](mcp::Context& context, const nlohmann::json&) -> mcp::Task<mcp::CallToolResult> {
            gate->started = true;
            co_await wait_for_release(gate);
            mcp::ElicitRequestFormParams question;
            question.message = "Your name?";
            question.requestedSchema = {{"type", "object"},
                                        {"properties", {{"name", {{"type", "string"}}}}}};
            const auto params = std::make_shared<const mcp::ElicitRequestParams>(question);
            const auto answer = co_await context.elicit(*params);
            co_return mcp::make_tool_text_result(
                answer.content.value_or(nlohmann::json::object()).value("name", "nobody"));
        });
    mcp::HttpRunOptions options;
    options.on_drain_start = [this] { note("drain"); };
    const auto port = start(options);

    mcp::ClientOptions client_options;
    client_options.protocol = mcp::ClientProtocol::legacy();
    mcp::Client client(mcp::Endpoint::http("http://127.0.0.1:" + std::to_string(port) + "/mcp"),
                       client_options);
    client.on_elicitation([](const mcp::ElicitRequestParams&) -> mcp::Task<mcp::ElicitResult> {
        mcp::ElicitResult result;
        result.action = mcp::ElicitAction::eAccept;
        result.content = nlohmann::json{{"name", "mona"}};
        co_return result;
    });
    client.sync().connect("drain-test", "1.0");

    auto call = std::async(std::launch::async, [&client] {
        try {
            const nlohmann::json result = client.sync().call_tool("ask", nlohmann::json::object());
            return result.at("content").at(0).at("text").get<std::string>();
        } catch (const std::exception& error) {
            return std::string("failed: ") + error.what();
        }
    });
    ASSERT_TRUE(eventually([&] { return gate_->started.load(); }));
    server_->stop();
    ASSERT_TRUE(eventually([&] { return !events().empty(); }));
    gate_->release = true;

    EXPECT_EQ(call.get(), "mona");
    join();
    EXPECT_FALSE(run_error_);
    client.close();
}

// A legacy client's notification is served during the drain, on a connection it kept open and on
// a new one.
TEST_F(ServerHttpDrainTest, ALegacyClientsNotificationIsServedDuringTheDrain) {
    mcp::HttpRunOptions options;
    options.on_drain_start = [this] { note("drain"); };
    const auto port = start(options);

    asio::io_context client_ctx;
    beast::tcp_stream kept(client_ctx);
    connect_when_listening(kept, port);
    const auto init_resp = send_json_rpc(kept, make_initialize_request("1"));
    const std::string session_id(init_resp["MCP-Session-Id"]);
    static_cast<void>(send_json_rpc(kept, make_initialized_notification(), session_id));

    beast::tcp_stream calling(client_ctx);
    connect_when_listening(calling, port);
    http::write(calling,
                make_post(make_tool_call_request("2", "slow", nlohmann::json::object()), session_id));
    ASSERT_TRUE(eventually([&] { return gate_->started.load(); }));
    server_->stop();
    ASSERT_TRUE(eventually([&] { return !events().empty(); }));

    const nlohmann::json cancelled = {
        {"jsonrpc", "2.0"},
        {"method", "notifications/cancelled"},
        {"params", {{"requestId", "2"}, {"reason", "drain test"}}},
    };
    EXPECT_EQ(send_json_rpc(kept, cancelled, session_id).result_int(), 202U);
    beast::tcp_stream fresh(client_ctx);
    fresh.connect(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));
    EXPECT_EQ(send_json_rpc(fresh, cancelled, session_id).result_int(), 202U);

    gate_->release = true;
    join();
}

// The callbacks run once each however many times the run is told to end, and in order.
TEST_F(ServerHttpDrainTest, OnDrainStartRunsOnceBeforeOnShutdown) {
    mcp::HttpRunOptions options;
    options.on_drain_start = [this] { note("drain"); };
    options.on_shutdown = [this]() -> mcp::Task<void> {
        note("shutdown");
        co_return;
    };
    start(options);

    server_->stop();
    server_->stop();
    join();
    EXPECT_EQ(events(), (std::vector<std::string>{"drain", "shutdown"}));
}

// A handler that never finishes: the first wait ends at the drain timeout, on_shutdown runs, and
// run_http() returns at the shutdown timeout.
TEST_F(ServerHttpDrainTest, AHandlerThatNeverFinishesEndsTheFirstWaitAtTheDrainTimeout) {
    const auto drain_timeout = std::chrono::milliseconds(150);
    const auto shutdown_timeout = std::chrono::milliseconds(1500);
    std::mutex times_mutex;
    std::chrono::steady_clock::time_point shutdown_at;
    mcp::HttpRunOptions options;
    options.drain_timeout = drain_timeout;
    options.shutdown_timeout = shutdown_timeout;
    options.on_shutdown = [&]() -> mcp::Task<void> {
        {
            const std::scoped_lock lock(times_mutex);
            shutdown_at = std::chrono::steady_clock::now();
        }
        note("shutdown");
        co_return;
    };
    const auto port = start(options);

    auto call = start_slow_call(port, false);
    ASSERT_TRUE(eventually([&] { return gate_->started.load(); }));
    const auto stopped_at = std::chrono::steady_clock::now();
    server_->stop();
    join();
    const auto finished_at = std::chrono::steady_clock::now();

    EXPECT_EQ(events(), (std::vector<std::string>{"shutdown"}));
    EXPECT_GE(shutdown_at - stopped_at, drain_timeout);
    EXPECT_GE(finished_at - stopped_at, shutdown_timeout);
    EXPECT_TRUE(finished_.load());
    gate_->release = true;
    call.wait();
}

// An answer a handler produces because on_shutdown ran is delivered: the connections stay open
// while it runs.
TEST_F(ServerHttpDrainTest, AnAnswerProducedDuringOnShutdownIsDelivered) {
    mcp::HttpRunOptions options;
    options.drain_timeout = std::chrono::milliseconds(100);
    options.shutdown_timeout = std::chrono::seconds(30);
    options.on_shutdown = [gate = gate_]() -> mcp::Task<void> {
        gate->release = true;
        co_return;
    };
    const auto port = start(options);

    auto call = start_slow_call(port, false);
    ASSERT_TRUE(eventually([&] { return gate_->started.load(); }));
    server_->stop();

    const auto answers = call.get();
    EXPECT_EQ(answers.first.status, 200);
    EXPECT_NE(answers.first.body.find("done"), std::string::npos) << answers.first.body;
    join();
    EXPECT_FALSE(run_error_);
}

// An on_shutdown that does not return is cut off at the shutdown timeout.
TEST_F(ServerHttpDrainTest, AHangingOnShutdownIsCutAtTheShutdownTimeout) {
    const auto shutdown_timeout = std::chrono::milliseconds(500);
    mcp::HttpRunOptions options;
    options.drain_timeout = std::chrono::milliseconds(100);
    options.shutdown_timeout = shutdown_timeout;
    options.on_shutdown = []() -> mcp::Task<void> {
        const auto executor = co_await asio::this_coro::executor;
        for (;;) {
            asio::steady_timer timer(executor, std::chrono::milliseconds(10));
            boost::system::error_code ignored;
            co_await timer.async_wait(asio::redirect_error(asio::use_awaitable, ignored));
        }
    };
    start(options);

    const auto stopped_at = std::chrono::steady_clock::now();
    server_->stop();
    join();
    EXPECT_GE(std::chrono::steady_clock::now() - stopped_at, shutdown_timeout);
    EXPECT_TRUE(finished_.load());
    EXPECT_FALSE(run_error_);
}

// What on_start sets up is there for the first client, and the executor of configure's manager
// is the loop's.
TEST_F(ServerHttpDrainTest, OnStartIsAwaitedBeforeTheServerListens) {
    mcp::Executor manager_executor;
    mcp::HttpRunOptions options;
    options.configure = [&](mcp::StreamableHttpSessionManager& http) {
        manager_executor = http.get_executor();
    };
    options.on_start = [this](mcp::StreamableHttpSessionManager&) -> mcp::Task<void> {
        const auto executor = co_await asio::this_coro::executor;
        asio::steady_timer timer(executor, std::chrono::milliseconds(100));
        boost::system::error_code ignored;
        co_await timer.async_wait(asio::redirect_error(asio::use_awaitable, ignored));
        server_->add_tool("late", "Registered in on_start", nlohmann::json{{"type", "object"}},
                          [](const nlohmann::json&) -> nlohmann::json { return "late"; });
    };
    const auto port = start(options);
    EXPECT_TRUE(static_cast<bool>(manager_executor));

    mcp::Client client(mcp::Endpoint::http("http://127.0.0.1:" + std::to_string(port) + "/mcp"));
    client.sync().connect("drain-test", "1.0");
    std::set<std::string> names;
    for (const auto& tool : client.sync().list_tools().tools) {
        names.insert(tool.name);
    }
    EXPECT_TRUE(names.contains("late"));
    client.close();
    server_->stop();
    join();
}

// A failing on_start shuts the run down, including on_shutdown, and run_http() rethrows it.
TEST_F(ServerHttpDrainTest, AnExceptionFromOnStartIsRethrownAfterTheShutdown) {
    mcp::HttpRunOptions options;
    options.on_start = [](mcp::StreamableHttpSessionManager&) -> mcp::Task<void> {
        throw std::runtime_error("start failed");
        co_return;
    };
    options.on_drain_start = [this] { note("drain"); };
    options.on_shutdown = [this]() -> mcp::Task<void> {
        note("shutdown");
        co_return;
    };
    try {
        server_->run_http("127.0.0.1", 0, options);
        ADD_FAILURE() << "run_http() did not throw";
    } catch (const std::runtime_error& error) {
        EXPECT_STREQ(error.what(), "start failed");
    }
    EXPECT_EQ(events(), (std::vector<std::string>{"drain", "shutdown"}));
}

// A run that is ended while on_start still runs never listens, runs on_shutdown, and returns
// without an error once on_start has.
TEST_F(ServerHttpDrainTest, StopDuringOnStartSkipsListenAndRunsOnShutdown) {
    std::atomic<bool> entered{false};
    std::atomic<bool> proceed{false};
    mcp::HttpRunOptions options;
    options.on_start = [&](mcp::StreamableHttpSessionManager&) -> mcp::Task<void> {
        entered = true;
        const auto executor = co_await asio::this_coro::executor;
        const auto give_up = std::chrono::steady_clock::now() + g_release_guard;
        while (!proceed.load() && std::chrono::steady_clock::now() < give_up) {
            asio::steady_timer timer(executor, std::chrono::milliseconds(10));
            boost::system::error_code ignored;
            co_await timer.async_wait(asio::redirect_error(asio::use_awaitable, ignored));
        }
    };
    options.on_shutdown = [this]() -> mcp::Task<void> {
        note("shutdown");
        co_return;
    };
    const auto port = start(options);
    ASSERT_TRUE(eventually([&] { return entered.load(); }));

    // The system accepts the connection, but nothing reads from it.
    asio::io_context client_ctx;
    beast::tcp_stream stream(client_ctx);
    connect_when_listening(stream, port);
    http::request<http::string_body> req = make_post(make_initialize_request("1"), "");
    req.erase("MCP-Session-Id");
    http::write(stream, req);

    ASSERT_TRUE(eventually([&] {
        server_->stop();
        return !events().empty();
    }));
    proceed = true;
    join();
    EXPECT_FALSE(run_error_);
    EXPECT_EQ(events(), (std::vector<std::string>{"shutdown"}));

    bool answered = false;
    try {
        beast::flat_buffer buffer;
        answered = read_answer(stream, buffer).status == 200;
    } catch (const std::exception&) {
        // The connection ended without an answer.
    }
    EXPECT_FALSE(answered);
}

// An on_start that fails because on_shutdown closed what it waited on, such as an upstream
// connection, does not turn the shutdown into an error: run_http() returns normally.
TEST_F(ServerHttpDrainTest, AnOnStartThatFailsAfterTheShutdownBeganIsNotRethrown) {
    std::atomic<bool> entered{false};
    std::atomic<bool> upstream_closed{false};
    mcp::HttpRunOptions options;
    options.on_start = [&](mcp::StreamableHttpSessionManager&) -> mcp::Task<void> {
        entered = true;
        const auto executor = co_await asio::this_coro::executor;
        const auto give_up = std::chrono::steady_clock::now() + g_release_guard;
        while (!upstream_closed.load() && std::chrono::steady_clock::now() < give_up) {
            asio::steady_timer timer(executor, std::chrono::milliseconds(10));
            boost::system::error_code ignored;
            co_await timer.async_wait(asio::redirect_error(asio::use_awaitable, ignored));
        }
        throw std::runtime_error("the upstream connection closed");
    };
    options.on_shutdown = [this, &upstream_closed]() -> mcp::Task<void> {
        note("shutdown");
        upstream_closed = true;
        co_return;
    };
    start(options);
    ASSERT_TRUE(eventually([&] { return entered.load(); }));

    ASSERT_TRUE(eventually([&] {
        server_->stop();
        return !events().empty();
    }));
    join();
    EXPECT_FALSE(run_error_);
    EXPECT_EQ(events(), (std::vector<std::string>{"shutdown"}));
}

// At the start of the drain the subscriptions/listen stream of a 2026-07-28 client and the GET
// stream of a legacy session end, while a request is still being served.
TEST_F(ServerHttpDrainTest, ListenAndLegacyGetStreamsEndWhenTheDrainStarts) {
    const auto port = start({});
    const auto url = "http://127.0.0.1:" + std::to_string(port) + "/mcp";

    mcp::Client modern(mcp::Endpoint::http(url));
    modern.sync().connect("drain-test", "1.0");
    mcp::SubscriptionFilter filter;
    filter.toolsListChanged = true;
    auto subscription = modern.sync().listen(filter);

    std::atomic<bool> get_open{false};
    auto get_stream = std::async(std::launch::async, [&] {
        asio::io_context client_ctx;
        beast::tcp_stream stream(client_ctx);
        connect_when_listening(stream, port);
        try {
            const auto init_resp = send_json_rpc(stream, make_initialize_request("1"));
            const std::string session_id(init_resp["MCP-Session-Id"]);
            static_cast<void>(send_json_rpc(stream, make_initialized_notification(), session_id));
            http::request<http::string_body> req{http::verb::get, "/mcp", 11};
            req.set(http::field::host, "127.0.0.1");
            req.set(http::field::accept, "text/event-stream");
            req.set("MCP-Protocol-Version", std::string(protocol::g_LATEST_LEGACY_PROTOCOL_VERSION));
            req.set("MCP-Session-Id", session_id);
            http::write(stream, req);
            beast::flat_buffer buffer;
            http::response_parser<http::string_body> parser;
            http::read_header(stream, buffer, parser);
            get_open = true;
            http::read(stream, buffer, parser);
            return true;
        } catch (const std::exception&) {
            return false;
        }
    });
    ASSERT_TRUE(eventually([&] { return get_open.load(); }));

    auto call = start_slow_call(port, false);
    ASSERT_TRUE(eventually([&] { return gate_->started.load(); }));
    server_->stop();

    EXPECT_FALSE(subscription.sync().next().has_value());
    EXPECT_EQ(subscription.end_reason(), mcp::SubscriptionEnd::eServerClosed);
    EXPECT_TRUE(get_stream.get());
    EXPECT_FALSE(finished_.load());

    gate_->release = true;
    EXPECT_EQ(call.get().first.status, 200);
    join();
    modern.close();
}

// Without run_http(): the drain of a manager on the application's own loop.
TEST_F(ServerHttpDrainTest, TheManagersDrainWaitsForTheRequestsBeingServed) {
    asio::io_context io_ctx;
    auto work = asio::make_work_guard(io_ctx);
    mcp::StreamableHttpSessionManager manager(io_ctx.get_executor(), "127.0.0.1", 0, *server_);
    asio::co_spawn(io_ctx, manager.listen(), asio::detached);
    std::thread loop([&io_ctx] { io_ctx.run(); });
    const auto port = manager.port();

    auto call = start_slow_call(port, false);
    ASSERT_TRUE(eventually([&] { return gate_->started.load(); }));

    auto drained = asio::co_spawn(io_ctx, manager.drain(std::chrono::seconds(60)), asio::use_future);
    EXPECT_EQ(drained.wait_for(std::chrono::milliseconds(200)), std::future_status::timeout);
    // The drain refuses a new session.
    {
        asio::io_context client_ctx;
        beast::tcp_stream stream(client_ctx);
        beast::error_code ec;
        stream.socket().connect(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), port), ec);
        bool answered = false;
        if (!ec) {
            try {
                http::request<http::string_body> req{http::verb::post, "/mcp", 11};
                req.set(http::field::host, "127.0.0.1");
                req.set(http::field::content_type, "application/json");
                req.body() = make_initialize_request("1").dump();
                req.prepare_payload();
                http::write(stream, req);
                beast::flat_buffer buffer;
                answered = read_answer(stream, buffer).status == 200;
            } catch (const std::exception&) {
                // Closed without an answer.
            }
        }
        EXPECT_FALSE(answered);
    }

    gate_->release = true;
    EXPECT_EQ(call.get().first.status, 200);
    EXPECT_EQ(drained.wait_for(g_hang_guard), std::future_status::ready);
    drained.get();

    // A drain with nothing to wait for returns at once, and one with a timeout does not wait past it.
    asio::co_spawn(io_ctx, manager.drain(std::chrono::seconds(60)), asio::use_future).get();
    EXPECT_THROW(static_cast<void>(manager.drain(std::chrono::milliseconds(-1))),
                 std::invalid_argument);

    manager.close();
    work.reset();
    loop.join();
}

// The drain of one manager ends only the listen streams it carries, not those of another manager
// that serves the same server.
TEST_F(ServerHttpDrainTest, TheManagersDrainLeavesTheListenStreamsOfAnotherManager) {
    mcp::ServerCapabilities caps;
    caps.tools = mcp::ServerCapabilities::ToolsCapability{true};
    mcp::Server served(mcp::Implementation{"listen-test", "1.0"}, caps);
    asio::io_context io_ctx;
    auto work = asio::make_work_guard(io_ctx);
    mcp::StreamableHttpSessionManager drained(io_ctx.get_executor(), "127.0.0.1", 0, served);
    mcp::StreamableHttpSessionManager other(io_ctx.get_executor(), "127.0.0.1", 0, served);
    asio::co_spawn(io_ctx, drained.listen(), asio::detached);
    asio::co_spawn(io_ctx, other.listen(), asio::detached);
    std::thread loop([&io_ctx] { io_ctx.run(); });

    const auto url = [](uint16_t port) { return "http://127.0.0.1:" + std::to_string(port) + "/mcp"; };
    mcp::Client on_drained(mcp::Endpoint::http(url(drained.port())));
    mcp::Client on_other(mcp::Endpoint::http(url(other.port())));
    on_drained.sync().connect("drain-test", "1.0");
    on_other.sync().connect("drain-test", "1.0");
    mcp::SubscriptionFilter filter;
    filter.toolsListChanged = true;
    auto ended = on_drained.sync().listen(filter);
    auto kept = on_other.sync().listen(filter);
    ASSERT_TRUE(eventually([&] { return served.subscription_count() == 2; }));

    asio::co_spawn(io_ctx, drained.drain(std::chrono::seconds(60)), asio::use_future).get();
    EXPECT_FALSE(ended.sync().next().has_value());
    EXPECT_EQ(ended.end_reason(), mcp::SubscriptionEnd::eServerClosed);

    asio::co_spawn(io_ctx, served.notify_tools_list_changed(), asio::use_future).get();
    EXPECT_TRUE(kept.sync().next().has_value());
    EXPECT_EQ(served.subscription_count(), 1U);

    on_drained.close();
    on_other.close();
    drained.close();
    other.close();
    work.reset();
    loop.join();
}

// A drain whose time runs out with a request still being served returns, leaving it to close().
TEST_F(ServerHttpDrainTest, TheManagersDrainEndsAtItsTimeout) {
    asio::io_context io_ctx;
    auto work = asio::make_work_guard(io_ctx);
    mcp::StreamableHttpSessionManager manager(io_ctx.get_executor(), "127.0.0.1", 0, *server_);
    asio::co_spawn(io_ctx, manager.listen(), asio::detached);
    std::thread loop([&io_ctx] { io_ctx.run(); });

    auto call = start_slow_call(manager.port(), false);
    ASSERT_TRUE(eventually([&] { return gate_->started.load(); }));
    const auto began = std::chrono::steady_clock::now();
    asio::co_spawn(io_ctx, manager.drain(std::chrono::milliseconds(200)), asio::use_future).get();
    EXPECT_GE(std::chrono::steady_clock::now() - began, std::chrono::milliseconds(200));

    gate_->release = true;
    call.wait();
    manager.close();
    work.reset();
    loop.join();
}

// Two loop threads serve 2026-07-28 and legacy sessions at once, and the blocking calls of
// Server::sync() are refused on both.
TEST_F(ServerHttpDrainTest, TwoLoopThreadsServeModernAndLegacySessionsAndRefuseSync) {
    std::mutex probe_mutex;
    std::set<std::thread::id> refused_on;
    std::atomic<int> sent{0};
    server_->add_tool("probe", "Tries a blocking call on the loop", nlohmann::json{{"type", "object"}},
                      [&](const nlohmann::json&) -> nlohmann::json {
                          std::this_thread::sleep_for(std::chrono::milliseconds(2));
                          try {
                              server_->sync().notify_tools_list_changed();
                          } catch (const std::logic_error&) {
                              const std::scoped_lock lock(probe_mutex);
                              refused_on.insert(std::this_thread::get_id());
                              return "refused";
                          }
                          ++sent;
                          return "sent";
                      });

    std::atomic<std::thread::id> calling_thread{};
    mcp::HttpRunOptions options;
    options.io_threads = 2;
    options.on_drain_start = [this] { note("drain"); };
    thread_ = std::thread([&] {
        calling_thread = std::this_thread::get_id();
        try {
            server_->run_http("127.0.0.1", 0, options);
        } catch (...) {
            run_error_ = std::current_exception();
        }
        finished_ = true;
    });
    guard_.emplace(*server_, thread_, finished_);
    const auto port = wait_for_port(*server_);
    const auto url = "http://127.0.0.1:" + std::to_string(port) + "/mcp";

    std::vector<std::future<std::string>> clients;
    for (int index = 0; index < 8; ++index) {
        clients.push_back(std::async(std::launch::async, [&url, index] {
            mcp::ClientOptions client_options;
            if (index % 2 == 1) {
                client_options.protocol = mcp::ClientProtocol::legacy();
            }
            try {
                mcp::Client client(mcp::Endpoint::http(url), client_options);
                const auto connected = client.sync().connect("two-threads", "1.0");
                for (int call = 0; call < 30; ++call) {
                    const auto answer =
                        mcp::result_text(client.sync().call_tool("probe", nlohmann::json::object()));
                    if (answer != "refused") {
                        return std::string("unexpected: ") + answer;
                    }
                }
                return connected.protocolVersion;
            } catch (const std::exception& error) {
                return std::string("failed: ") + error.what();
            }
        }));
    }
    std::set<std::string> protocols;
    for (auto& client : clients) {
        protocols.insert(client.get());
    }
    EXPECT_EQ(protocols,
              (std::set<std::string>{std::string(protocol::g_LATEST_PROTOCOL_VERSION),
                                     std::string(protocol::g_LATEST_LEGACY_PROTOCOL_VERSION)}));
    EXPECT_EQ(sent.load(), 0);
    {
        const std::scoped_lock lock(probe_mutex);
        EXPECT_EQ(refused_on.size(), 2U);
        EXPECT_TRUE(refused_on.contains(calling_thread.load()));
    }
    server_->stop();
    join();
    EXPECT_FALSE(run_error_);
    EXPECT_EQ(events(), (std::vector<std::string>{"drain"}));
}

TEST_F(ServerHttpDrainTest, OptionsThatCannotWorkAreRefusedBeforeAnythingRuns) {
    const auto refused = [this](const std::function<void(mcp::HttpRunOptions&)>& change) {
        mcp::HttpRunOptions options;
        std::atomic<bool> configured{false};
        options.configure = [&configured](mcp::StreamableHttpSessionManager&) { configured = true; };
        change(options);
        EXPECT_THROW(server_->run_http("127.0.0.1", 0, options), std::invalid_argument);
        EXPECT_FALSE(configured.load());
    };
    refused([](mcp::HttpRunOptions& options) { options.io_threads = 0; });
    refused(
        [](mcp::HttpRunOptions& options) { options.shutdown_timeout = std::chrono::milliseconds(-1); });
    refused(
        [](mcp::HttpRunOptions& options) { options.drain_timeout = std::chrono::milliseconds(-1); });
    refused([](mcp::HttpRunOptions& options) {
        options.shutdown_timeout = std::chrono::milliseconds(100);
        options.drain_timeout = std::chrono::milliseconds(101);
    });
    refused([](mcp::HttpRunOptions& options) {
        options.shutdown_timeout = std::chrono::milliseconds(100);
        options.drain_timeout = std::chrono::milliseconds(100);
        options.on_shutdown = []() -> mcp::Task<void> { co_return; };
    });

    // Without on_shutdown the two may be equal.
    mcp::HttpRunOptions equal;
    equal.shutdown_timeout = std::chrono::milliseconds(100);
    equal.drain_timeout = std::chrono::milliseconds(100);
    equal.on_start = [](mcp::StreamableHttpSessionManager&) -> mcp::Task<void> {
        throw std::runtime_error("accepted");
        co_return;
    };
    EXPECT_THROW(server_->run_http("127.0.0.1", 0, equal), std::runtime_error);
}

}  // namespace server_http_test

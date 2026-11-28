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

// server.sync().notify() reaches every session of run_http() that began with initialize.
TEST_F(ServerHttpTest, NotifyReachesEachLegacySession) {
    std::atomic<bool> finished{false};
    std::thread server_thread([&] {
        server_->run_http("127.0.0.1", 0);
        finished = true;
    });
    const StopGuard guard(*server_, server_thread, finished);
    const auto port = wait_for_port(*server_);

    mcp::ClientOptions options;
    options.protocol = mcp::ClientProtocol::legacy();
    const auto endpoint = mcp::Endpoint::http("http://127.0.0.1:" + std::to_string(port) + "/mcp");
    mcp::Client first(endpoint, options);
    mcp::Client second(endpoint, options);
    std::atomic<int> first_seen{0};
    std::atomic<int> second_seen{0};
    first.on_notification("notifications/example/changed",
                          [&](const nlohmann::json& params) { first_seen += params.value("n", 0); });
    second.on_notification("notifications/example/changed",
                           [&](const nlohmann::json& params) { second_seen += params.value("n", 0); });
    first.sync().connect("first", "1.0");
    second.sync().connect("second", "1.0");
    // Each client opens the stream that carries such notifications after connecting.
    EXPECT_TRUE(eventually(
        [&] {
            if (first_seen.load() != 0 && second_seen.load() != 0) {
                return true;
            }
            server_->sync().notify("notifications/example/changed", nlohmann::json{{"n", 1}});
            return false;
        },
        g_hang_guard, std::chrono::milliseconds(20)));

    first.close();
    second.close();
    server_->stop();
    server_thread.join();
}

// notifications/elicitation/complete goes to the session that sent the elicitation: two clients of
// run_http() share the server's records, and each sees only its own completion. An id that is in
// progress in one session cannot be used by another.
TEST_F(ServerHttpTest, ElicitationCompleteReachesOnlyTheSessionThatAsked) {
    server_->add_tool<nlohmann::json, nlohmann::json>(
        "open_with_id", "Asks a URL with a given id", nlohmann::json{{"type", "object"}},
        [](mcp::Context& ctx, nlohmann::json args) -> mcp::Task<nlohmann::json> {
            mcp::ElicitRequestURLParams params;
            params.message = "Please sign in";
            params.url = "https://example.com/sign-in";
            params.elicitationId = args.at("id").get<std::string>();
            nlohmann::json outcome;
            try {
                const auto answer = co_await ctx.elicit(params);
                outcome = {{"action", nlohmann::json(answer.action)}};
            } catch (const std::invalid_argument&) {
                outcome = {{"error", "in use"}};
            }
            co_return outcome;
        });
    server_->add_tool<nlohmann::json, nlohmann::json>(
        "require_id", "Needs a URL elicitation with a given id", nlohmann::json{{"type", "object"}},
        [](mcp::Context& /*ctx*/, nlohmann::json args) -> mcp::Task<nlohmann::json> {
            mcp::ElicitRequestURLParams params;
            params.message = "Please sign in";
            params.url = "https://example.com/sign-in";
            params.elicitationId = args.at("id").get<std::string>();
            throw mcp::UrlElicitationRequired({params});
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
    const auto endpoint = mcp::Endpoint::http("http://127.0.0.1:" + std::to_string(port) + "/mcp");
    mcp::Client first(endpoint, options);
    mcp::Client second(endpoint, options);
    std::mutex mutex;
    std::vector<std::string> first_completed;
    std::vector<std::string> second_received;
    std::atomic<int> first_listed{0};
    std::atomic<int> second_listed{0};
    for (auto* client : {&first, &second}) {
        client->on_elicitation([](const mcp::ElicitRequestParams&) -> mcp::Task<mcp::ElicitResult> {
            mcp::ElicitResult answer;
            answer.action = mcp::ElicitAction::eAccept;
            co_return answer;
        });
    }
    first.on_elicitation_complete([&](const mcp::ElicitationCompleteNotificationParams& params) {
        const std::scoped_lock lock(mutex);
        first_completed.push_back(params.elicitationId);
    });
    // The second client sees every completion notification that reaches it, unfiltered.
    second.on_notification("notifications/elicitation/complete", [&](const nlohmann::json& params) {
        const std::scoped_lock lock(mutex);
        second_received.push_back(params.value("elicitationId", ""));
    });
    first.on_notification("notifications/tools/list_changed",
                          [&](const nlohmann::json&) { ++first_listed; });
    second.on_notification("notifications/tools/list_changed",
                           [&](const nlohmann::json&) { ++second_listed; });
    mcp::ClientCapabilities capabilities;
    capabilities.elicitation = mcp::ClientCapabilities::ElicitationCapability{
        .form = nlohmann::json::object(), .url = nlohmann::json::object()};
    first.sync().connect(mcp::Implementation{"first", "1.0"}, capabilities);
    second.sync().connect(mcp::Implementation{"second", "1.0"}, capabilities);
    // Each client opens the stream that carries notifications outside a request after
    // connecting, so the test waits until both have one.
    eventually(
        [&] {
            if (first_listed.load() != 0 && second_listed.load() != 0) {
                return true;
            }
            server_->sync().notify_tools_list_changed();
            return false;
        },
        g_hang_guard, std::chrono::milliseconds(20));

    const auto structured = [](const mcp::CallToolResult& result) {
        return result.structuredContent.value_or(nlohmann::json());
    };
    EXPECT_EQ(structured(first.sync().call_tool("open_with_id", {{"id", "a-flow"}})),
              (nlohmann::json{{"action", "accept"}}));
    EXPECT_EQ(structured(second.sync().call_tool("open_with_id", {{"id", "a-flow"}})),
              (nlohmann::json{{"error", "in use"}}));
    const auto error_code = [](mcp::Client& client, const char* id) {
        try {
            static_cast<void>(client.sync().call_tool("require_id", {{"id", id}}));
        } catch (const mcp::McpError& error) {
            return error.code();
        }
        return 0;
    };
    // The same id in a URL elicitation error is refused as well.
    EXPECT_EQ(error_code(second, "a-flow"), mcp::constants::jsonrpc::g_INTERNAL_ERROR);
    EXPECT_EQ(error_code(second, "c-flow"), mcp::constants::jsonrpc::g_URL_ELICITATION_REQUIRED);
    EXPECT_EQ(structured(second.sync().call_tool("open_with_id", {{"id", "b-flow"}})),
              (nlohmann::json{{"action", "accept"}}));

    EXPECT_TRUE(server_->sync().notify_elicitation_complete("a-flow"));
    EXPECT_FALSE(server_->sync().notify_elicitation_complete("a-flow"));
    EXPECT_FALSE(server_->sync().notify_elicitation_complete("never-sent"));
    EXPECT_TRUE(eventually(
        [&] {
            const std::scoped_lock lock(mutex);
            return !first_completed.empty();
        },
        g_hang_guard, std::chrono::milliseconds(5)));
    EXPECT_TRUE(server_->sync().notify_elicitation_complete("b-flow"));
    EXPECT_TRUE(eventually(
        [&] {
            const std::scoped_lock lock(mutex);
            return !second_received.empty();
        },
        g_hang_guard, std::chrono::milliseconds(5)));
    {
        const std::scoped_lock lock(mutex);
        EXPECT_EQ(first_completed, std::vector<std::string>{"a-flow"});
        EXPECT_EQ(second_received, std::vector<std::string>{"b-flow"});
    }

    first.close();
    second.close();
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

}  // namespace server_http_test

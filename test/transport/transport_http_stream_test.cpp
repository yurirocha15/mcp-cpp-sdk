#include "../test_utils.hpp"

#include "mcp/server/server.hpp"
#include "mcp/transport/http_session_manager.hpp"
#include "mcp/transport/memory.hpp"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <chrono>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using nlohmann::json;
using tcp = asio::ip::tcp;

/// Waits, a millisecond at a time, until @p condition holds. Gives up after 30 s so a broken
/// stream fails the test instead of hanging the run.
mcp::Task<bool> wait_until(std::function<bool()> condition) {
    auto executor = co_await asio::this_coro::executor;
    const auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!condition() && std::chrono::steady_clock::now() < give_up) {
        asio::steady_timer timer(executor, std::chrono::milliseconds(1));
        co_await timer.async_wait(asio::use_awaitable);
    }
    co_return condition();
}

/// Lets the executor run @p turns other handlers first. For a server-side event no test can
/// observe, such as the server reading a half-close it has no reason to answer: when the event has
/// not happened yet the test still passes, so it cannot fail falsely.
mcp::Task<void> yield_turns(int turns) {
    auto executor = co_await asio::this_coro::executor;
    for (int turn = 0; turn < turns; ++turn) {
        co_await asio::post(executor, asio::use_awaitable);
    }
}

/// The bytes of an HTTP request carrying @p body as a 2026-07-28 client sends it.
std::string modern_request_bytes(const json& body, bool accepts_sse = true, unsigned version = 11) {
    http::request<http::string_body> request{http::verb::post, "/mcp", version};
    request.set(http::field::host, "127.0.0.1");
    request.set(http::field::content_type, "application/json");
    request.set(http::field::accept,
                accepts_sse ? "application/json, text/event-stream" : "application/json");
    request.set("MCP-Protocol-Version", "2026-07-28");
    request.set("Mcp-Method", body["method"].get<std::string>());
    if (body["method"] == "tools/call") {
        request.set("Mcp-Name", body["params"]["name"].get<std::string>());
    }
    request.keep_alive(true);
    request.body() = body.dump();
    request.prepare_payload();
    std::ostringstream bytes;
    bytes << request;
    return bytes.str();
}

/// A tools/call whose handler may log at any level: a 2026-07-28 request is logged for only at
/// the level its own `_meta` names.
json call_tool(const json& id, const std::string& name) {
    auto body = make_modern_request(id, "tools/call", {{"name", name}, {"arguments", json::object()}});
    body["params"]["_meta"]["io.modelcontextprotocol/logLevel"] = "debug";
    return body;
}

/// Collects what @p transport delivers until it closes.
mcp::Task<void> read_all(std::shared_ptr<mcp::ITransport> transport, std::vector<std::string>* out) {
    try {
        for (;;) {
            out->push_back(co_await transport->read_message());
        }
    } catch (const std::exception&) {
    }
}

/// A raw client connection that reads whatever arrives, so a test can see a stream as it grows.
class RawClient {
   public:
    explicit RawClient(asio::any_io_executor executor) : socket_(std::move(executor)) {}

    mcp::Task<void> connect(unsigned short port) {
        co_await socket_.async_connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), port),
                                       asio::use_awaitable);
    }

    mcp::Task<void> send(std::string bytes) {
        co_await asio::async_write(socket_, asio::buffer(bytes), asio::use_awaitable);
    }

    /// Reads until @p text has arrived or the connection ends; returns whether it arrived.
    mcp::Task<bool> read_until(std::string text) {
        while (received_.find(text) == std::string::npos) {
            std::array<char, 4096> chunk{};
            boost::system::error_code error;
            const auto bytes = co_await socket_.async_read_some(
                asio::buffer(chunk), asio::redirect_error(asio::use_awaitable, error));
            if (error) {
                co_return false;
            }
            received_.append(chunk.data(), bytes);
        }
        co_return true;
    }

    /// Ends the connection with a reset, as a client that crashed or was killed does.
    void reset() {
        boost::system::error_code ignored;
        (void)socket_.set_option(asio::socket_base::linger(true, 0), ignored);
        (void)socket_.close(ignored);
    }

    void close() {
        boost::system::error_code ignored;
        (void)socket_.close(ignored);
    }

    void shutdown_send() {
        boost::system::error_code ignored;
        (void)socket_.shutdown(tcp::socket::shutdown_send, ignored);
    }

    [[nodiscard]] const std::string& received() const { return received_; }

   private:
    tcp::socket socket_;
    std::string received_;
};

class HttpStreamTest : public ::testing::Test {
   protected:
    asio::io_context io_ctx_;
    std::atomic<bool> released_{false};
    std::atomic<bool> handler_started_{false};
    std::atomic<bool> handler_done_{false};
    std::atomic<bool> handler_saw_cancel_{false};
    std::atomic<int> handlers_done_{0};

    std::unique_ptr<mcp::Server> make_server() {
        mcp::ServerCapabilities caps;
        caps.tools = mcp::ServerCapabilities::ToolsCapability{};
        caps.logging = json::object();
        auto server = std::make_unique<mcp::Server>(mcp::Implementation{"stream", "1.0"}, caps);
        server->add_tool<json, json>("echo", "Echoes", json{{"type", "object"}},
                                     [](json args) -> mcp::Task<json> { co_return args; });
        server->add_tool<json, json>(
            "log_then_wait", "Logs, then returns once released", json{{"type", "object"}},
            [this](mcp::Context& ctx, json /*args*/) -> mcp::Task<json> {
                handler_started_.store(true);
                co_await ctx.log(mcp::LoggingLevel::eError, "first");
                const bool released = co_await wait_until([this] { return released_.load(); });
                handler_done_.store(true);
                co_return json{{"released", released}};
            });
        server->add_tool<json, json>("wait", "Returns once released", json{{"type", "object"}},
                                     [this](json /*args*/) -> mcp::Task<json> {
                                         handler_started_.store(true);
                                         const bool released =
                                             co_await wait_until([this] { return released_.load(); });
                                         co_return json{{"released", released}};
                                     });
        server->add_tool<json, json>("log", "Logs once", json{{"type", "object"}},
                                     [](mcp::Context& ctx, json /*args*/) -> mcp::Task<json> {
                                         co_await ctx.log(mcp::LoggingLevel::eError, "only");
                                         co_return json{{"logged", true}};
                                     });
        server->add_tool<json, json>("flood", "Logs 100 times", json{{"type", "object"}},
                                     [](mcp::Context& ctx, json /*args*/) -> mcp::Task<json> {
                                         for (int index = 0; index < 100; ++index) {
                                             co_await ctx.log(mcp::LoggingLevel::eError, "flood");
                                         }
                                         co_return json{{"flooded", true}};
                                     });
        server->add_tool<json, json>("progress", "Reports progress", json{{"type", "object"}},
                                     [](mcp::Context& ctx, json /*args*/) -> mcp::Task<json> {
                                         co_await ctx.report_progress(1, 2, "halfway");
                                         co_return json{{"done", true}};
                                     });
        server->add_tool<json, json>(
            "log_then_wait_for_cancel", "Logs, then returns once cancelled", json{{"type", "object"}},
            [this](mcp::Context& ctx, json /*args*/) -> mcp::Task<json> {
                co_await ctx.log(mcp::LoggingLevel::eError, "first");
                const bool cancelled = co_await wait_until([&ctx] { return ctx.is_cancelled(); });
                handler_saw_cancel_.store(cancelled);
                handler_done_.store(true);
                co_return json{{"cancelled", cancelled}};
            });
        server->add_tool<json, json>(
            "wait_for_cancel", "Returns once cancelled", json{{"type", "object"}},
            [this](mcp::Context& ctx, json /*args*/) -> mcp::Task<json> {
                handler_started_.store(true);
                const bool cancelled = co_await wait_until([&ctx] { return ctx.is_cancelled(); });
                handler_saw_cancel_.store(cancelled);
                handlers_done_.fetch_add(1);
                handler_done_.store(true);
                co_return json{{"cancelled", cancelled}};
            });
        return server;
    }

    /// Runs @p exchange against a manager on @p port, then closes the manager.
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

    std::unique_ptr<mcp::StreamableHttpSessionManager> make_manager(unsigned short port) {
        auto manager = std::make_unique<mcp::StreamableHttpSessionManager>(
            io_ctx_.get_executor(), "127.0.0.1", port,
            [](const asio::any_io_executor&) -> std::unique_ptr<mcp::Server> { return nullptr; });
        manager->set_modern_server(make_server());
        return manager;
    }

    static std::string log_line(std::string_view text) {
        return R"("data":")" + std::string(text) + "\"";
    }
};

TEST_F(HttpStreamTest, NotificationIsDeliveredBeforeTheHandlerFinishes) {
    auto manager = make_manager(20430);
    RawClient client(io_ctx_.get_executor());
    bool saw_log = false;
    bool saw_end = false;
    run(*manager, [&]() -> mcp::Task<void> {
        co_await client.connect(20430);
        co_await client.send(modern_request_bytes(call_tool(1, "log_then_wait")));
        saw_log = co_await client.read_until(log_line("first"));
        // The handler is still waiting: what arrived was streamed, not buffered.
        EXPECT_FALSE(handler_done_.load());
        released_.store(true);
        saw_end = co_await client.read_until("0\r\n\r\n");
    });

    ASSERT_TRUE(saw_log) << client.received();
    ASSERT_TRUE(saw_end) << client.received();
    const auto& text = client.received();
    EXPECT_NE(text.find("Content-Type: text/event-stream"), std::string::npos) << text;
    EXPECT_NE(text.find("X-Accel-Buffering: no"), std::string::npos) << text;
    EXPECT_NE(text.find("Transfer-Encoding: chunked"), std::string::npos) << text;
    EXPECT_EQ(text.find("Mcp-Session-Id"), std::string::npos) << text;
    EXPECT_EQ(text.find("\nid:"), std::string::npos) << "no event ids on 2026-07-28 streams: " << text;
    const auto log_at = text.find("notifications/message");
    const auto result_at = text.find(R"("released":true)");
    ASSERT_NE(result_at, std::string::npos) << text;
    EXPECT_LT(log_at, result_at);
    EXPECT_NE(text.find(R"("resultType":"complete")"), std::string::npos) << text;
    EXPECT_EQ(manager->stream_stats().streams_opened, 1u);
}

TEST_F(HttpStreamTest, ProgressIsStreamedAheadOfTheResult) {
    auto manager = make_manager(20431);
    RawClient client(io_ctx_.get_executor());
    bool saw_end = false;
    run(*manager, [&]() -> mcp::Task<void> {
        auto body = call_tool(1, "progress");
        body["params"]["_meta"]["progressToken"] = "p1";
        co_await client.connect(20431);
        co_await client.send(modern_request_bytes(body));
        saw_end = co_await client.read_until("0\r\n\r\n");
    });
    ASSERT_TRUE(saw_end) << client.received();
    const auto& text = client.received();
    const auto progress_at = text.find("notifications/progress");
    ASSERT_NE(progress_at, std::string::npos) << text;
    EXPECT_NE(text.find(R"("progressToken":"p1")"), std::string::npos) << text;
    EXPECT_LT(progress_at, text.find(R"("done":true)"));
}

TEST_F(HttpStreamTest, AnswersWithoutNotificationsStayPlainJson) {
    auto manager = make_manager(20432);
    RawClient echo_client(io_ctx_.get_executor());
    RawClient json_only_client(io_ctx_.get_executor());
    RawClient http10_client(io_ctx_.get_executor());
    run(*manager, [&]() -> mcp::Task<void> {
        co_await echo_client.connect(20432);
        co_await echo_client.send(modern_request_bytes(call_tool(1, "echo")));
        co_await echo_client.read_until(R"("resultType":"complete")");
        // A client that does not accept SSE, and an HTTP/1.0 client that cannot read chunked
        // encoding, get plain JSON; the notification is dropped.
        co_await json_only_client.connect(20432);
        co_await json_only_client.send(modern_request_bytes(call_tool(2, "log"), false));
        co_await json_only_client.read_until(R"("logged":true)");
        co_await http10_client.connect(20432);
        co_await http10_client.send(modern_request_bytes(call_tool(3, "log"), true, 10));
        co_await http10_client.read_until(R"("logged":true)");
    });

    for (const auto* client : {&echo_client, &json_only_client, &http10_client}) {
        const auto& text = client->received();
        EXPECT_NE(text.find("Content-Type: application/json"), std::string::npos) << text;
        EXPECT_NE(text.find("Content-Length:"), std::string::npos) << text;
        EXPECT_EQ(text.find("notifications/message"), std::string::npos) << text;
    }
    EXPECT_EQ(manager->stream_stats().streams_opened, 0u);
}

TEST_F(HttpStreamTest, ResetBeforeAnythingIsWrittenCancelsTheHandler) {
    auto manager = make_manager(20433);
    RawClient client(io_ctx_.get_executor());
    run(*manager, [&]() -> mcp::Task<void> {
        co_await client.connect(20433);
        co_await client.send(modern_request_bytes(call_tool(1, "wait_for_cancel")));
        co_await wait_until([this] { return handler_started_.load(); });
        client.reset();
        co_await wait_until([this] { return handler_done_.load(); });
    });
    EXPECT_TRUE(handler_saw_cancel_.load());
    EXPECT_EQ(manager->stream_stats().disconnect_cancels, 1u);
}

TEST_F(HttpStreamTest, ResetMidStreamCancelsTheHandler) {
    auto manager = make_manager(20434);
    RawClient client(io_ctx_.get_executor());
    bool saw_log = false;
    run(*manager, [&]() -> mcp::Task<void> {
        co_await client.connect(20434);
        co_await client.send(modern_request_bytes(call_tool(1, "log_then_wait_for_cancel")));
        saw_log = co_await client.read_until(log_line("first"));
        client.reset();
        co_await wait_until([this] { return handler_done_.load(); });
    });
    EXPECT_TRUE(saw_log) << client.received();
    EXPECT_TRUE(handler_saw_cancel_.load());
    EXPECT_EQ(manager->stream_stats().disconnect_cancels, 1u);
}

TEST_F(HttpStreamTest, HalfCloseIsProbedAndDoesNotCancel) {
    auto manager = make_manager(20435);
    manager->set_stream_options({std::chrono::milliseconds(15000), std::chrono::milliseconds(5), 4096});
    RawClient client(io_ctx_.get_executor());
    bool saw_end = false;
    run(*manager, [&]() -> mcp::Task<void> {
        co_await client.connect(20435);
        co_await client.send(modern_request_bytes(call_tool(1, "log_then_wait")));
        client.shutdown_send();
        co_await wait_until([&] { return manager->stream_stats().half_close_probes >= 2; });
        released_.store(true);
        saw_end = co_await client.read_until("0\r\n\r\n");
    });
    ASSERT_TRUE(saw_end) << client.received();
    EXPECT_NE(client.received().find(": probe"), std::string::npos) << client.received();
    EXPECT_NE(client.received().find(R"("released":true)"), std::string::npos) << client.received();
    EXPECT_EQ(manager->stream_stats().disconnect_cancels, 0u);
}

TEST_F(HttpStreamTest, CloseAfterHalfCloseIsDetectedByTheProbes) {
    auto manager = make_manager(20436);
    manager->set_stream_options({std::chrono::milliseconds(15000), std::chrono::milliseconds(5), 4096});
    RawClient client(io_ctx_.get_executor());
    run(*manager, [&]() -> mcp::Task<void> {
        co_await client.connect(20436);
        co_await client.send(modern_request_bytes(call_tool(1, "wait_for_cancel")));
        co_await wait_until([this] { return handler_started_.load(); });
        client.close();  // a FIN the server cannot tell from a half-close until a write fails
        co_await wait_until([this] { return handler_done_.load(); });
    });
    EXPECT_TRUE(handler_saw_cancel_.load());
    EXPECT_EQ(manager->stream_stats().disconnect_cancels, 1u);
    EXPECT_GE(manager->stream_stats().half_close_probes, 1u);
}

TEST_F(HttpStreamTest, PipelinedRequestDuringAStreamIsServedNext) {
    auto manager = make_manager(20437);
    RawClient client(io_ctx_.get_executor());
    bool saw_second = false;
    run(*manager, [&]() -> mcp::Task<void> {
        co_await client.connect(20437);
        co_await client.send(modern_request_bytes(call_tool(1, "log_then_wait")) +
                             modern_request_bytes(call_tool(2, "echo")));
        co_await client.read_until(log_line("first"));
        released_.store(true);
        co_await client.read_until("0\r\n\r\n");
        saw_second = co_await client.read_until(R"("id":2)");
    });
    ASSERT_TRUE(saw_second) << client.received();
    const auto& text = client.received();
    EXPECT_LT(text.find(R"("released":true)"), text.find(R"("id":2)")) << text;
}

TEST_F(HttpStreamTest, ConnectionIsReusedAfterAStream) {
    auto manager = make_manager(20438);
    // The response is ready while the notification is still being written; a wake lost then
    // would leave the stream waiting for the next keep-alive, which here never comes.
    manager->set_stream_options({std::chrono::hours(1), std::chrono::milliseconds(1000), 4096});
    RawClient client(io_ctx_.get_executor());
    bool saw_second = false;
    run(*manager, [&]() -> mcp::Task<void> {
        co_await client.connect(20438);
        co_await client.send(modern_request_bytes(call_tool(1, "log")));
        co_await client.read_until("0\r\n\r\n");
        co_await client.send(modern_request_bytes(call_tool(2, "echo")));
        saw_second = co_await client.read_until(R"("id":2)");
    });
    EXPECT_TRUE(saw_second) << client.received();
}

TEST_F(HttpStreamTest, IdleStreamsCarryKeepAliveComments) {
    auto manager = make_manager(20439);
    manager->set_stream_options({std::chrono::milliseconds(5), std::chrono::milliseconds(1000), 4096});
    RawClient client(io_ctx_.get_executor());
    bool saw_comment = false;
    run(*manager, [&]() -> mcp::Task<void> {
        co_await client.connect(20439);
        co_await client.send(modern_request_bytes(call_tool(1, "log_then_wait")));
        saw_comment = co_await client.read_until(": keep-alive");
        released_.store(true);
        co_await client.read_until("0\r\n\r\n");
    });
    EXPECT_TRUE(saw_comment) << client.received();
}

TEST_F(HttpStreamTest, NotificationsBeyondTheQueueLimitAreDroppedAndCounted) {
    auto manager = make_manager(20440);
    manager->set_stream_options({std::chrono::milliseconds(15000), std::chrono::milliseconds(1000), 1});
    RawClient client(io_ctx_.get_executor());
    bool saw_end = false;
    run(*manager, [&]() -> mcp::Task<void> {
        co_await client.connect(20440);
        co_await client.send(modern_request_bytes(call_tool(1, "flood")));
        saw_end = co_await client.read_until("0\r\n\r\n");
    });
    ASSERT_TRUE(saw_end) << client.received();
    EXPECT_NE(client.received().find(R"("flooded":true)"), std::string::npos);
    EXPECT_GT(manager->stream_stats().dropped_notifications, 0u);
}

TEST_F(HttpStreamTest, ClientsSharingAnIdAreCancelledIndependently) {
    auto manager = make_manager(20441);
    RawClient leaving(io_ctx_.get_executor());
    RawClient staying(io_ctx_.get_executor());
    bool staying_answered = false;
    run(*manager, [&]() -> mcp::Task<void> {
        co_await leaving.connect(20441);
        co_await leaving.send(modern_request_bytes(call_tool(1, "wait_for_cancel")));
        co_await staying.connect(20441);
        co_await staying.send(modern_request_bytes(call_tool(1, "log_then_wait")));
        co_await staying.read_until(log_line("first"));
        leaving.reset();
        co_await wait_until([this] { return handlers_done_.load() == 1; });
        released_.store(true);
        staying_answered = co_await staying.read_until("0\r\n\r\n");
    });
    EXPECT_TRUE(handler_saw_cancel_.load());
    EXPECT_TRUE(staying_answered) << staying.received();
    EXPECT_NE(staying.received().find(R"("released":true)"), std::string::npos) << staying.received();
    EXPECT_EQ(manager->stream_stats().disconnect_cancels, 1u);
}

// A client that pipelines and then half-closes is answered in full, and the connection ends once
// nothing it sent is left, instead of waiting for an end of stream that was already read.
TEST_F(HttpStreamTest, PipelinedRequestsBeforeHalfCloseAreAllAnswered) {
    auto manager = make_manager(20444);
    RawClient client(io_ctx_.get_executor());
    bool ended = false;
    run(*manager, [&]() -> mcp::Task<void> {
        co_await client.connect(20444);
        co_await client.send(modern_request_bytes(call_tool(1, "echo")) +
                             modern_request_bytes(call_tool(2, "echo")));
        client.shutdown_send();
        ended = !co_await client.read_until("never sent");
    });
    EXPECT_TRUE(ended);
    const auto& text = client.received();
    EXPECT_NE(text.find(R"("id":1)"), std::string::npos) << text;
    EXPECT_NE(text.find(R"("id":2)"), std::string::npos) << text;
}

TEST_F(HttpStreamTest, TruncatedPipelinedRequestBeforeHalfCloseEndsTheConnection) {
    auto manager = make_manager(20445);
    RawClient client(io_ctx_.get_executor());
    bool ended = false;
    run(*manager, [&]() -> mcp::Task<void> {
        co_await client.connect(20445);
        co_await client.send(modern_request_bytes(call_tool(1, "echo")) +
                             modern_request_bytes(call_tool(2, "echo")).substr(0, 100));
        client.shutdown_send();
        ended = !co_await client.read_until("never sent");
    });
    EXPECT_TRUE(ended);
    EXPECT_NE(client.received().find(R"("id":1)"), std::string::npos) << client.received();
    EXPECT_EQ(client.received().find(R"("id":2)"), std::string::npos) << client.received();
}

// The first request is held while the server has every chance to read the second one and the
// end of the stream; the second is still answered, with or without a stream for the first.
class HttpStreamSlowFirstTest : public HttpStreamTest, public ::testing::WithParamInterface<bool> {};

TEST_P(HttpStreamSlowFirstTest, PipelinedThenHalfClosedAnswersBoth) {
    const bool accepts_sse = GetParam();
    const unsigned short port = accepts_sse ? 20446 : 20447;
    auto manager = make_manager(port);
    RawClient client(io_ctx_.get_executor());
    bool ended = false;
    run(*manager, [&]() -> mcp::Task<void> {
        co_await client.connect(port);
        co_await client.send(modern_request_bytes(call_tool(1, "wait"), accepts_sse) +
                             modern_request_bytes(call_tool(2, "echo"), accepts_sse));
        client.shutdown_send();
        co_await yield_turns(2000);
        released_.store(true);
        ended = !co_await client.read_until("never sent");
    });
    EXPECT_TRUE(ended);
    EXPECT_NE(client.received().find(R"("released":true)"), std::string::npos) << client.received();
    EXPECT_NE(client.received().find(R"("id":2)"), std::string::npos) << client.received();
}

INSTANTIATE_TEST_SUITE_P(SseAndJson, HttpStreamSlowFirstTest, ::testing::Values(true, false));

// After a half-close nothing more can be read, but closing the manager still cancels the request
// of a client that gets plain JSON.
TEST_F(HttpStreamTest, ClosingTheManagerCancelsARequestWhoseClientHalfClosed) {
    auto manager = make_manager(20448);
    RawClient client(io_ctx_.get_executor());
    asio::co_spawn(io_ctx_, manager->listen(), asio::detached);
    asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            co_await client.connect(20448);
            co_await client.send(modern_request_bytes(call_tool(1, "wait_for_cancel"), false));
            co_await wait_until([this] { return handler_started_.load(); });
            client.shutdown_send();
            co_await yield_turns(500);
            manager->close();
        },
        asio::detached);
    io_ctx_.run();
    EXPECT_TRUE(handler_saw_cancel_.load());
}

// A reset after a half-close cancels the request, for a client that gets plain JSON as well.
TEST_F(HttpStreamTest, ResetAfterHalfCloseCancelsTheRequest) {
    auto manager = make_manager(20449);
    RawClient client(io_ctx_.get_executor());
    run(*manager, [&]() -> mcp::Task<void> {
        co_await client.connect(20449);
        co_await client.send(modern_request_bytes(call_tool(1, "wait_for_cancel"), false));
        co_await wait_until([this] { return handler_started_.load(); });
        client.shutdown_send();
        co_await yield_turns(2000);
        client.reset();
        co_await wait_until([this] { return handler_done_.load(); });
    });
    EXPECT_TRUE(handler_saw_cancel_.load());
    EXPECT_EQ(manager->stream_stats().disconnect_cancels, 1u);
}

// The modern server may also serve a session on another transport. A 2026-07-28 request that came
// over HTTP without a stream drops its notifications rather than sending them to that session.
TEST_F(HttpStreamTest, NotificationsStayOffAnotherTransportOfTheSharedServer) {
    std::shared_ptr<mcp::Server> server = make_server();
    auto [client_end, server_end] = mcp::create_memory_transport_pair(io_ctx_.get_executor());
    mcp::StreamableHttpSessionManager manager(
        io_ctx_.get_executor(), "127.0.0.1", 20450,
        [](const asio::any_io_executor&) -> std::unique_ptr<mcp::Server> { return nullptr; });
    manager.set_modern_server(server);
    std::vector<std::string> on_other_transport;
    RawClient client(io_ctx_.get_executor());
    asio::co_spawn(io_ctx_, server->run(server_end, io_ctx_.get_executor()), asio::detached);
    asio::co_spawn(io_ctx_, read_all(client_end, &on_other_transport), asio::detached);
    run(manager, [&]() -> mcp::Task<void> {
        co_await client.connect(20450);
        co_await client.send(modern_request_bytes(call_tool(1, "log"), false));
        co_await client.read_until(R"("logged":true)");
        // A notification written to the memory pair would arrive within these turns.
        co_await yield_turns(200);
        client_end->close();
    });
    EXPECT_NE(client.received().find(R"("logged":true)"), std::string::npos) << client.received();
    EXPECT_TRUE(on_other_transport.empty()) << on_other_transport.front();
}

TEST(HttpStreamOptions, InvalidOptionsAreRefused) {
    asio::io_context io_ctx;
    mcp::StreamableHttpSessionManager manager(
        io_ctx.get_executor(), "127.0.0.1", 20442,
        [](const asio::any_io_executor&) -> std::unique_ptr<mcp::Server> { return nullptr; });
    EXPECT_THROW(
        manager.set_stream_options({std::chrono::milliseconds(0), std::chrono::milliseconds(1), 1}),
        std::invalid_argument);
    EXPECT_THROW(
        manager.set_stream_options({std::chrono::milliseconds(1), std::chrono::milliseconds(-1), 1}),
        std::invalid_argument);
    EXPECT_THROW(
        manager.set_stream_options({std::chrono::milliseconds(1), std::chrono::milliseconds(1), 0}),
        std::invalid_argument);
    EXPECT_NO_THROW(manager.set_stream_options({}));
}

}  // namespace

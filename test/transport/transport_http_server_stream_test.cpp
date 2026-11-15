#include "../support/hang_guard.hpp"
#include "../test_utils.hpp"

#include "../support/spawn.hpp"
#include "mcp/server/server.hpp"
#include "mcp/transport/http_server.hpp"

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
#include <exception>
#include <functional>
#include <mcp/core/constants.hpp>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace transport_http_server_stream_test {

namespace jsonrpc = mcp::constants::jsonrpc;
namespace protocol = mcp::constants::protocol;

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using nlohmann::json;
using tcp = asio::ip::tcp;

/// Waits, a millisecond at a time, until @p condition holds. Gives up after g_hang_guard so a broken
/// stream fails the test instead of hanging the run.
mcp::Task<bool> wait_until(std::function<bool()> condition) {
    auto executor = co_await asio::this_coro::executor;
    const auto give_up = std::chrono::steady_clock::now() + g_hang_guard;
    while (!condition() && std::chrono::steady_clock::now() < give_up) {
        asio::steady_timer timer(executor, std::chrono::milliseconds(1));
        co_await timer.async_wait(asio::use_awaitable);
    }
    co_return condition();
}

/// Lets the executor run @p turns other handlers first.
mcp::Task<void> yield_turns(int turns) {
    auto executor = co_await asio::this_coro::executor;
    for (int turn = 0; turn < turns; ++turn) {
        co_await asio::post(executor, asio::use_awaitable);
    }
}

/// The value at JSON pointer @p pointer, or null when the message lacks it.
json field(const json& message, const char* pointer) {
    return message.value(json::json_pointer(pointer), json());
}

/// The subscription id a notification names, or null.
json subscription_of(const json& message) {
    return field(message, "/params/_meta/io.modelcontextprotocol~1subscriptionId");
}

bool is_method(const json& message, std::string_view method) {
    return message.value("method", "") == method;
}

/// The bytes of an HTTP request carrying @p body as a 2026-07-28 client sends it.
std::string modern_request_bytes(const json& body, bool accepts_sse = true, unsigned version = 11,
                                 const std::string& origin = {}) {
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
    if (!origin.empty()) {
        request.set(http::field::origin, origin);
    }
    request.keep_alive(true);
    request.body() = body.dump();
    request.prepare_payload();
    std::ostringstream bytes;
    bytes << request;
    return bytes.str();
}

/// A tools/call whose handler may log at any level.
json call_tool(const json& id, const std::string& name) {
    auto body = make_modern_request(id, "tools/call", {{"name", name}, {"arguments", json::object()}});
    body["params"]["_meta"]["io.modelcontextprotocol/logLevel"] = "debug";
    return body;
}

json listen_request(const json& id, json filter) {
    return make_modern_request(id, "subscriptions/listen", {{"notifications", std::move(filter)}});
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
    /// @p text is json so that a short one is not an SSO std::string in the coroutine frame.
    mcp::Task<bool> read_until(json text) {
        while (received_.find(text.get_ref<const std::string&>()) == std::string::npos) {
            const bool more = co_await read_more();
            if (!more) {
                co_return false;
            }
        }
        co_return true;
    }

    /// Reads until @p count SSE events have arrived or the connection ends.
    mcp::Task<bool> read_events(std::size_t count) {
        while (events().size() < count) {
            const bool more = co_await read_more();
            if (!more) {
                co_return false;
            }
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

    /// The JSON messages of the `data:` lines received so far.
    [[nodiscard]] std::vector<json> events() const {
        std::vector<json> messages;
        std::istringstream lines(received_);
        std::string line;
        while (std::getline(lines, line)) {
            if (line.rfind("data: ", 0) == 0) {
                messages.push_back(json::parse(line.substr(6)));
            }
        }
        return messages;
    }

   private:
    mcp::Task<bool> read_more() {
        std::array<char, 4096> chunk{};
        boost::system::error_code error;
        const auto bytes = co_await socket_.async_read_some(
            asio::buffer(chunk), asio::redirect_error(asio::use_awaitable, error));
        if (error) {
            co_return false;
        }
        received_.append(chunk.data(), bytes);
        co_return true;
    }

    tcp::socket socket_;
    std::string received_;
};

struct LegacyReply {
    unsigned int status{0};
    std::string body;
    std::string session_id;
};

/// Sends one legacy POST that accepts JSON only and reads its whole answer.
mcp::Task<LegacyReply> post_legacy(asio::any_io_executor executor, unsigned short port, json body,
                                   std::string session_id) {
    beast::tcp_stream stream(executor);
    co_await stream.async_connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), port),
                                  asio::use_awaitable);
    http::request<http::string_body> request{http::verb::post, "/mcp", 11};
    request.set(http::field::host, "127.0.0.1");
    request.set(http::field::content_type, "application/json");
    request.set(http::field::accept, "application/json");
    if (!session_id.empty()) {
        request.set("MCP-Session-Id", session_id);
        request.set("MCP-Protocol-Version", std::string(protocol::g_LATEST_LEGACY_PROTOCOL_VERSION));
    }
    request.body() = body.dump();
    request.prepare_payload();
    co_await http::async_write(stream, request, asio::use_awaitable);
    beast::flat_buffer buffer;
    http::response<http::string_body> response;
    co_await http::async_read(stream, buffer, response, asio::use_awaitable);
    LegacyReply reply;
    reply.status = response.result_int();
    reply.body = response.body();
    if (auto session = response.find("MCP-Session-Id"); session != response.end()) {
        reply.session_id = std::string(session->value());
    }
    boost::system::error_code ignored;
    stream.socket().shutdown(tcp::socket::shutdown_both, ignored);
    co_return reply;
}

class HttpServerStreamTest : public ::testing::Test {
   protected:
    asio::io_context io_ctx_;
    std::shared_ptr<mcp::HttpServerTransport> transport_ =
        std::make_shared<mcp::HttpServerTransport>(io_ctx_.get_executor(), "127.0.0.1", 0);
    std::unique_ptr<mcp::Server> server_;
    std::atomic<bool> released_{false};
    std::atomic<bool> handler_started_{false};
    std::atomic<bool> handler_done_{false};
    std::atomic<bool> handler_saw_cancel_{false};
    std::atomic<int> handlers_done_{0};

    void SetUp() override {
        mcp::ServerCapabilities caps;
        caps.tools = mcp::ServerCapabilities::ToolsCapability{true};
        caps.prompts = mcp::ServerCapabilities::PromptsCapability{true};
        caps.resources = mcp::ServerCapabilities::ResourcesCapability{true, true};
        caps.logging = json::object();
        server_ = std::make_unique<mcp::Server>(mcp::Implementation{"http-stream", "1.0"}, caps);
        server_->add_tool<json, json>("echo", "Echoes", json{{"type", "object"}},
                                      [](json args) -> mcp::Task<json> { co_return args; });
        server_->add_tool<json, json>(
            "log_then_wait", "Logs, then returns once released", json{{"type", "object"}},
            [this](mcp::Context& ctx, json /*args*/) -> mcp::Task<json> {
                handler_started_.store(true);
                co_await ctx.log(mcp::LoggingLevel::eError, "first");
                const bool released = co_await wait_until([this] { return released_.load(); });
                handler_done_.store(true);
                co_return json{{"released", released}};
            });
        server_->add_tool<json, json>("log", "Logs once", json{{"type", "object"}},
                                      [](mcp::Context& ctx, json /*args*/) -> mcp::Task<json> {
                                          co_await ctx.log(mcp::LoggingLevel::eError, "only");
                                          co_return json{{"logged", true}};
                                      });
        server_->add_tool<json, json>("flood", "Logs 100 times", json{{"type", "object"}},
                                      [](mcp::Context& ctx, json /*args*/) -> mcp::Task<json> {
                                          for (int index = 0; index < 100; ++index) {
                                              co_await ctx.log(mcp::LoggingLevel::eError, "flood");
                                          }
                                          co_return json{{"flooded", true}};
                                      });
        server_->add_tool<json, json>("progress", "Reports progress", json{{"type", "object"}},
                                      [](mcp::Context& ctx, json /*args*/) -> mcp::Task<json> {
                                          co_await ctx.report_progress(1, 2, "halfway");
                                          co_return json{{"done", true}};
                                      });
        server_->add_tool<json, json>(
            "log_then_wait_for_cancel", "Logs, then returns once cancelled", json{{"type", "object"}},
            [this](mcp::Context& ctx, json /*args*/) -> mcp::Task<json> {
                co_await ctx.log(mcp::LoggingLevel::eError, "first");
                const bool cancelled = co_await wait_until([&ctx] { return ctx.is_cancelled(); });
                handler_saw_cancel_.store(cancelled);
                handler_done_.store(true);
                co_return json{{"cancelled", cancelled}};
            });
        server_->add_tool<json, json>(
            "log_big_until_cancelled", "Logs large messages until cancelled", json{{"type", "object"}},
            [this](mcp::Context& ctx, json /*args*/) -> mcp::Task<json> {
                handler_started_.store(true);
                const std::string text(65536, 'x');
                auto executor = co_await asio::this_coro::executor;
                while (!ctx.is_cancelled()) {
                    co_await ctx.log(mcp::LoggingLevel::eError, text);
                    asio::steady_timer pause(executor, std::chrono::milliseconds(1));
                    co_await pause.async_wait(asio::use_awaitable);
                }
                handler_saw_cancel_.store(true);
                handler_done_.store(true);
                co_return json::object();
            });
        server_->add_tool<json, json>("log_large", "Logs ten messages of 64 KiB",
                                      json{{"type", "object"}},
                                      [](mcp::Context& ctx, json /*args*/) -> mcp::Task<json> {
                                          const std::string text(65536, 'x');
                                          for (int index = 0; index < 10; ++index) {
                                              co_await ctx.log(mcp::LoggingLevel::eError, text);
                                          }
                                          co_return json{{"logged", true}};
                                      });
        server_->add_tool<json, json>(
            "wait_released", "Returns once released, without notifying", json{{"type", "object"}},
            [this](json /*args*/) -> mcp::Task<json> {
                handler_started_.store(true);
                const bool released = co_await wait_until([this] { return released_.load(); });
                co_return json{{"released", released}};
            });
        server_->add_tool<json, json>(
            "wait_for_cancel", "Returns once cancelled", json{{"type", "object"}},
            [this](mcp::Context& ctx, json /*args*/) -> mcp::Task<json> {
                handler_started_.store(true);
                const bool cancelled = co_await wait_until([&ctx] { return ctx.is_cancelled(); });
                handler_saw_cancel_.store(cancelled);
                handlers_done_.fetch_add(1);
                handler_done_.store(true);
                co_return json{{"cancelled", cancelled}};
            });
    }

    static mcp::Task<void> serve(mcp::Server* server, std::shared_ptr<mcp::ITransport> transport,
                                 asio::any_io_executor executor) {
        co_await server->run(std::move(transport), std::move(executor));
    }

    /// Serves the transport, runs @p exchange, then closes the transport and lets everything end.
    template <typename Exchange>
    void run(Exchange exchange) {
        mcp_test::spawn_detached(io_ctx_, transport_->listen());
        mcp_test::spawn_detached(io_ctx_, serve(server_.get(), transport_, io_ctx_.get_executor()));
        mcp_test::spawn_detached(io_ctx_,
                                 [this, exchange = std::move(exchange)]() mutable -> mcp::Task<void> {
                                     co_await exchange();
                                     transport_->close();
                                 });
        io_ctx_.run();
    }

    static std::string log_line(std::string_view text) {
        return R"("data":")" + std::string(text) + "\"";
    }
};

TEST_F(HttpServerStreamTest, NotificationIsDeliveredBeforeTheHandlerFinishes) {
    RawClient client(io_ctx_.get_executor());
    bool saw_log = false;
    bool saw_end = false;
    run([&]() -> mcp::Task<void> {
        co_await client.connect(transport_->port());
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
    EXPECT_EQ(transport_->stream_stats().streams_opened, 1u);
}

TEST_F(HttpServerStreamTest, ProgressIsStreamedAheadOfTheResult) {
    RawClient client(io_ctx_.get_executor());
    bool saw_end = false;
    run([&]() -> mcp::Task<void> {
        auto body = call_tool(1, "progress");
        body["params"]["_meta"]["progressToken"] = "p1";
        co_await client.connect(transport_->port());
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

TEST_F(HttpServerStreamTest, AnswersWithoutNotificationsStayPlainJson) {
    RawClient echo_client(io_ctx_.get_executor());
    RawClient json_only_client(io_ctx_.get_executor());
    RawClient http10_client(io_ctx_.get_executor());
    run([&]() -> mcp::Task<void> {
        co_await echo_client.connect(transport_->port());
        co_await echo_client.send(modern_request_bytes(call_tool(1, "echo")));
        co_await echo_client.read_until(R"("resultType":"complete")");
        // A client that does not accept SSE, and an HTTP/1.0 client that cannot read chunked
        // encoding, get plain JSON; the notification is dropped.
        co_await json_only_client.connect(transport_->port());
        co_await json_only_client.send(modern_request_bytes(call_tool(2, "log"), false));
        co_await json_only_client.read_until(R"("logged":true)");
        co_await http10_client.connect(transport_->port());
        co_await http10_client.send(modern_request_bytes(call_tool(3, "log"), true, 10));
        co_await http10_client.read_until(R"("logged":true)");
    });

    for (const auto* client : {&echo_client, &json_only_client, &http10_client}) {
        const auto& text = client->received();
        EXPECT_NE(text.find("Content-Type: application/json"), std::string::npos) << text;
        EXPECT_NE(text.find("Content-Length:"), std::string::npos) << text;
        EXPECT_EQ(text.find("notifications/message"), std::string::npos) << text;
    }
    EXPECT_EQ(transport_->stream_stats().streams_opened, 0u);
}

TEST_F(HttpServerStreamTest, JsonOnlyTransportNeverStreams) {
    transport_->set_json_only(true);
    RawClient client(io_ctx_.get_executor());
    run([&]() -> mcp::Task<void> {
        co_await client.connect(transport_->port());
        co_await client.send(modern_request_bytes(call_tool(1, "log")));
        co_await client.read_until(R"("logged":true)");
    });
    const auto& text = client.received();
    EXPECT_NE(text.find("Content-Type: application/json"), std::string::npos) << text;
    EXPECT_EQ(text.find("text/event-stream"), std::string::npos) << text;
    EXPECT_EQ(transport_->stream_stats().streams_opened, 0u);
}

TEST_F(HttpServerStreamTest, ResetBeforeAnythingIsWrittenCancelsTheHandler) {
    RawClient client(io_ctx_.get_executor());
    run([&]() -> mcp::Task<void> {
        co_await client.connect(transport_->port());
        co_await client.send(modern_request_bytes(call_tool(1, "wait_for_cancel")));
        co_await wait_until([this] { return handler_started_.load(); });
        client.reset();
        co_await wait_until([this] { return handler_done_.load(); });
    });
    EXPECT_TRUE(handler_saw_cancel_.load());
    EXPECT_EQ(transport_->stream_stats().disconnect_cancels, 1u);
}

TEST_F(HttpServerStreamTest, ResetMidStreamCancelsTheHandler) {
    RawClient client(io_ctx_.get_executor());
    bool saw_log = false;
    run([&]() -> mcp::Task<void> {
        co_await client.connect(transport_->port());
        co_await client.send(modern_request_bytes(call_tool(1, "log_then_wait_for_cancel")));
        saw_log = co_await client.read_until(log_line("first"));
        client.reset();
        co_await wait_until([this] { return handler_done_.load(); });
    });
    EXPECT_TRUE(saw_log) << client.received();
    EXPECT_TRUE(handler_saw_cancel_.load());
    EXPECT_EQ(transport_->stream_stats().disconnect_cancels, 1u);
}

TEST_F(HttpServerStreamTest, HalfCloseIsProbedAndDoesNotCancel) {
    transport_->set_stream_options(
        {std::chrono::milliseconds(15000), std::chrono::milliseconds(5), 4096});
    RawClient client(io_ctx_.get_executor());
    bool saw_end = false;
    run([&]() -> mcp::Task<void> {
        co_await client.connect(transport_->port());
        co_await client.send(modern_request_bytes(call_tool(1, "log_then_wait")));
        client.shutdown_send();
        co_await wait_until([&] { return transport_->stream_stats().half_close_probes >= 2; });
        released_.store(true);
        saw_end = co_await client.read_until("0\r\n\r\n");
    });
    ASSERT_TRUE(saw_end) << client.received();
    EXPECT_NE(client.received().find(": probe"), std::string::npos) << client.received();
    EXPECT_NE(client.received().find(R"("released":true)"), std::string::npos) << client.received();
    EXPECT_EQ(transport_->stream_stats().disconnect_cancels, 0u);
}

TEST_F(HttpServerStreamTest, CloseAfterHalfCloseIsDetectedByTheProbes) {
    transport_->set_stream_options(
        {std::chrono::milliseconds(15000), std::chrono::milliseconds(5), 4096});
    RawClient client(io_ctx_.get_executor());
    run([&]() -> mcp::Task<void> {
        co_await client.connect(transport_->port());
        co_await client.send(modern_request_bytes(call_tool(1, "wait_for_cancel")));
        co_await wait_until([this] { return handler_started_.load(); });
        client.close();  // a FIN the server cannot tell from a half-close until a write fails
        co_await wait_until([this] { return handler_done_.load(); });
    });
    EXPECT_TRUE(handler_saw_cancel_.load());
    EXPECT_EQ(transport_->stream_stats().disconnect_cancels, 1u);
    EXPECT_GE(transport_->stream_stats().half_close_probes, 1u);
}

TEST_F(HttpServerStreamTest, ClientsSharingAnIdAreCancelledIndependently) {
    RawClient leaving(io_ctx_.get_executor());
    RawClient staying(io_ctx_.get_executor());
    bool staying_answered = false;
    run([&]() -> mcp::Task<void> {
        co_await leaving.connect(transport_->port());
        co_await leaving.send(modern_request_bytes(call_tool(1, "wait_for_cancel")));
        co_await staying.connect(transport_->port());
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
    EXPECT_EQ(transport_->stream_stats().disconnect_cancels, 1u);
}

TEST_F(HttpServerStreamTest, PipelinedRequestDuringAStreamIsServedNext) {
    RawClient client(io_ctx_.get_executor());
    bool saw_second = false;
    run([&]() -> mcp::Task<void> {
        co_await client.connect(transport_->port());
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

TEST_F(HttpServerStreamTest, ConnectionIsReusedAfterAStream) {
    // The response is ready while the notification is still being written; a wake lost then
    // would leave the stream waiting for the next keep-alive, which here never comes.
    transport_->set_stream_options({std::chrono::hours(1), std::chrono::milliseconds(1000), 4096});
    RawClient client(io_ctx_.get_executor());
    bool saw_second = false;
    run([&]() -> mcp::Task<void> {
        co_await client.connect(transport_->port());
        co_await client.send(modern_request_bytes(call_tool(1, "log")));
        co_await client.read_until("0\r\n\r\n");
        co_await client.send(modern_request_bytes(call_tool(2, "echo")));
        saw_second = co_await client.read_until(R"("id":2)");
    });
    EXPECT_TRUE(saw_second) << client.received();
}

TEST_F(HttpServerStreamTest, IdleStreamsCarryKeepAliveComments) {
    transport_->set_stream_options(
        {std::chrono::milliseconds(5), std::chrono::milliseconds(1000), 4096});
    RawClient client(io_ctx_.get_executor());
    bool saw_comment = false;
    run([&]() -> mcp::Task<void> {
        co_await client.connect(transport_->port());
        co_await client.send(modern_request_bytes(call_tool(1, "log_then_wait")));
        saw_comment = co_await client.read_until(": keep-alive");
        released_.store(true);
        co_await client.read_until("0\r\n\r\n");
    });
    EXPECT_TRUE(saw_comment) << client.received();
}

TEST_F(HttpServerStreamTest, NotificationsBeyondTheQueueLimitAreDroppedAndCounted) {
    transport_->set_stream_options(
        {std::chrono::milliseconds(15000), std::chrono::milliseconds(1000), 1});
    RawClient client(io_ctx_.get_executor());
    bool saw_end = false;
    run([&]() -> mcp::Task<void> {
        co_await client.connect(transport_->port());
        co_await client.send(modern_request_bytes(call_tool(1, "flood")));
        saw_end = co_await client.read_until("0\r\n\r\n");
    });
    ASSERT_TRUE(saw_end) << client.received();
    EXPECT_NE(client.received().find(R"("flooded":true)"), std::string::npos);
    EXPECT_GT(transport_->stream_stats().dropped_notifications, 0u);
}

// A client that does not read what is written to it cannot keep its request: a write that takes
// longer than the request timeout ends the connection, which cancels the request.
TEST_F(HttpServerStreamTest, AClientThatDoesNotReadItsStreamLosesIt) {
    mcp::HttpConnectionLimits limits;
    limits.request_read_timeout = std::chrono::milliseconds(50);
    transport_->set_connection_limits(limits);
    RawClient client(io_ctx_.get_executor());
    run([&]() -> mcp::Task<void> {
        co_await client.connect(transport_->port());
        co_await client.send(modern_request_bytes(call_tool(1, "log_big_until_cancelled")));
        co_await wait_until([this] { return handler_done_.load(); });
    });
    EXPECT_TRUE(handler_saw_cancel_.load());
    EXPECT_EQ(transport_->stream_stats().disconnect_cancels, 1u);
}

// A half-closed client that does not accept a stream gets plain JSON, with no stream to probe.
TEST_F(HttpServerStreamTest, HalfClosedClientNotAcceptingStreamsGetsAPlainAnswer) {
    transport_->set_stream_options(
        {std::chrono::milliseconds(15000), std::chrono::milliseconds(5), 4096});
    RawClient client(io_ctx_.get_executor());
    bool saw_result = false;
    run([&]() -> mcp::Task<void> {
        co_await client.connect(transport_->port());
        co_await client.send(modern_request_bytes(call_tool(1, "wait_released"), false));
        client.shutdown_send();
        co_await wait_until([this] { return handler_started_.load(); });
        auto executor = co_await asio::this_coro::executor;
        asio::steady_timer pause(executor, std::chrono::milliseconds(50));
        co_await pause.async_wait(asio::use_awaitable);
        released_.store(true);
        saw_result = co_await client.read_until(R"("released":true)");
    });
    ASSERT_TRUE(saw_result) << client.received();
    EXPECT_EQ(client.received().rfind("HTTP/1.1 200", 0), 0u) << client.received();
    EXPECT_NE(client.received().find("application/json"), std::string::npos) << client.received();
    EXPECT_EQ(client.received().find("text/event-stream"), std::string::npos) << client.received();
    EXPECT_EQ(client.received().find(": probe"), std::string::npos) << client.received();
    EXPECT_EQ(transport_->stream_stats().streams_opened, 0u);
    EXPECT_EQ(transport_->stream_stats().half_close_probes, 0u);
}

// A reset after a half-close is found by asking the socket, for a client that accepts no stream.
TEST_F(HttpServerStreamTest, ResetAfterHalfCloseCancelsAPlainAnswer) {
    transport_->set_stream_options(
        {std::chrono::milliseconds(15000), std::chrono::milliseconds(5), 4096});
    RawClient client(io_ctx_.get_executor());
    run([&]() -> mcp::Task<void> {
        co_await client.connect(transport_->port());
        co_await client.send(modern_request_bytes(call_tool(1, "wait_for_cancel"), false));
        client.shutdown_send();
        co_await wait_until([this] { return handler_started_.load(); });
        client.reset();
#if defined(_WIN32)
        // A Windows client that half-closed sends no reset when it aborts (see
        // HttpStreamTest.ResetAfterHalfCloseCancelsTheRequest): the request keeps running until
        // the transport closes.
        const auto checked = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
        co_await wait_until([checked] { return std::chrono::steady_clock::now() >= checked; });
        EXPECT_FALSE(handler_done_.load());
        transport_->close();
#endif
        co_await wait_until([this] { return handler_done_.load(); });
    });
    EXPECT_TRUE(handler_saw_cancel_.load());
    EXPECT_EQ(transport_->stream_stats().disconnect_cancels, 1u);
    EXPECT_EQ(transport_->stream_stats().streams_opened, 0u);
    EXPECT_EQ(transport_->stream_stats().half_close_probes, 0u);
}

// Notifications are held to a byte limit as well as a count: past it they are dropped and
// counted, and the request is answered as usual.
TEST_F(HttpServerStreamTest, NotificationsBeyondTheByteLimitAreDroppedAndCounted) {
    transport_->set_stream_options(
        {std::chrono::milliseconds(15000), std::chrono::milliseconds(1000), 4096, 200000});
    RawClient client(io_ctx_.get_executor());
    bool saw_end = false;
    run([&]() -> mcp::Task<void> {
        co_await client.connect(transport_->port());
        co_await client.send(modern_request_bytes(call_tool(1, "log_large")));
        saw_end = co_await client.read_until("0\r\n\r\n");
    });
    ASSERT_TRUE(saw_end) << client.received().substr(0, 2000);
    std::size_t delivered = 0;
    for (const auto& event : client.events()) {
        if (is_method(event, "notifications/message")) {
            ++delivered;
        }
    }
    EXPECT_NE(client.received().find(R"("logged":true)"), std::string::npos);
    EXPECT_GT(delivered, 0u);
    EXPECT_LE(delivered, 3u);
    EXPECT_EQ(transport_->stream_stats().dropped_notifications, 10u - delivered);
}

// A client that sends a request and never reads the answer cannot make the server queue more than
// the byte limit of notifications for it.
TEST_F(HttpServerStreamTest, ANonReadingClientCannotQueueMoreThanTheByteLimit) {
    transport_->set_stream_options(
        {std::chrono::milliseconds(15000), std::chrono::milliseconds(1000), 4096, 262144});
    RawClient client(io_ctx_.get_executor());
    run([&]() -> mcp::Task<void> {
        co_await client.connect(transport_->port());
        co_await client.send(modern_request_bytes(call_tool(1, "log_big_until_cancelled")));
        const bool dropping = co_await wait_until(
            [this] { return transport_->stream_stats().dropped_notifications > 0; });
        EXPECT_TRUE(dropping);
        client.reset();
        co_await wait_until([this] { return handler_done_.load(); });
    });
    EXPECT_TRUE(handler_saw_cancel_.load());
    EXPECT_GT(transport_->stream_stats().dropped_notifications, 0u);
}

// Queued notifications are charged to the connection memory budget, and given back when the
// request ends.
TEST_F(HttpServerStreamTest, QueuedNotificationsAreChargedToTheMemoryBudget) {
    mcp::HttpConnectionLimits limits;
    limits.max_request_bytes = 150000;
    transport_->set_connection_limits(limits);
    std::vector<std::unique_ptr<RawClient>> attempts;
    std::size_t first_delivered = 0;
    std::size_t last_delivered = 0;
    run([&]() -> mcp::Task<void> {
        for (int attempt = 0; attempt < 10000; ++attempt) {
            auto next = std::make_unique<RawClient>(io_ctx_.get_executor());
            co_await next->connect(transport_->port());
            co_await next->send(modern_request_bytes(call_tool(1, "log_large")));
            co_await next->read_until("0\r\n\r\n");
            std::size_t delivered = 0;
            for (const auto& event : next->events()) {
                if (is_method(event, "notifications/message")) {
                    ++delivered;
                }
            }
            if (attempt == 0) {
                first_delivered = delivered;
            }
            last_delivered = delivered;
            attempts.push_back(std::move(next));
            // The budget is given back once the connection has finished with the first request:
            // a later request holds the same share.
            if (attempt > 0 && delivered > 0) {
                break;
            }
        }
    });
    ASSERT_FALSE(attempts.empty());
    EXPECT_GT(first_delivered, 0u) << attempts.front()->received().substr(0, 500);
    EXPECT_LE(first_delivered, 2u);
    EXPECT_GT(last_delivered, 0u);
    EXPECT_LE(last_delivered, 2u);
    EXPECT_GT(transport_->stream_stats().dropped_notifications, 0u);
}

// A subscription that loses a notification would leave its client unaware of the change, so the
// stream ends instead, with its closing result.
TEST_F(HttpServerStreamTest, ListenEndsWhenItsQueueOverflows) {
    transport_->set_stream_options(
        {std::chrono::milliseconds(15000), std::chrono::milliseconds(1000), 1});
    RawClient client(io_ctx_.get_executor());
    bool saw_end = false;
    const auto request = modern_request_bytes(listen_request(1, {{"toolsListChanged", true}}));
    run([&]() -> mcp::Task<void> {
        co_await client.connect(transport_->port());
        co_await client.send(request);
        co_await client.read_events(1);
        // Started together, every notification reaches the queue before the stream's writer can
        // take one, so the overflow does not depend on how the platform orders write completions.
        int notified = 0;
        for (int index = 0; index < 100; ++index) {
            mcp_test::spawn_then(io_ctx_, server_->notify_tools_list_changed(),
                                 [&notified](const std::exception_ptr&) { ++notified; });
        }
        saw_end = co_await client.read_until("0\r\n\r\n");
        const bool removed = co_await wait_until([&] { return server_->subscription_count() == 0; });
        EXPECT_TRUE(removed);
        const bool all_notified = co_await wait_until([&] { return notified == 100; });
        EXPECT_TRUE(all_notified);
    });
    ASSERT_TRUE(saw_end) << client.received();
    const auto events = client.events();
    ASSERT_FALSE(events.empty());
    EXPECT_EQ(events.back()["id"], 1) << client.received();
    EXPECT_EQ(field(events.back(), "/result/resultType"), "complete");
    EXPECT_LT(events.size(), 100u);
    EXPECT_GT(transport_->stream_stats().dropped_notifications, 0u);
}

TEST_F(HttpServerStreamTest, ClosingTheTransportCancelsARunningRequest) {
    RawClient client(io_ctx_.get_executor());
    run([&]() -> mcp::Task<void> {
        co_await client.connect(transport_->port());
        co_await client.send(modern_request_bytes(call_tool(1, "log_then_wait_for_cancel")));
        co_await client.read_until(log_line("first"));
    });
    // run() closed the transport with the request still going; its handler saw the cancellation.
    EXPECT_TRUE(handler_saw_cancel_.load());
}

// The listen request is an SSE stream: acknowledged first, then the notifications its filter asks
// for and no others, and it ends with the closing result when the server ends the subscription.
TEST_F(HttpServerStreamTest, ListenAcknowledgesDeliversFilteredNotificationsAndEnds) {
    RawClient client(io_ctx_.get_executor());
    bool saw_end = false;
    json filter = {{"toolsListChanged", true}};
    filter["resourceSubscriptions"] = json::array({"file:///a"});
    const auto request = modern_request_bytes(listen_request(1, filter));
    run([&]() -> mcp::Task<void> {
        co_await client.connect(transport_->port());
        co_await client.send(request);
        const bool acknowledged = co_await client.read_events(1);
        EXPECT_TRUE(acknowledged) << client.received();
        EXPECT_EQ(server_->subscription_count(), 1u);

        co_await server_->notify_prompts_list_changed();
        co_await server_->notify_tools_list_changed();
        co_await server_->notify_resource_updated("file:///other");
        co_await server_->notify_resource_updated("file:///a");
        const bool delivered = co_await client.read_events(3);
        EXPECT_TRUE(delivered) << client.received();
        // The stream is still open after the batch.
        EXPECT_EQ(client.received().find("0\r\n\r\n"), std::string::npos) << client.received();

        server_->close_subscriptions();
        saw_end = co_await client.read_until("0\r\n\r\n");
        const bool removed = co_await wait_until([&] { return server_->subscription_count() == 0; });
        EXPECT_TRUE(removed);
    });

    ASSERT_TRUE(saw_end) << client.received();
    const auto& text = client.received();
    EXPECT_NE(text.find("Content-Type: text/event-stream"), std::string::npos) << text;
    EXPECT_EQ(text.find("Mcp-Session-Id"), std::string::npos) << text;
    const auto events = client.events();
    ASSERT_EQ(events.size(), 4u) << text;
    EXPECT_TRUE(is_method(events[0], "notifications/subscriptions/acknowledged"));
    EXPECT_EQ(field(events[0], "/params/notifications"),
              (json{{"toolsListChanged", true}, {"resourceSubscriptions", {"file:///a"}}}));
    EXPECT_TRUE(is_method(events[1], "notifications/tools/list_changed"));
    EXPECT_TRUE(is_method(events[2], "notifications/resources/updated"));
    EXPECT_EQ(field(events[2], "/params/uri"), "file:///a");
    for (std::size_t index = 0; index < 3; ++index) {
        EXPECT_EQ(subscription_of(events[index]), 1) << events[index];
    }
    EXPECT_EQ(events[3]["id"], 1);
    EXPECT_EQ(field(events[3], "/result/resultType"), "complete");
    EXPECT_EQ(transport_->stream_stats().streams_opened, 1u);
}

// Two clients may use the same request id: each stream gets only what its own subscription asked
// for.
TEST_F(HttpServerStreamTest, TwoListenStreamsAreServedApart) {
    RawClient tools_client(io_ctx_.get_executor());
    RawClient prompts_client(io_ctx_.get_executor());
    const auto tools_request = modern_request_bytes(listen_request(1, {{"toolsListChanged", true}}));
    const auto prompts_request =
        modern_request_bytes(listen_request(1, {{"promptsListChanged", true}}));
    run([&]() -> mcp::Task<void> {
        co_await tools_client.connect(transport_->port());
        co_await tools_client.send(tools_request);
        co_await prompts_client.connect(transport_->port());
        co_await prompts_client.send(prompts_request);
        co_await tools_client.read_events(1);
        co_await prompts_client.read_events(1);
        EXPECT_EQ(server_->subscription_count(), 2u);

        co_await server_->notify_tools_list_changed();
        co_await server_->notify_prompts_list_changed();
        co_await server_->notify_tools_list_changed();
        co_await tools_client.read_events(3);
        // Ordered after the first change, so a tools change that leaked to this stream would
        // already have arrived.
        co_await prompts_client.read_events(2);
    });

    const auto tools_events = tools_client.events();
    ASSERT_EQ(tools_events.size(), 3u) << tools_client.received();
    EXPECT_TRUE(is_method(tools_events[1], "notifications/tools/list_changed"));
    EXPECT_TRUE(is_method(tools_events[2], "notifications/tools/list_changed"));
    const auto prompts_events = prompts_client.events();
    ASSERT_EQ(prompts_events.size(), 2u) << prompts_client.received();
    EXPECT_TRUE(is_method(prompts_events[1], "notifications/prompts/list_changed"));
}

// Closing the response stream cancels the request, which ends the subscription.
TEST_F(HttpServerStreamTest, ListenEndsWhenTheClientResets) {
    RawClient client(io_ctx_.get_executor());
    const auto request = modern_request_bytes(listen_request(1, {{"toolsListChanged", true}}));
    run([&]() -> mcp::Task<void> {
        co_await client.connect(transport_->port());
        co_await client.send(request);
        co_await client.read_events(1);
        EXPECT_EQ(server_->subscription_count(), 1u);
        client.reset();
        const bool removed = co_await wait_until([&] { return server_->subscription_count() == 0; });
        EXPECT_TRUE(removed);
    });
    EXPECT_EQ(transport_->stream_stats().disconnect_cancels, 1u);
}

// A client that closes its end without a reset looks like a half-close until a probe fails.
TEST_F(HttpServerStreamTest, ListenEndsWhenTheClientCloses) {
    transport_->set_stream_options(
        {std::chrono::milliseconds(15000), std::chrono::milliseconds(5), 4096});
    RawClient client(io_ctx_.get_executor());
    const auto request = modern_request_bytes(listen_request(1, {{"toolsListChanged", true}}));
    run([&]() -> mcp::Task<void> {
        co_await client.connect(transport_->port());
        co_await client.send(request);
        co_await client.read_events(1);
        client.close();
        const bool removed = co_await wait_until([&] { return server_->subscription_count() == 0; });
        EXPECT_TRUE(removed);
    });
    EXPECT_EQ(transport_->stream_stats().disconnect_cancels, 1u);
}

// Closing the transport ends the streams it carries, so the server stops serving.
TEST_F(HttpServerStreamTest, ClosingTheTransportEndsAnOpenListenStream) {
    RawClient client(io_ctx_.get_executor());
    const auto request = modern_request_bytes(listen_request(1, {{"toolsListChanged", true}}));
    run([&]() -> mcp::Task<void> {
        co_await client.connect(transport_->port());
        co_await client.send(request);
        co_await client.read_events(1);
        EXPECT_EQ(server_->subscription_count(), 1u);
    });
    EXPECT_EQ(server_->subscription_count(), 0u);
}

// The response to a listen is a stream; a client that does not accept one is refused.
TEST_F(HttpServerStreamTest, ListenFromAClientNotAcceptingStreamsIsRefused) {
    RawClient client(io_ctx_.get_executor());
    const auto request = modern_request_bytes(listen_request(1, {{"toolsListChanged", true}}), false);
    run([&]() -> mcp::Task<void> {
        co_await client.connect(transport_->port());
        co_await client.send(request);
        co_await client.read_until(R"("error")");
        EXPECT_EQ(server_->subscription_count(), 0u);
    });

    EXPECT_NE(client.received().find("Content-Type: application/json"), std::string::npos)
        << client.received();
    EXPECT_NE(client.received().find(std::to_string(jsonrpc::g_INVALID_REQUEST)), std::string::npos)
        << client.received();
}

// The legacy session keeps its own stream and its own replay history while modern requests stream.
TEST_F(HttpServerStreamTest, LegacySessionIsUnaffectedByStreamingModernRequests) {
    RawClient client(io_ctx_.get_executor());
    LegacyReply initialized;
    LegacyReply during;
    const json initialize = make_initialize_request("init");
    const json initialized_notification = make_initialized_notification();
    const json legacy_call = make_tool_call_request("t1", "echo", json{{"x", 1}});
    run([&]() -> mcp::Task<void> {
        initialized = co_await post_legacy(io_ctx_.get_executor(), transport_->port(), initialize, "");
        const std::string session_id = initialized.session_id;
        co_await post_legacy(io_ctx_.get_executor(), transport_->port(), initialized_notification,
                             session_id);
        const auto stored_before = transport_->event_store().size();

        co_await client.connect(transport_->port());
        co_await client.send(modern_request_bytes(call_tool(1, "log_then_wait")));
        co_await client.read_until(log_line("first"));
        during =
            co_await post_legacy(io_ctx_.get_executor(), transport_->port(), legacy_call, session_id);
        released_.store(true);
        co_await client.read_until("0\r\n\r\n");
        // Only the legacy response entered the replay store.
        EXPECT_EQ(transport_->event_store().size(), stored_before + 1);
    });

    EXPECT_EQ(initialized.status, 200u) << initialized.body;
    EXPECT_FALSE(initialized.session_id.empty());
    EXPECT_EQ(during.status, 200u) << during.body;
    EXPECT_NE(during.body.find(R"("id":"t1")"), std::string::npos) << during.body;
    EXPECT_EQ(client.received().find("Mcp-Session-Id"), std::string::npos) << client.received();
}

// A request on a stream holds one of the requests the connection limits allow until it ends.
TEST_F(HttpServerStreamTest, AStreamHoldsItsPlaceUnderTheRequestLimit) {
    mcp::HttpConnectionLimits limits;
    limits.max_requests = 1;
    transport_->set_connection_limits(limits);
    RawClient streaming(io_ctx_.get_executor());
    RawClient refused(io_ctx_.get_executor());
    std::vector<std::unique_ptr<RawClient>> attempts;
    run([&]() -> mcp::Task<void> {
        co_await streaming.connect(transport_->port());
        co_await streaming.send(modern_request_bytes(call_tool(1, "log_then_wait")));
        co_await streaming.read_until(log_line("first"));
        co_await refused.connect(transport_->port());
        co_await refused.send(modern_request_bytes(call_tool(2, "echo")));
        co_await refused.read_until("Too many requests");
        released_.store(true);
        co_await streaming.read_until("0\r\n\r\n");
        // The place is given back once the connection has finished with the stream: ask until a
        // request is accepted.
        for (int attempt = 0; attempt < 10000; ++attempt) {
            auto next = std::make_unique<RawClient>(io_ctx_.get_executor());
            co_await next->connect(transport_->port());
            co_await next->send(modern_request_bytes(call_tool(3, "echo")));
            co_await next->read_until("\r\n\r\n");
            const bool accepted = next->received().find("HTTP/1.1 200") != std::string::npos;
            if (accepted) {
                co_await next->read_until(R"("id":3)");
            }
            attempts.push_back(std::move(next));
            if (accepted) {
                break;
            }
        }
    });
    EXPECT_NE(refused.received().find("503"), std::string::npos) << refused.received();
    EXPECT_NE(refused.received().find("Too many requests"), std::string::npos) << refused.received();
    ASSERT_FALSE(attempts.empty());
    EXPECT_NE(attempts.back()->received().find(R"("id":3)"), std::string::npos)
        << attempts.back()->received();
}

// A request is held to the memory the connection limits give requests, whether or not its answer
// would stream.
TEST_F(HttpServerStreamTest, StreamingRequestsAreHeldToTheMemoryLimit) {
    mcp::HttpConnectionLimits limits;
    limits.max_request_bytes = 1;
    transport_->set_connection_limits(limits);
    RawClient client(io_ctx_.get_executor());
    run([&]() -> mcp::Task<void> {
        co_await client.connect(transport_->port());
        co_await client.send(modern_request_bytes(call_tool(1, "log")));
        co_await client.read_until("Server busy");
    });
    EXPECT_NE(client.received().find("503"), std::string::npos) << client.received();
    EXPECT_FALSE(handler_started_.load());
    EXPECT_EQ(transport_->stream_stats().streams_opened, 0u);
}

// The headers are checked before the body is read, and the mirrored ones before the request is
// dispatched, however the answer would be sent.
TEST_F(HttpServerStreamTest, HeaderChecksStillRefuseAStreamingRequest) {
    RawClient origin_client(io_ctx_.get_executor());
    RawClient mismatch_client(io_ctx_.get_executor());
    run([&]() -> mcp::Task<void> {
        co_await origin_client.connect(transport_->port());
        co_await origin_client.send(
            modern_request_bytes(call_tool(1, "log_then_wait"), true, 11, "http://elsewhere.test"));
        co_await origin_client.read_until("Origin not allowed");
        auto mismatched = modern_request_bytes(call_tool(2, "log_then_wait"));
        const auto at = mismatched.find("Mcp-Name: log_then_wait");
        mismatched.replace(at, std::string("Mcp-Name: log_then_wait").size(), "Mcp-Name: echo");
        co_await mismatch_client.connect(transport_->port());
        co_await mismatch_client.send(mismatched);
        co_await mismatch_client.read_until(R"("error")");
    });
    EXPECT_NE(origin_client.received().find("403"), std::string::npos) << origin_client.received();
    EXPECT_NE(mismatch_client.received().find("400"), std::string::npos) << mismatch_client.received();
    EXPECT_NE(mismatch_client.received().find(std::to_string(jsonrpc::g_HEADER_MISMATCH)),
              std::string::npos)
        << mismatch_client.received();
    EXPECT_EQ(transport_->header_mismatch_rejections(), 1u);
    EXPECT_FALSE(handler_started_.load());
    EXPECT_EQ(transport_->stream_stats().streams_opened, 0u);
}

TEST(HttpServerStreamOptions, InvalidOptionsAreRefused) {
    asio::io_context io_ctx;
    mcp::HttpServerTransport transport(io_ctx.get_executor(), "127.0.0.1", 0);
    EXPECT_THROW(
        transport.set_stream_options({std::chrono::milliseconds(0), std::chrono::milliseconds(1), 1}),
        std::invalid_argument);
    EXPECT_THROW(
        transport.set_stream_options({std::chrono::milliseconds(1), std::chrono::milliseconds(-1), 1}),
        std::invalid_argument);
    EXPECT_THROW(
        transport.set_stream_options({std::chrono::milliseconds(1), std::chrono::milliseconds(1), 0}),
        std::invalid_argument);
    EXPECT_THROW(transport.set_stream_options(
                     {std::chrono::milliseconds(1), std::chrono::milliseconds(1), 1, 0}),
                 std::invalid_argument);
    EXPECT_NO_THROW(transport.set_stream_options({}));
}

TEST(HttpServerStreamOptions, OptionsCannotChangeOnceListening) {
    asio::io_context io_ctx;
    mcp::HttpServerTransport transport(io_ctx.get_executor(), "127.0.0.1", 0);
    mcp_test::spawn_detached(io_ctx, transport.listen());
    EXPECT_THROW(transport.set_stream_options({}), std::logic_error);
    transport.close();
    io_ctx.run();
}

}  // namespace

}  // namespace transport_http_server_stream_test

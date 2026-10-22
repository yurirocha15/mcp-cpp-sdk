#include "../test_utils.hpp"

#include "mcp/server/server.hpp"
#include "mcp/transport/http_server.hpp"

#include <gtest/gtest.h>

#include <algorithm>
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
    std::string allow;
    std::optional<std::string> session_id;

    [[nodiscard]] json parsed() const { return json::parse(body); }
};

// Everything is taken by value: this is a coroutine, and callers pass temporaries.
mcp::Task<Reply> send(asio::any_io_executor executor, unsigned short port, http::verb verb,
                      std::string body, Headers headers) {
    beast::tcp_stream stream(executor);
    asio::ip::tcp::resolver resolver(executor);
    auto endpoints =
        co_await resolver.async_resolve("127.0.0.1", std::to_string(port), asio::use_awaitable);
    co_await stream.async_connect(*endpoints.begin(), asio::use_awaitable);

    http::request<http::string_body> request{verb, "/mcp", 11};
    request.set(http::field::host, "127.0.0.1");
    request.set(http::field::content_type, "application/json");
    request.set(http::field::accept, "application/json, text/event-stream");
    for (const auto& [name, value] : headers) {
        request.insert(name, value);
    }
    request.body() = std::move(body);
    request.prepare_payload();
    co_await http::async_write(stream, request, asio::use_awaitable);

    beast::flat_buffer buffer;
    http::response<http::string_body> response;
    co_await http::async_read(stream, buffer, response, asio::use_awaitable);

    Reply reply;
    reply.status = response.result_int();
    reply.body = response.body();
    reply.allow = std::string(response[http::field::allow]);
    if (auto session = response.find("MCP-Session-Id"); session != response.end()) {
        reply.session_id = std::string(session->value());
    }
    beast::error_code ignored;
    stream.socket().shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
    co_return reply;
}

/// The headers a 2026-07-28 client sends with @p body.
Headers modern_headers(const json& body) {
    Headers headers{
        {"MCP-Protocol-Version", body["params"]["_meta"]["io.modelcontextprotocol/protocolVersion"]},
        {"Mcp-Method", body["method"]}};
    const auto method = body["method"].get<std::string>();
    if (method == "tools/call" || method == "prompts/get") {
        headers.emplace_back("Mcp-Name", body["params"]["name"]);
    }
    return headers;
}

Headers with(Headers headers, const std::string& name, const std::string& value) {
    std::erase_if(headers, [&name](const auto& header) { return header.first == name; });
    headers.emplace_back(name, value);
    return headers;
}

json call_tool(const json& id, const std::string& name, json arguments = json::object()) {
    return make_modern_request(id, "tools/call", {{"name", name}, {"arguments", std::move(arguments)}});
}

/// Waits, a millisecond at a time, until @p condition holds. Gives up after 30 s so a broken
/// rendezvous fails the test instead of hanging the run.
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

/// A transport that forwards everything to an HttpServerTransport, and hands it over through
/// http_server_transport() so Server::run() serves it as one.
class ForwardingTransport final : public mcp::ITransport {
   public:
    explicit ForwardingTransport(std::shared_ptr<mcp::HttpServerTransport> inner)
        : inner_(std::move(inner)) {}

    mcp::Task<std::string> read_message() override { return inner_->read_message(); }
    mcp::Task<mcp::IncomingMessage> read_incoming() override { return inner_->read_incoming(); }
    mcp::Task<void> write_message(std::string_view message) override {
        return inner_->write_message(message);
    }
    void close() override { inner_->close(); }
    mcp::HttpServerTransport* http_server_transport() noexcept override { return inner_.get(); }

   private:
    std::shared_ptr<mcp::HttpServerTransport> inner_;
};

class HttpServerModernTest : public ::testing::Test {
   protected:
    asio::io_context io_ctx_;
    std::shared_ptr<mcp::HttpServerTransport> transport_ =
        std::make_shared<mcp::HttpServerTransport>(io_ctx_.get_executor(), "127.0.0.1", 0);
    std::unique_ptr<mcp::Server> server_;
    std::atomic<int> rendezvous_arrivals_{0};
    std::atomic<bool> held_started_{false};
    std::atomic<bool> release_held_{false};

    void SetUp() override {
        mcp::ServerCapabilities caps;
        caps.tools = mcp::ServerCapabilities::ToolsCapability{};
        caps.logging = json::object();
        server_ = std::make_unique<mcp::Server>(mcp::Implementation{"http-modern", "1.0"}, caps);
        server_->add_tool<json, json>("echo", "Echoes its arguments", json{{"type", "object"}},
                                      [](json args) -> mcp::Task<json> { co_return args; });
        server_->add_tool<json, json>(
            "route", "Mirrors region into a header",
            json{{"type", "object"},
                 {"properties", {{"region", {{"type", "string"}, {"x-mcp-header", "Region"}}}}}},
            [](json args) -> mcp::Task<json> { co_return args; });
        server_->add_tool<json, json>("rendezvous", "Returns once two calls are in flight",
                                      json{{"type", "object"}}, [this](json args) -> mcp::Task<json> {
                                          rendezvous_arrivals_.fetch_add(1);
                                          const bool met = co_await wait_until(
                                              [this] { return rendezvous_arrivals_.load() >= 2; });
                                          co_return json{{"met", met}, {"who", args.value("who", "")}};
                                      });
        server_->add_tool<json, json>(
            "held", "Returns once released", json{{"type", "object"}},
            [this](json /*args*/) -> mcp::Task<json> {
                held_started_.store(true);
                const bool released = co_await wait_until([this] { return release_held_.load(); });
                co_return json{{"released", released}};
            });
        server_->add_tool<json, json>("log", "Logs at error level", json{{"type", "object"}},
                                      [](mcp::Context& ctx, json /*args*/) -> mcp::Task<json> {
                                          co_await ctx.log(mcp::LoggingLevel::eError, "error message");
                                          co_return json{{"logged", true}};
                                      });
    }

    /// Runs the server on the transport, then @p exchange, then closes everything.
    /// The server runs on @p served when given, otherwise on the transport itself.
    template <typename Exchange>
    void run(Exchange exchange, std::shared_ptr<mcp::ITransport> served = nullptr) {
        if (!served) {
            served = transport_;
        }
        asio::co_spawn(io_ctx_, transport_->listen(), asio::detached);
        asio::co_spawn(io_ctx_, serve(server_.get(), std::move(served), io_ctx_.get_executor()),
                       asio::detached);
        asio::co_spawn(
            io_ctx_,
            [this, exchange = std::move(exchange)]() mutable -> mcp::Task<void> {
                co_await exchange();
                transport_->close();
            },
            asio::detached);
        io_ctx_.run();
    }

    static mcp::Task<void> serve(mcp::Server* server, std::shared_ptr<mcp::ITransport> transport,
                                 asio::any_io_executor executor) {
        co_await server->run(std::move(transport), std::move(executor));
    }

    mcp::Task<Reply> post(json body, Headers headers) {
        return send(io_ctx_.get_executor(), transport_->port(), http::verb::post, body.dump(),
                    std::move(headers));
    }

    mcp::Task<Reply> post_modern(json body) {
        auto headers = modern_headers(body);
        return post(std::move(body), std::move(headers));
    }

    mcp::Task<Reply> post_legacy(json body, std::optional<std::string> session_id) {
        Headers headers;
        if (session_id) {
            headers.emplace_back("MCP-Session-Id", *session_id);
            headers.emplace_back("MCP-Protocol-Version",
                                 std::string(mcp::g_LATEST_LEGACY_PROTOCOL_VERSION));
        }
        return post(std::move(body), std::move(headers));
    }

    /// Initializes a legacy session and returns its id.
    mcp::Task<std::string> start_legacy_session() {
        const auto initialized = co_await post_legacy(make_initialize_request("init"), std::nullopt);
        EXPECT_EQ(initialized.status, 200u) << initialized.body;
        const auto session_id = initialized.session_id.value_or("");
        co_await post_legacy(make_initialized_notification(), session_id);
        co_return session_id;
    }

    static void expect_error(const Reply& reply, unsigned int status, int code) {
        EXPECT_EQ(reply.status, status) << reply.body;
        const auto body = json::parse(reply.body);
        ASSERT_TRUE(body.contains("error")) << reply.body;
        EXPECT_EQ(body["error"]["code"], code) << reply.body;
    }
};

TEST_F(HttpServerModernTest, ServesAModernRequestWithoutASession) {
    Reply reply;
    run([&]() -> mcp::Task<void> {
        reply = co_await post_modern(make_modern_request(1, "tools/list"));
    });

    EXPECT_EQ(reply.status, 200u) << reply.body;
    const auto body = reply.parsed();
    EXPECT_EQ(body["id"], 1);
    EXPECT_EQ(body["result"]["resultType"], "complete") << reply.body;
    EXPECT_EQ(body["result"]["tools"].size(), 5u);
    EXPECT_FALSE(reply.session_id.has_value());
    EXPECT_FALSE(server_->is_initialized());
}

TEST_F(HttpServerModernTest, TwoClientsUsingTheSameIdAreBothServed) {
    Reply first;
    Reply second;
    run([&]() -> mcp::Task<void> {
        int finished = 0;
        auto client = [&](std::string who, Reply& out) -> mcp::Task<void> {
            json arguments = json::object();
            arguments["who"] = who;
            auto body = call_tool(1, "rendezvous", std::move(arguments));
            out = co_await post_modern(std::move(body));
            ++finished;
        };
        asio::co_spawn(io_ctx_, client("a", first), asio::detached);
        asio::co_spawn(io_ctx_, client("b", second), asio::detached);
        co_await wait_until([&] { return finished == 2; });
    });

    for (const auto* reply : {&first, &second}) {
        EXPECT_EQ(reply->status, 200u) << reply->body;
        const auto body = reply->parsed();
        EXPECT_EQ(body["id"], 1) << reply->body;
        EXPECT_TRUE(body["result"]["structuredContent"]["met"].get<bool>()) << reply->body;
    }
    EXPECT_NE(first.parsed()["result"]["structuredContent"]["who"],
              second.parsed()["result"]["structuredContent"]["who"]);
}

TEST_F(HttpServerModernTest, LegacySessionAndModernRequestsShareTheEndpoint) {
    std::string session_id;
    Reply legacy;
    Reply modern;
    run([&]() -> mcp::Task<void> {
        session_id = co_await start_legacy_session();
        const json legacy_list = {
            {"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}, {"params", json::object()}};
        legacy = co_await post_legacy(legacy_list, session_id);
        modern = co_await post_modern(make_modern_request(1, "tools/list"));
    });

    ASSERT_FALSE(session_id.empty());
    EXPECT_EQ(legacy.status, 200u) << legacy.body;
    EXPECT_FALSE(legacy.body.find("resultType") != std::string::npos) << legacy.body;
    EXPECT_EQ(modern.status, 200u) << modern.body;
    EXPECT_EQ(modern.parsed()["result"]["resultType"], "complete") << modern.body;
    EXPECT_FALSE(modern.session_id.has_value());
    EXPECT_TRUE(server_->is_initialized());
}

TEST_F(HttpServerModernTest, ErrorsUseTheSpecifiedStatuses) {
    std::vector<std::pair<std::string, Reply>> replies;
    run([&]() -> mcp::Task<void> {
        const auto list = make_modern_request(1, "tools/list");
        const Headers version_only = {{"MCP-Protocol-Version", "2026-07-28"}};
        replies.emplace_back("missing Mcp-Method", co_await post(list, version_only));
        const json header_only = {
            {"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/list"}, {"params", json::object()}};
        const Headers both = {{"MCP-Protocol-Version", "2026-07-28"}, {"Mcp-Method", "tools/list"}};
        replies.emplace_back("header without _meta", co_await post(header_only, both));
        auto future = make_modern_request(3, "tools/list");
        future["params"]["_meta"]["io.modelcontextprotocol/protocolVersion"] = "2099-01-01";
        replies.emplace_back("unsupported version", co_await post_modern(future));
        replies.emplace_back("unknown method",
                             co_await post_modern(make_modern_request(4, "subscriptions/listen")));
        replies.emplace_back("unknown tool", co_await post_modern(call_tool(5, "missing")));
    });

    ASSERT_EQ(replies.size(), 5u);
    expect_error(replies[0].second, 400, mcp::g_HEADER_MISMATCH);
    expect_error(replies[1].second, 400, mcp::g_INVALID_PARAMS);
    expect_error(replies[2].second, 400, mcp::g_UNSUPPORTED_PROTOCOL_VERSION);
    EXPECT_EQ(replies[2].second.parsed()["error"]["data"]["requested"], "2099-01-01");
    expect_error(replies[3].second, 404, mcp::g_METHOD_NOT_FOUND);
    expect_error(replies[4].second, 400, mcp::g_INVALID_PARAMS);
    EXPECT_EQ(transport_->header_mismatch_rejections(), 1u);
}

TEST_F(HttpServerModernTest, McpParamHeadersAreCheckedAgainstTheTool) {
    Reply matched;
    Reply mismatched;
    Reply missing;
    run([&]() -> mcp::Task<void> {
        const auto body = call_tool(1, "route", {{"region", "us-west1"}});
        matched = co_await post(body, with(modern_headers(body), "Mcp-Param-Region", "us-west1"));
        mismatched = co_await post(body, with(modern_headers(body), "Mcp-Param-Region", "eu"));
        missing = co_await post_modern(body);
    });

    EXPECT_EQ(matched.status, 200u) << matched.body;
    expect_error(mismatched, 400, mcp::g_HEADER_MISMATCH);
    expect_error(missing, 400, mcp::g_HEADER_MISMATCH);
    EXPECT_EQ(transport_->header_mismatch_rejections(), 2u);
}

TEST_F(HttpServerModernTest, GetAndDeleteWithAModernHeaderAndNoSessionAre405) {
    Reply get;
    Reply del;
    run([&]() -> mcp::Task<void> {
        const Headers version_only = {{"MCP-Protocol-Version", "2026-07-28"}};
        get = co_await send(io_ctx_.get_executor(), transport_->port(), http::verb::get, "",
                            version_only);
        del = co_await send(io_ctx_.get_executor(), transport_->port(), http::verb::delete_, "",
                            version_only);
    });
    EXPECT_EQ(get.status, 405u) << get.body;
    EXPECT_EQ(get.allow, "POST");
    EXPECT_EQ(del.status, 405u) << del.body;
    EXPECT_EQ(del.allow, "POST");
}

// 2026-07-28 has no initialize. The error names the versions the server serves, which Server::run()
// told the transport; a version the transport does not know keeps the legacy refusal.
TEST_F(HttpServerModernTest, InitializeWithAModernHeaderIsMethodNotFound) {
    mcp::ServerProtocolPolicy policy;
    policy.legacy_versions = std::vector<std::string>{"2025-11-25"};
    server_->set_protocol_policy(policy);
    Reply modern;
    Reply unknown;
    run([&]() -> mcp::Task<void> {
        const Headers modern_header = {{"MCP-Protocol-Version", "2026-07-28"}};
        const Headers unknown_header = {{"MCP-Protocol-Version", "2099-01-01"}};
        modern = co_await post(make_initialize_request("i"), modern_header);
        unknown = co_await post(make_initialize_request("u"), unknown_header);
    });
    expect_error(modern, 404, mcp::g_METHOD_NOT_FOUND);
    EXPECT_EQ(modern.parsed()["id"], "i");
    EXPECT_EQ(modern.parsed()["error"]["data"]["supported"], (json{"2025-11-25", "2026-07-28"}));
    EXPECT_FALSE(modern.session_id.has_value());
    EXPECT_EQ(unknown.status, 400u);
    EXPECT_EQ(unknown.parsed(), (json{{"error", "Invalid MCP-Protocol-Version header"}}));
    EXPECT_FALSE(server_->is_initialized());
}

// HttpServerTransport answers every request with plain JSON, so it has no stream for a
// subscription to keep open.
TEST_F(HttpServerModernTest, SubscriptionsListenIsMethodNotFound) {
    Reply reply;
    json params = json::object();
    params["notifications"] = {{"toolsListChanged", true}};
    const auto request = make_modern_request(5, "subscriptions/listen", std::move(params));
    run([&]() -> mcp::Task<void> { reply = co_await post_modern(request); });
    expect_error(reply, 404, mcp::g_METHOD_NOT_FOUND);
    EXPECT_EQ(reply.parsed()["id"], 5);
    EXPECT_EQ(server_->subscription_count(), 0u);
}

// A wrapper that hands over its HttpServerTransport is served as one: the Mcp-Param headers are
// checked, and a 2026-07-28 request's notifications stay out of the legacy session's stream.
TEST_F(HttpServerModernTest, WrapperHandingOverTheTransportIsServedAsIt) {
    Reply logged;
    Reply missing_param;
    std::size_t stored_before = 0;
    run(
        [&]() -> mcp::Task<void> {
            co_await start_legacy_session();
            stored_before = transport_->event_store().size();
            auto log_body = call_tool(1, "log");
            log_body["params"]["_meta"]["io.modelcontextprotocol/logLevel"] = "debug";
            logged = co_await post_modern(log_body);
            const json region = {{"region", "us-west1"}};
            auto route_body = call_tool(2, "route", region);
            missing_param = co_await post_modern(std::move(route_body));
        },
        std::make_shared<ForwardingTransport>(transport_));

    EXPECT_EQ(logged.status, 200u) << logged.body;
    EXPECT_EQ(transport_->event_store().size(), stored_before);
    expect_error(missing_param, 400, mcp::g_HEADER_MISMATCH);
    EXPECT_EQ(transport_->header_mismatch_rejections(), 1u);
}

TEST_F(HttpServerModernTest, NotificationsOfAModernRequestStayOutOfTheSessionStream) {
    Reply logged;
    std::size_t stored_before = 0;
    run([&]() -> mcp::Task<void> {
        co_await start_legacy_session();
        stored_before = transport_->event_store().size();
        auto body = call_tool(1, "log");
        body["params"]["_meta"]["io.modelcontextprotocol/logLevel"] = "debug";
        logged = co_await post_modern(body);
    });

    EXPECT_EQ(logged.status, 200u) << logged.body;
    EXPECT_TRUE(logged.parsed()["result"]["structuredContent"]["logged"].get<bool>()) << logged.body;
    EXPECT_EQ(transport_->event_store().size(), stored_before);
}

TEST_F(HttpServerModernTest, EndingTheLegacySessionLeavesModernRequestsRunning) {
    Reply held;
    Reply deleted;
    run([&]() -> mcp::Task<void> {
        const auto session_id = co_await start_legacy_session();
        bool held_done = false;
        asio::co_spawn(
            io_ctx_,
            [&]() -> mcp::Task<void> {
                held = co_await post_modern(call_tool(1, "held"));
                held_done = true;
            },
            asio::detached);
        // End the session while the modern request runs, then let that request finish.
        co_await wait_until([this] { return held_started_.load(); });
        const Headers session_headers = {
            {"MCP-Session-Id", session_id},
            {"MCP-Protocol-Version", std::string(mcp::g_LATEST_LEGACY_PROTOCOL_VERSION)}};
        deleted = co_await send(io_ctx_.get_executor(), transport_->port(), http::verb::delete_, "",
                                session_headers);
        release_held_.store(true);
        co_await wait_until([&] { return held_done; });
    });

    EXPECT_EQ(deleted.status, 200u) << deleted.body;
    EXPECT_EQ(held.status, 200u) << held.body;
    EXPECT_TRUE(held.parsed()["result"]["structuredContent"]["released"].get<bool>()) << held.body;
}

}  // namespace

#include "../test_utils.hpp"

#include "mcp/server/server.hpp"
#include "mcp/transport/http_session_manager.hpp"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
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

/// The value at JSON pointer @p pointer, or null when the message lacks it.
json field(const json& message, const char* pointer) {
    return message.value(json::json_pointer(pointer), json());
}

/// The subscription id a notification names, or null.
json subscription_of(const json& message) {
    return field(message, "/params/_meta/io.modelcontextprotocol~1subscriptionId");
}

/// The subscription id a result names, or null.
json result_subscription_of(const json& message) {
    return field(message, "/result/_meta/io.modelcontextprotocol~1subscriptionId");
}

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

/// The bytes of an HTTP request carrying @p body as a 2026-07-28 client sends it.
std::string modern_request_bytes(const json& body, bool accepts_sse = true) {
    http::request<http::string_body> request{http::verb::post, "/mcp", 11};
    request.set(http::field::host, "127.0.0.1");
    request.set(http::field::content_type, "application/json");
    request.set(http::field::accept,
                accepts_sse ? "application/json, text/event-stream" : "application/json");
    request.set("MCP-Protocol-Version", "2026-07-28");
    request.set("Mcp-Method", body["method"].get<std::string>());
    request.keep_alive(true);
    request.body() = body.dump();
    request.prepare_payload();
    std::ostringstream bytes;
    bytes << request;
    return bytes.str();
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

class HttpListen : public ::testing::Test {
   protected:
    asio::io_context io_ctx_;
    std::atomic<int> factory_calls_{0};

    static std::unique_ptr<mcp::Server> make_server(const std::string& name) {
        mcp::ServerCapabilities caps;
        caps.tools = mcp::ServerCapabilities::ToolsCapability{true};
        caps.prompts = mcp::ServerCapabilities::PromptsCapability{true};
        caps.resources = mcp::ServerCapabilities::ResourcesCapability{true, true};
        auto server = std::make_unique<mcp::Server>(mcp::Implementation{name, "1.0"}, caps);
        server->add_tool<json, json>("echo", "Echoes", json{{"type", "object"}},
                                     [](json args) -> mcp::Task<json> { co_return args; });
        return server;
    }

    /// A manager whose shared modern server is built lazily from the factory.
    std::unique_ptr<mcp::StreamableHttpSessionManager> make_manager(unsigned short port) {
        return std::make_unique<mcp::StreamableHttpSessionManager>(
            io_ctx_.get_executor(), "127.0.0.1", port,
            [this](const asio::any_io_executor&) -> std::unique_ptr<mcp::Server> {
                factory_calls_.fetch_add(1);
                return make_server("factory");
            });
    }

    /// Runs @p exchange against @p manager, then closes the manager.
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

    static bool is_method(const json& message, std::string_view method) {
        return message.contains("method") && message["method"] == method;
    }
};

// The stream stays open after a batch of notifications has been written, carries later ones, and
// ends only with the response the server sends when it closes the subscription.
TEST_F(HttpListen, StreamOutlivesNotificationBatch) {
    auto manager = make_manager(20470);
    RawClient client(io_ctx_.get_executor());
    bool saw_end = false;
    json filter = {{"toolsListChanged", true}};
    filter["resourceSubscriptions"] = json::array({"file:///a"});
    const auto request = modern_request_bytes(listen_request(1, filter));
    run(*manager, [&]() -> mcp::Task<void> {
        co_await client.connect(20470);
        co_await client.send(request);
        const bool acknowledged = co_await client.read_events(1);
        EXPECT_TRUE(acknowledged) << client.received();

        auto server = manager->modern_server();
        for (int index = 0; index < 3; ++index) {
            co_await server->notify_tools_list_changed();
        }
        co_await server->notify_resource_updated("file:///a");
        const bool batch = co_await client.read_events(5);
        EXPECT_TRUE(batch) << client.received();
        // Everything queued has been written, and the stream is still open.
        EXPECT_EQ(client.received().find("0\r\n\r\n"), std::string::npos) << client.received();
        EXPECT_EQ(server->subscription_count(), 1u);

        co_await server->notify_tools_list_changed();
        const bool later = co_await client.read_events(6);
        EXPECT_TRUE(later) << client.received();

        server->close_subscriptions();
        saw_end = co_await client.read_until("0\r\n\r\n");
        const bool removed = co_await wait_until([&] { return server->subscription_count() == 0; });
        EXPECT_TRUE(removed);
    });

    ASSERT_TRUE(saw_end) << client.received();
    const auto& text = client.received();
    EXPECT_NE(text.find("Content-Type: text/event-stream"), std::string::npos) << text;
    EXPECT_NE(text.find("X-Accel-Buffering: no"), std::string::npos) << text;
    const auto events = client.events();
    ASSERT_EQ(events.size(), 7u) << text;
    EXPECT_TRUE(is_method(events[0], "notifications/subscriptions/acknowledged"));
    EXPECT_EQ(field(events[0], "/params/notifications"),
              (json{{"toolsListChanged", true}, {"resourceSubscriptions", {"file:///a"}}}));
    for (std::size_t index = 0; index < 6; ++index) {
        EXPECT_EQ(subscription_of(events[index]), 1) << events[index];
    }
    EXPECT_TRUE(is_method(events[4], "notifications/resources/updated"));
    EXPECT_EQ(field(events[4], "/params/uri"), "file:///a");
    EXPECT_EQ(events[6]["id"], 1);
    EXPECT_EQ(field(events[6], "/result/resultType"), "complete");
    EXPECT_EQ(result_subscription_of(events[6]), 1);
    EXPECT_EQ(manager->stream_stats().streams_opened, 1u);
}

// A subscription needs no set_modern_server(): it runs on the instance built from the factory.
TEST_F(HttpListen, WorksOnLazilyBuiltInstance) {
    auto manager = make_manager(20471);
    RawClient client(io_ctx_.get_executor());
    const auto request = modern_request_bytes(listen_request(7, {{"toolsListChanged", true}}));
    run(*manager, [&]() -> mcp::Task<void> {
        co_await client.connect(20471);
        co_await client.send(request);
        const bool acknowledged = co_await client.read_events(1);
        EXPECT_TRUE(acknowledged) << client.received();
    });

    EXPECT_NE(client.received().find("HTTP/1.1 200"), std::string::npos) << client.received();
    EXPECT_EQ(client.received().find("set_modern_server"), std::string::npos) << client.received();
    const auto events = client.events();
    ASSERT_GE(events.size(), 1u);
    EXPECT_TRUE(is_method(events[0], "notifications/subscriptions/acknowledged"));
    EXPECT_EQ(subscription_of(events[0]), 7);
    EXPECT_EQ(factory_calls_.load(), 1);
}

// Notifications fired on the server the accessor returns reach the subscriptions, for every type.
TEST_F(HttpListen, NotificationsFiredOnModernServerAccessorReachListeners) {
    auto manager = make_manager(20472);
    RawClient client(io_ctx_.get_executor());
    json filter = {
        {"toolsListChanged", true}, {"promptsListChanged", true}, {"resourcesListChanged", true}};
    filter["resourceSubscriptions"] = json::array({"file:///a"});
    const auto request = modern_request_bytes(listen_request("all", filter));
    run(*manager, [&]() -> mcp::Task<void> {
        co_await client.connect(20472);
        co_await client.send(request);
        co_await client.read_events(1);
        auto server = manager->modern_server();
        co_await server->notify_tools_list_changed();
        co_await server->notify_prompts_list_changed();
        co_await server->notify_resources_list_changed();
        co_await server->notify_resource_updated("file:///a");
        const bool all = co_await client.read_events(5);
        EXPECT_TRUE(all) << client.received();
    });

    const auto events = client.events();
    ASSERT_GE(events.size(), 5u) << client.received();
    const std::vector<std::string> methods = {
        "notifications/subscriptions/acknowledged", "notifications/tools/list_changed",
        "notifications/prompts/list_changed", "notifications/resources/list_changed",
        "notifications/resources/updated"};
    for (std::size_t index = 0; index < methods.size(); ++index) {
        EXPECT_TRUE(is_method(events[index], methods[index])) << events[index];
        EXPECT_EQ(subscription_of(events[index]), "all") << events[index];
    }
    EXPECT_EQ(field(events[4], "/params/uri"), "file:///a");
}

// A server given to set_modern_server() answers the subscription and fires its notifications;
// the factory is never asked for one.
TEST_F(HttpListen, SetModernServerOverrideReceivesNotifications) {
    auto manager = make_manager(20473);
    auto custom = std::shared_ptr<mcp::Server>(make_server("custom"));
    manager->set_modern_server(custom);
    RawClient client(io_ctx_.get_executor());
    const auto request = modern_request_bytes(listen_request(1, {{"toolsListChanged", true}}));
    run(*manager, [&]() -> mcp::Task<void> {
        co_await client.connect(20473);
        co_await client.send(request);
        co_await client.read_events(1);
        EXPECT_EQ(custom->subscription_count(), 1u);
        co_await custom->notify_tools_list_changed();
        const bool changed = co_await client.read_events(2);
        EXPECT_TRUE(changed) << client.received();
    });

    const auto events = client.events();
    ASSERT_GE(events.size(), 2u);
    EXPECT_TRUE(is_method(events[1], "notifications/tools/list_changed"));
    EXPECT_EQ(subscription_of(events[1]), 1);
    EXPECT_EQ(factory_calls_.load(), 0);
}

// Two clients may use the same request id: each stream gets only what its own subscription asked
// for.
TEST_F(HttpListen, TwoStreamsDemux) {
    auto manager = make_manager(20474);
    RawClient tools_client(io_ctx_.get_executor());
    RawClient resource_client(io_ctx_.get_executor());
    json resource_filter = json::object();
    resource_filter["resourceSubscriptions"] = json::array({"file:///a"});
    const auto tools_request = modern_request_bytes(listen_request(1, {{"toolsListChanged", true}}));
    const auto resource_request = modern_request_bytes(listen_request(1, resource_filter));
    run(*manager, [&]() -> mcp::Task<void> {
        co_await tools_client.connect(20474);
        co_await tools_client.send(tools_request);
        co_await resource_client.connect(20474);
        co_await resource_client.send(resource_request);
        co_await tools_client.read_events(1);
        co_await resource_client.read_events(1);
        auto server = manager->modern_server();
        EXPECT_EQ(server->subscription_count(), 2u);

        co_await server->notify_tools_list_changed();
        co_await server->notify_resource_updated("file:///a");
        co_await server->notify_tools_list_changed();
        co_await tools_client.read_events(3);
        // Ordered after the first change, so a tools change that leaked to this stream would
        // already have arrived.
        co_await resource_client.read_events(2);
        co_await server->notify_resource_updated("file:///a");
        co_await resource_client.read_events(3);
    });

    const auto tools_events = tools_client.events();
    ASSERT_EQ(tools_events.size(), 3u) << tools_client.received();
    EXPECT_TRUE(is_method(tools_events[1], "notifications/tools/list_changed"));
    EXPECT_TRUE(is_method(tools_events[2], "notifications/tools/list_changed"));
    const auto resource_events = resource_client.events();
    ASSERT_EQ(resource_events.size(), 3u) << resource_client.received();
    EXPECT_TRUE(is_method(resource_events[1], "notifications/resources/updated"));
    EXPECT_TRUE(is_method(resource_events[2], "notifications/resources/updated"));
}

// "Closing the SSE response stream MUST be treated by the server as cancellation of that request."
TEST_F(HttpListen, ListenerRemovedWhenTheClientResets) {
    auto manager = make_manager(20475);
    RawClient client(io_ctx_.get_executor());
    const auto request = modern_request_bytes(listen_request(1, {{"toolsListChanged", true}}));
    run(*manager, [&]() -> mcp::Task<void> {
        co_await client.connect(20475);
        co_await client.send(request);
        co_await client.read_events(1);
        auto server = manager->modern_server();
        EXPECT_EQ(server->subscription_count(), 1u);
        client.reset();
        const bool removed = co_await wait_until([&] { return server->subscription_count() == 0; });
        EXPECT_TRUE(removed);
    });
    EXPECT_EQ(manager->stream_stats().disconnect_cancels, 1u);
}

// A client that closes its end without a reset looks like a half-close until a probe fails.
TEST_F(HttpListen, ListenerRemovedWhenTheClientCloses) {
    auto manager = make_manager(20476);
    manager->set_stream_options({std::chrono::milliseconds(15000), std::chrono::milliseconds(5), 4096});
    RawClient client(io_ctx_.get_executor());
    const auto request = modern_request_bytes(listen_request(1, {{"toolsListChanged", true}}));
    run(*manager, [&]() -> mcp::Task<void> {
        co_await client.connect(20476);
        co_await client.send(request);
        co_await client.read_events(1);
        auto server = manager->modern_server();
        client.close();
        const bool removed = co_await wait_until([&] { return server->subscription_count() == 0; });
        EXPECT_TRUE(removed);
    });
    EXPECT_EQ(manager->stream_stats().disconnect_cancels, 1u);
}

// The response to a listen is a stream; a client that does not accept one is refused.
TEST_F(HttpListen, ClientNotAcceptingStreamsIsRefused) {
    auto manager = make_manager(20477);
    RawClient client(io_ctx_.get_executor());
    const auto request = modern_request_bytes(listen_request(1, {{"toolsListChanged", true}}), false);
    run(*manager, [&]() -> mcp::Task<void> {
        co_await client.connect(20477);
        co_await client.send(request);
        co_await client.read_until(R"("error")");
        EXPECT_EQ(manager->modern_server()->subscription_count(), 0u);
    });

    EXPECT_NE(client.received().find("Content-Type: application/json"), std::string::npos)
        << client.received();
    EXPECT_NE(client.received().find(std::to_string(mcp::g_INVALID_REQUEST)), std::string::npos)
        << client.received();
}

}  // namespace

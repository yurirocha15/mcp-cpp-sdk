// Streams of sessions of protocol revision 2025-11-25 and earlier over HTTP: the answer to a POST
// carries the request's notifications and the server's own requests as they come, a GET opens the
// stream for what belongs to no request, and a stream the client left is resumed with
// Last-Event-ID. Covers StreamableHttpSessionManager, HttpServerTransport and HttpClientTransport.

#include "mcp/client/client.hpp"
#include "mcp/core/async.hpp"
#include "mcp/server/server.hpp"
#include "mcp/transport/http_client.hpp"
#include "mcp/transport/http_server.hpp"
#include "mcp/transport/http_session_manager.hpp"
#include "support/hang_guard.hpp"
#include "support/spawn.hpp"

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
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <chrono>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace transport_http_legacy_stream_test {

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = boost::beast::http;
using nlohmann::json;

constexpr auto g_version = "2025-11-25";

/// Waits, a millisecond at a time, until @p condition holds. Gives up after g_hang_guard so a broken
/// hand-off fails the test instead of hanging the run.
mcp::Task<bool> wait_until(std::function<bool()> condition) {
    auto executor = co_await asio::this_coro::executor;
    const auto give_up = std::chrono::steady_clock::now() + g_hang_guard;
    while (!condition() && std::chrono::steady_clock::now() < give_up) {
        asio::steady_timer tick(executor, std::chrono::milliseconds(1));
        co_await tick.async_wait(asio::use_awaitable);
    }
    co_return condition();
}

/// One SSE event: its id and its data.
struct Event {
    std::string id;
    std::string data;
};

/// One HTTP exchange whose answer is read an event at a time, as a client reads a stream.
class Exchange {
   public:
    explicit Exchange(const asio::any_io_executor& executor) : stream_(executor) {}

    /// Sends a request with @p headers and reads the head of the answer.
    mcp::Task<void> open(unsigned short port, http::verb verb, std::string body,
                         std::vector<std::pair<std::string, std::string>> headers) {
        auto resolver = asio::ip::tcp::resolver(stream_.get_executor());
        auto endpoints =
            co_await resolver.async_resolve("127.0.0.1", std::to_string(port), asio::use_awaitable);
        co_await stream_.async_connect(*endpoints.begin(), asio::use_awaitable);
        http::request<http::string_body> request{verb, "/mcp", 11};
        request.set(http::field::host, "127.0.0.1");
        request.set(http::field::accept, "application/json, text/event-stream");
        if (verb == http::verb::post) {
            request.set(http::field::content_type, "application/json");
        }
        for (const auto& [name, value] : headers) {
            request.set(name, value);
        }
        request.body() = std::move(body);
        request.prepare_payload();
        co_await http::async_write(stream_, request, asio::use_awaitable);
        parser_.body_limit(std::numeric_limits<std::uint64_t>::max());
        co_await http::async_read_header(stream_, buffer_, parser_, asio::use_awaitable);
        status_ = parser_.get().result_int();
        content_type_ = std::string(parser_.get()[http::field::content_type]);
        session_id_ = std::string(parser_.get()["Mcp-Session-Id"]);
    }

    /// The next event of the stream, or nothing once the answer ends.
    mcp::Task<std::optional<Event>> next() {
        for (;;) {
            if (auto event = take_event()) {
                co_return event;
            }
            if (parser_.is_done()) {
                co_return std::nullopt;
            }
            std::array<char, 4096> chunk{};
            parser_.get().body().data = chunk.data();
            parser_.get().body().size = chunk.size();
            boost::system::error_code error;
            co_await http::async_read_some(stream_, buffer_, parser_,
                                           asio::redirect_error(asio::use_awaitable, error));
            if (error && error != http::error::need_buffer) {
                co_return std::nullopt;
            }
            text_.append(chunk.data(), chunk.size() - parser_.get().body().size);
        }
    }

    /// The next event that carries a message, skipping a priming event.
    mcp::Task<std::optional<json>> next_message() {
        for (;;) {
            auto event = co_await next();
            if (!event) {
                co_return std::nullopt;
            }
            if (!event->data.empty()) {
                last_id_ = event->id;
                co_return json::parse(event->data);
            }
        }
    }

    void close() {
        beast::error_code ignored;
        (void)stream_.socket().shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
        (void)stream_.socket().close(ignored);
    }

    /// Gives the connection a small receive buffer, so that the server's write of a large event
    /// waits until the event is read. Called before open().
    void shrink_receive_buffer() {
        stream_.socket().open(asio::ip::tcp::v4());
        stream_.socket().set_option(asio::socket_base::receive_buffer_size(4096));
    }

    [[nodiscard]] unsigned status() const { return status_; }
    [[nodiscard]] const std::string& content_type() const { return content_type_; }
    [[nodiscard]] const std::string& session_id() const { return session_id_; }
    [[nodiscard]] const std::string& last_id() const { return last_id_; }
    /// The SSE comments, such as keep-alives, read so far.
    [[nodiscard]] std::size_t comments() const { return comments_; }

   private:
    std::optional<Event> take_event() {
        for (;;) {
            // The search goes on where the last one stopped, so a large event is scanned once.
            const auto end = text_.find("\n\n", searched_);
            if (end == std::string::npos) {
                searched_ = text_.empty() ? 0 : text_.size() - 1;
                return std::nullopt;
            }
            const auto block = text_.substr(0, end);
            text_.erase(0, end + 2);
            searched_ = 0;
            Event event;
            bool has_field = false;
            std::size_t offset = 0;
            while (offset <= block.size()) {
                auto line_end = block.find('\n', offset);
                if (line_end == std::string::npos) {
                    line_end = block.size();
                }
                const auto line = block.substr(offset, line_end - offset);
                if (line.starts_with("id: ")) {
                    event.id = line.substr(4);
                    has_field = true;
                } else if (line.starts_with("data: ")) {
                    event.data += line.substr(6);
                    has_field = true;
                } else if (line == "data:") {
                    has_field = true;
                }
                offset = line_end + 1;
            }
            if (has_field) {
                return event;
            }
            if (block.starts_with(":")) {
                ++comments_;
            }
        }
    }

    beast::tcp_stream stream_;
    beast::flat_buffer buffer_;
    http::response_parser<http::buffer_body> parser_;
    std::string text_;
    std::size_t searched_{0};
    std::size_t comments_{0};
    unsigned status_{0};
    std::string content_type_;
    std::string session_id_;
    std::string last_id_;
};

struct Reply {
    unsigned status{0};
    std::string session_id;
    std::string body;
};

/// The messages of @p exchange's stream from here to its end.
mcp::Task<std::vector<json>> rest_of(std::shared_ptr<Exchange> exchange) {
    std::vector<json> messages;
    for (;;) {
        auto message = co_await exchange->next_message();
        if (!message) {
            co_return messages;
        }
        messages.push_back(std::move(*message));
    }
}

/// Sends one request and reads the whole answer.
mcp::Task<Reply> send(const asio::any_io_executor& executor, unsigned short port, http::verb verb,
                      std::string body, std::string session_id) {
    beast::tcp_stream stream(executor);
    auto resolver = asio::ip::tcp::resolver(executor);
    auto endpoints =
        co_await resolver.async_resolve("127.0.0.1", std::to_string(port), asio::use_awaitable);
    co_await stream.async_connect(*endpoints.begin(), asio::use_awaitable);
    http::request<http::string_body> request{verb, "/mcp", 11};
    request.set(http::field::host, "127.0.0.1");
    request.set(http::field::accept, "application/json, text/event-stream");
    request.set(http::field::content_type, "application/json");
    request.set("MCP-Protocol-Version", g_version);
    if (!session_id.empty()) {
        request.set("Mcp-Session-Id", session_id);
    }
    request.body() = std::move(body);
    request.prepare_payload();
    co_await http::async_write(stream, request, asio::use_awaitable);
    beast::flat_buffer buffer;
    http::response<http::string_body> response;
    co_await http::async_read(stream, buffer, response, asio::use_awaitable);
    Reply reply;
    reply.status = response.result_int();
    reply.session_id = std::string(response["Mcp-Session-Id"]);
    reply.body = response.body();
    co_return reply;
}

/// The status of a GET on @p session_id whose Accept header is @p accept, or has none when empty.
mcp::Task<unsigned> get_status(const asio::any_io_executor& executor, unsigned short port,
                               std::string session_id, std::string accept) {
    beast::tcp_stream stream(executor);
    auto resolver = asio::ip::tcp::resolver(executor);
    auto endpoints =
        co_await resolver.async_resolve("127.0.0.1", std::to_string(port), asio::use_awaitable);
    co_await stream.async_connect(*endpoints.begin(), asio::use_awaitable);
    http::request<http::empty_body> request{http::verb::get, "/mcp", 11};
    request.set(http::field::host, "127.0.0.1");
    if (!accept.empty()) {
        request.set(http::field::accept, accept);
    }
    request.set("MCP-Protocol-Version", g_version);
    request.set("Mcp-Session-Id", session_id);
    co_await http::async_write(stream, request, asio::use_awaitable);
    beast::flat_buffer buffer;
    http::response<http::string_body> response;
    co_await http::async_read(stream, buffer, response, asio::use_awaitable);
    co_return response.result_int();
}

json initialize_request() {
    return {{"jsonrpc", "2.0"},
            {"id", 1},
            {"method", "initialize"},
            {"params",
             {{"protocolVersion", g_version},
              {"capabilities", {{"elicitation", json::object()}}},
              {"clientInfo", {{"name", "raw"}, {"version", "1"}}}}}};
}

/// Starts a session as a client does, and returns its id.
mcp::Task<std::string> start_session(const asio::any_io_executor& executor, unsigned short port) {
    // Named: GCC mishandles a temporary in a co_await operand.
    const auto initialize = initialize_request().dump();
    const auto reply = co_await send(executor, port, http::verb::post, initialize, {});
    const auto notification = json{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}.dump();
    co_await send(executor, port, http::verb::post, notification, reply.session_id);
    co_return reply.session_id;
}

std::vector<std::pair<std::string, std::string>> session_headers(const std::string& session) {
    return {{"MCP-Protocol-Version", g_version}, {"Mcp-Session-Id", session}};
}

json call_tool(int id, const std::string& name, json arguments = json::object()) {
    return {{"jsonrpc", "2.0"},
            {"id", id},
            {"method", "tools/call"},
            {"params", {{"name", name}, {"arguments", std::move(arguments)}}}};
}

std::string text_of(const json& response) {
    return response.at("result").at("content").at(0).at("text").get<std::string>();
}

std::string text_of(const mcp::CallToolResult& result) {
    return json(result).at("content").at(0).at("text").get<std::string>();
}

/// What the tools of these tests share: the flag that lets `wait` end, and the servers made.
struct Shared {
    std::atomic<bool> released{false};
    std::mutex mutex;
    std::vector<mcp::Server*> servers;

    mcp::Server* last_server() {
        std::lock_guard lock(mutex);
        return servers.empty() ? nullptr : servers.back();
    }
};

/// A server whose `ask` tool asks the client for a name, whose `wait` tool logs its tag, then waits
/// until released, and whose `flood` tool logs a message of `size` bytes, then waits until released.
std::unique_ptr<mcp::Server> make_server(const std::shared_ptr<Shared>& shared) {
    mcp::ServerCapabilities caps;
    caps.tools = mcp::ServerCapabilities::ToolsCapability{true};
    caps.logging = json::object();
    auto server = std::make_unique<mcp::Server>(mcp::Implementation{"streams", "1.0"}, caps);
    server->add_tool<json, mcp::CallToolResult>(
        "ask", "Asks for a name", json{{"type", "object"}},
        [](mcp::Context& context, json) -> mcp::Task<mcp::CallToolResult> {
            mcp::ElicitRequestFormParams params;
            params.message = "Your name?";
            params.requestedSchema = {{"type", "object"},
                                      {"properties", {{"name", {{"type", "string"}}}}}};
            const auto answer = co_await context.elicit(params);
            co_return mcp::make_tool_text_result(
                answer.content.value_or(json::object()).value("name", "nobody"));
        });
    server->add_tool<json, mcp::CallToolResult>(
        "wait", "Logs its tag, then waits until released", json{{"type", "object"}},
        [shared](mcp::Context& context, json arguments) -> mcp::Task<mcp::CallToolResult> {
            const auto tag = arguments.value("tag", std::string());
            co_await context.log_info(tag);
            co_await wait_until([shared] { return shared->released.load(); });
            co_return mcp::make_tool_text_result("done-" + tag);
        });
    server->add_tool<json, mcp::CallToolResult>(
        "flood", "Logs a large message, then waits until released", json{{"type", "object"}},
        [shared](mcp::Context& context, json arguments) -> mcp::Task<mcp::CallToolResult> {
            const std::string text(arguments.value("size", std::size_t{0}), 'x');
            co_await context.log_info(text);
            co_await wait_until([shared] { return shared->released.load(); });
            co_return mcp::make_tool_text_result("flooded");
        });
    {
        std::lock_guard lock(shared->mutex);
        shared->servers.push_back(server.get());
    }
    return server;
}

/// The size of the log of the `flood` tool: large enough that the server's write of it waits until a
/// client with a shrunk receive buffer reads it.
constexpr std::size_t g_flood_bytes = std::size_t{8} << 20U;

/// What the stream of a `flood` call carried: its messages and the comments among them.
struct Flooded {
    std::vector<json> messages;
    std::size_t comments{0};
};

/// Calls `flood` on a connection with a shrunk receive buffer and releases the tool once the stream
/// is open, so that the result comes while the server's write of the log waits for this client.
mcp::Task<Flooded> call_flood(asio::any_io_executor executor, unsigned short port,
                              std::shared_ptr<Shared> shared) {
    const auto session = co_await start_session(executor, port);
    auto exchange = std::make_shared<Exchange>(executor);
    exchange->shrink_receive_buffer();
    const auto body = call_tool(2, "flood", json{{"size", g_flood_bytes}}).dump();
    const auto headers = session_headers(session);
    co_await exchange->open(port, http::verb::post, body, headers);
    shared->released = true;
    // Gives the result time to come before the log is read.
    asio::steady_timer settle(executor, std::chrono::milliseconds(200));
    co_await settle.async_wait(asio::use_awaitable);
    Flooded flooded;
    flooded.messages = co_await rest_of(exchange);
    flooded.comments = exchange->comments();
    co_return flooded;
}

/// Checks that @p flooded carried the log, then the result, and no keep-alive comment before them.
void expect_log_then_result(const Flooded& flooded) {
    ASSERT_EQ(flooded.messages.size(), 2U);
    const auto& log = flooded.messages[0];
    EXPECT_EQ(log.value("method", std::string()), "notifications/message");
    EXPECT_EQ(log.value(json::json_pointer("/params/data"), std::string()).size(), g_flood_bytes);
    EXPECT_EQ(text_of(flooded.messages[1]), "flooded") << flooded.messages[1];
    EXPECT_EQ(flooded.comments, 0U);
}

class LegacyStreamTest : public ::testing::Test {
   protected:
    asio::io_context io_ctx_;
    std::shared_ptr<Shared> shared_ = std::make_shared<Shared>();
    /// Stops the run after g_hang_guard, so a stream that never ends fails the test instead of
    /// hanging it.
    asio::steady_timer guard_{io_ctx_, g_hang_guard};

    void SetUp() override {
        guard_.async_wait([this](const boost::system::error_code& error) {
            if (!error) {
                io_ctx_.stop();
            }
        });
    }

    std::unique_ptr<mcp::StreamableHttpSessionManager> make_manager(unsigned short port) {
        auto manager = std::make_unique<mcp::StreamableHttpSessionManager>(
            io_ctx_.get_executor(), "127.0.0.1", port,
            [shared = shared_](const mcp::Executor&) { return make_server(shared); });
        mcp_test::spawn_detached(io_ctx_, manager->listen());
        return manager;
    }

    /// Runs @p body, then @p stop, and the io_context until nothing is left.
    template <typename Body>
    void run(Body body, std::function<void()> stop) {
        mcp_test::spawn_detached(io_ctx_, [this, &body, stop = std::move(stop)]() -> mcp::Task<void> {
            co_await body();
            stop();
            guard_.cancel();
        });
        io_ctx_.run();
    }
};

// The server's request reaches the client on the stream of the tool call while the tool waits for
// its answer; with the answer posted, the same stream carries the result.
TEST_F(LegacyStreamTest, SessionManagerSendsTheServersRequestWhileTheToolWaits) {
    constexpr unsigned short port = 19451;
    auto manager = make_manager(port);
    json question;
    unsigned answer_status = 0;
    json result;
    bool ended = false;
    std::string content_type;
    run(
        [&]() -> mcp::Task<void> {
            const auto executor = io_ctx_.get_executor();
            const auto session = co_await start_session(executor, port);
            auto exchange = std::make_shared<Exchange>(executor);
            const auto body = call_tool(2, "ask").dump();
            const auto headers = session_headers(session);
            co_await exchange->open(port, http::verb::post, body, headers);
            content_type = exchange->content_type();
            question = (co_await exchange->next_message()).value_or(json());
            const json answer = {
                {"jsonrpc", "2.0"},
                {"id", question.value("id", json())},
                {"result", {{"action", "accept"}, {"content", {{"name", "octocat"}}}}}};
            const auto answer_text = answer.dump();
            const auto answered = co_await send(executor, port, http::verb::post, answer_text, session);
            answer_status = answered.status;
            result = (co_await exchange->next_message()).value_or(json());
            ended = !(co_await exchange->next());
        },
        [&] { manager->close(); });

    EXPECT_EQ(content_type, "text/event-stream");
    EXPECT_EQ(question.value("method", ""), "elicitation/create") << question;
    EXPECT_EQ(answer_status, 202U);
    EXPECT_EQ(result.value("id", 0), 2) << result;
    EXPECT_EQ(text_of(result), "octocat") << result;
    EXPECT_TRUE(ended);
}

// What a handler sends goes on the stream of its own request, whatever else is waiting.
TEST_F(LegacyStreamTest, SessionManagerSendsEachRequestsMessagesOnItsOwnStream) {
    constexpr unsigned short port = 19452;
    auto manager = make_manager(port);
    json first_a;
    json first_b;
    json result_a;
    json result_b;
    run(
        [&]() -> mcp::Task<void> {
            const auto executor = io_ctx_.get_executor();
            const auto session = co_await start_session(executor, port);
            auto a = std::make_shared<Exchange>(executor);
            auto b = std::make_shared<Exchange>(executor);
            // Named: GCC mishandles a braced temporary in a co_await operand.
            const auto body_a = call_tool(2, "wait", json{{"tag", "a"}}).dump();
            const auto body_b = call_tool(3, "wait", json{{"tag", "b"}}).dump();
            const auto headers = session_headers(session);
            co_await a->open(port, http::verb::post, body_a, headers);
            co_await b->open(port, http::verb::post, body_b, headers);
            first_a = (co_await a->next_message()).value_or(json());
            first_b = (co_await b->next_message()).value_or(json());
            shared_->released = true;
            result_a = (co_await a->next_message()).value_or(json());
            result_b = (co_await b->next_message()).value_or(json());
        },
        [&] { manager->close(); });

    EXPECT_EQ(first_a.value("method", ""), "notifications/message") << first_a;
    EXPECT_EQ(first_a["params"]["data"], "a") << first_a;
    EXPECT_EQ(first_b["params"]["data"], "b") << first_b;
    EXPECT_EQ(text_of(result_a), "done-a") << result_a;
    EXPECT_EQ(text_of(result_b), "done-b") << result_b;
}

// A GET opens the stream for what belongs to no request. It starts with an event that has an id
// and no data, so a client can resume it before anything else is sent, and a session has one.
TEST_F(LegacyStreamTest, SessionManagerGetStreamCarriesWhatBelongsToNoRequest) {
    constexpr unsigned short port = 19453;
    auto manager = make_manager(port);
    unsigned status = 0;
    std::optional<Event> priming;
    unsigned second_status = 0;
    json notification;
    run(
        [&]() -> mcp::Task<void> {
            const auto executor = io_ctx_.get_executor();
            const auto session = co_await start_session(executor, port);
            auto get = std::make_shared<Exchange>(executor);
            const auto headers = session_headers(session);
            const std::string no_body;
            co_await get->open(port, http::verb::get, no_body, headers);
            status = get->status();
            priming = co_await get->next();
            auto second = std::make_shared<Exchange>(executor);
            co_await second->open(port, http::verb::get, no_body, headers);
            second_status = second->status();
            co_await shared_->last_server()->notify_tools_list_changed();
            notification = (co_await get->next_message()).value_or(json());
        },
        [&] { manager->close(); });

    EXPECT_EQ(status, 200U);
    ASSERT_TRUE(priming.has_value());
    EXPECT_FALSE(priming->id.empty());
    EXPECT_TRUE(priming->data.empty());
    EXPECT_EQ(second_status, 409U);
    EXPECT_EQ(notification.value("method", ""), "notifications/tools/list_changed") << notification;
}

// Without a GET stream, what belongs to no request goes on the stream of the only POST waiting.
// With several waiting it is held, and goes on the stream of the next response, ahead of it.
TEST_F(LegacyStreamTest, SessionManagerSendsWhatBelongsToNoRequestOnAWaitingPostWithoutAGet) {
    constexpr unsigned short port = 19457;
    auto manager = make_manager(port);
    json only_log;
    std::vector<json> only_rest;
    json first_log;
    json second_log;
    std::vector<json> first_rest;
    std::vector<json> second_rest;
    run(
        [&]() -> mcp::Task<void> {
            const auto executor = io_ctx_.get_executor();
            const auto session = co_await start_session(executor, port);
            const auto headers = session_headers(session);

            auto only = std::make_shared<Exchange>(executor);
            const auto body_only = call_tool(2, "wait", json{{"tag", "only"}}).dump();
            co_await only->open(port, http::verb::post, body_only, headers);
            only_log = (co_await only->next_message()).value_or(json());
            co_await shared_->last_server()->notify_tools_list_changed();
            shared_->released = true;
            only_rest = co_await rest_of(only);

            shared_->released = false;
            auto a = std::make_shared<Exchange>(executor);
            auto b = std::make_shared<Exchange>(executor);
            const auto body_a = call_tool(3, "wait", json{{"tag", "a"}}).dump();
            const auto body_b = call_tool(4, "wait", json{{"tag", "b"}}).dump();
            co_await a->open(port, http::verb::post, body_a, headers);
            co_await b->open(port, http::verb::post, body_b, headers);
            first_log = (co_await a->next_message()).value_or(json());
            second_log = (co_await b->next_message()).value_or(json());
            co_await shared_->last_server()->notify_tools_list_changed();
            shared_->released = true;
            first_rest = co_await rest_of(a);
            second_rest = co_await rest_of(b);
        },
        [&] { manager->close(); });

    const auto method_of = [](const json& message) { return message.value("method", ""); };
    EXPECT_EQ(only_log["params"]["data"], "only") << only_log;
    ASSERT_EQ(only_rest.size(), 2U);
    EXPECT_EQ(method_of(only_rest[0]), "notifications/tools/list_changed") << only_rest[0];
    EXPECT_EQ(text_of(only_rest[1]), "done-only") << only_rest[1];

    EXPECT_EQ(first_log["params"]["data"], "a") << first_log;
    EXPECT_EQ(second_log["params"]["data"], "b") << second_log;
    // Both tools end together, so either response may be the next one.
    auto& carrier = first_rest.size() == 2 ? first_rest : second_rest;
    auto& other = first_rest.size() == 2 ? second_rest : first_rest;
    ASSERT_EQ(carrier.size(), 2U);
    ASSERT_EQ(other.size(), 1U);
    EXPECT_EQ(method_of(carrier[0]), "notifications/tools/list_changed") << carrier[0];
    EXPECT_TRUE(carrier[1].contains("result")) << carrier[1];
    EXPECT_TRUE(other[0].contains("result")) << other[0];
    EXPECT_EQ(text_of(first_rest.back()), "done-a") << first_rest.back();
    EXPECT_EQ(text_of(second_rest.back()), "done-b") << second_rest.back();
}

// A client that left a stream before its response resumes it with Last-Event-ID, and gets the rest
// of it live, ending with the response.
TEST_F(LegacyStreamTest, SessionManagerResumesAStreamTheClientLeft) {
    constexpr unsigned short port = 19454;
    auto manager = make_manager(port);
    json first;
    unsigned resume_status = 0;
    json result;
    run(
        [&]() -> mcp::Task<void> {
            const auto executor = io_ctx_.get_executor();
            const auto session = co_await start_session(executor, port);
            auto left = std::make_shared<Exchange>(executor);
            const auto body = call_tool(2, "wait", json{{"tag", "r"}}).dump();
            const auto post_headers = session_headers(session);
            co_await left->open(port, http::verb::post, body, post_headers);
            first = (co_await left->next_message()).value_or(json());
            const auto last_id = left->last_id();
            left->close();
            auto headers = session_headers(session);
            headers.emplace_back("Last-Event-ID", last_id);
            auto resumed = std::make_shared<Exchange>(executor);
            const std::string no_body;
            co_await resumed->open(port, http::verb::get, no_body, headers);
            resume_status = resumed->status();
            shared_->released = true;
            result = (co_await resumed->next_message()).value_or(json());
        },
        [&] { manager->close(); });

    EXPECT_EQ(first["params"]["data"], "r") << first;
    EXPECT_EQ(resume_status, 200U);
    EXPECT_EQ(result.value("id", 0), 2) << result;
    EXPECT_EQ(text_of(result), "done-r") << result;
}

// A response that comes while the stream writes is sent once the write ends, not held until the
// next keep-alive comment.
TEST_F(LegacyStreamTest, SessionManagerSendsWhatCameWhileItWrote) {
    constexpr unsigned short port = 19458;
    auto manager = make_manager(port);
    Flooded flooded;
    run(
        [&]() -> mcp::Task<void> {
            const auto executor = io_ctx_.get_executor();
            flooded = co_await call_flood(executor, port, shared_);
        },
        [&] { manager->close(); });

    expect_log_then_result(flooded);
}

// HttpServerTransport streams the answer to a POST the same way.
TEST_F(LegacyStreamTest, HttpServerTransportSendsTheServersRequestWhileTheToolWaits) {
    auto transport = std::make_shared<mcp::HttpServerTransport>(io_ctx_.get_executor(), "127.0.0.1", 0);
    const auto port = transport->port();
    auto server = make_server(shared_);
    mcp_test::spawn_detached(io_ctx_, transport->listen());
    mcp_test::spawn_detached(
        io_ctx_, [&]() -> mcp::Task<void> { co_await server->run(transport, io_ctx_.get_executor()); });
    json question;
    json result;
    json notification;
    run(
        [&]() -> mcp::Task<void> {
            const auto executor = io_ctx_.get_executor();
            const auto session = co_await start_session(executor, port);
            auto exchange = std::make_shared<Exchange>(executor);
            const auto body = call_tool(2, "ask").dump();
            const auto headers = session_headers(session);
            co_await exchange->open(port, http::verb::post, body, headers);
            question = (co_await exchange->next_message()).value_or(json());
            const json answer = {{"jsonrpc", "2.0"},
                                 {"id", question.value("id", json())},
                                 {"result", {{"action", "accept"}, {"content", {{"name", "hubot"}}}}}};
            const auto answer_text = answer.dump();
            co_await send(executor, port, http::verb::post, answer_text, session);
            result = (co_await exchange->next_message()).value_or(json());

            auto get = std::make_shared<Exchange>(executor);
            const std::string no_body;
            co_await get->open(port, http::verb::get, no_body, headers);
            co_await server->notify_tools_list_changed();
            notification = (co_await get->next_message()).value_or(json());
        },
        [&] { transport->close(); });

    EXPECT_EQ(question.value("method", ""), "elicitation/create") << question;
    EXPECT_EQ(text_of(result), "hubot") << result;
    EXPECT_EQ(notification.value("method", ""), "notifications/tools/list_changed") << notification;
}

// HttpServerTransport, without a GET stream, also sends what belongs to no request on the stream of
// the only POST waiting. With several waiting it has nowhere to go, and is dropped.
TEST_F(LegacyStreamTest, HttpServerTransportSendsWhatBelongsToNoRequestOnlyOnALonePost) {
    auto transport = std::make_shared<mcp::HttpServerTransport>(io_ctx_.get_executor(), "127.0.0.1", 0);
    const auto port = transport->port();
    auto server = make_server(shared_);
    mcp_test::spawn_detached(io_ctx_, transport->listen());
    mcp_test::spawn_detached(
        io_ctx_, [&]() -> mcp::Task<void> { co_await server->run(transport, io_ctx_.get_executor()); });
    json only_log;
    std::vector<json> only_rest;
    json first_log;
    json second_log;
    std::vector<json> first_rest;
    std::vector<json> second_rest;
    run(
        [&]() -> mcp::Task<void> {
            const auto executor = io_ctx_.get_executor();
            const auto session = co_await start_session(executor, port);
            const auto headers = session_headers(session);

            auto only = std::make_shared<Exchange>(executor);
            const auto body_only = call_tool(2, "wait", json{{"tag", "only"}}).dump();
            co_await only->open(port, http::verb::post, body_only, headers);
            only_log = (co_await only->next_message()).value_or(json());
            co_await server->notify_tools_list_changed();
            shared_->released = true;
            only_rest = co_await rest_of(only);

            shared_->released = false;
            auto a = std::make_shared<Exchange>(executor);
            auto b = std::make_shared<Exchange>(executor);
            const auto body_a = call_tool(3, "wait", json{{"tag", "a"}}).dump();
            const auto body_b = call_tool(4, "wait", json{{"tag", "b"}}).dump();
            co_await a->open(port, http::verb::post, body_a, headers);
            co_await b->open(port, http::verb::post, body_b, headers);
            first_log = (co_await a->next_message()).value_or(json());
            second_log = (co_await b->next_message()).value_or(json());
            co_await server->notify_tools_list_changed();
            shared_->released = true;
            first_rest = co_await rest_of(a);
            second_rest = co_await rest_of(b);
        },
        [&] { transport->close(); });

    const auto method_of = [](const json& message) { return message.value("method", ""); };
    EXPECT_EQ(only_log["params"]["data"], "only") << only_log;
    ASSERT_EQ(only_rest.size(), 2U);
    EXPECT_EQ(method_of(only_rest[0]), "notifications/tools/list_changed") << only_rest[0];
    EXPECT_EQ(text_of(only_rest[1]), "done-only") << only_rest[1];
    EXPECT_EQ(first_log["params"]["data"], "a") << first_log;
    EXPECT_EQ(second_log["params"]["data"], "b") << second_log;
    ASSERT_EQ(first_rest.size(), 1U);
    ASSERT_EQ(second_rest.size(), 1U);
    EXPECT_EQ(text_of(first_rest[0]), "done-a") << first_rest[0];
    EXPECT_EQ(text_of(second_rest[0]), "done-b") << second_rest[0];
}

// HttpServerTransport also sends a response that comes while the stream writes once the write ends.
TEST_F(LegacyStreamTest, HttpServerTransportSendsWhatCameWhileItWrote) {
    auto transport = std::make_shared<mcp::HttpServerTransport>(io_ctx_.get_executor(), "127.0.0.1", 0);
    const auto port = transport->port();
    auto server = make_server(shared_);
    mcp_test::spawn_detached(io_ctx_, transport->listen());
    mcp_test::spawn_detached(
        io_ctx_, [&]() -> mcp::Task<void> { co_await server->run(transport, io_ctx_.get_executor()); });
    Flooded flooded;
    run(
        [&]() -> mcp::Task<void> {
            const auto executor = io_ctx_.get_executor();
            flooded = co_await call_flood(executor, port, shared_);
        },
        [&] { transport->close(); });

    expect_log_then_result(flooded);
}

// A Client over HttpClientTransport answers the server's request while its call waits, sends calls
// side by side, and gets what the server sends outside any call on the GET stream.
TEST_F(LegacyStreamTest, ClientCallsOverlapAndGetTheServersRequestsAndNotifications) {
    constexpr unsigned short port = 19455;
    auto manager = make_manager(port);
    auto client = std::make_shared<mcp::Client>(
        std::make_shared<mcp::HttpClientTransport>(io_ctx_.get_executor(),
                                                   "http://127.0.0.1:" + std::to_string(port) + "/mcp"),
        io_ctx_.get_executor(), [] {
            mcp::ClientOptions options;
            options.protocol = mcp::ClientProtocol::legacy();
            return options;
        }());
    client->on_elicitation([](const mcp::ElicitRequestParams&) -> mcp::Task<mcp::ElicitResult> {
        mcp::ElicitResult result;
        result.action = mcp::ElicitAction::eAccept;
        result.content = json{{"name", "mona"}};
        co_return result;
    });
    std::atomic<int> list_changed{0};
    client->on_notification("notifications/tools/list_changed",
                            [&list_changed](const json&) { ++list_changed; });
    std::string asked;
    std::string first;
    std::string second;
    bool notified = false;
    run(
        [&]() -> mcp::Task<void> {
            co_await client->connect("streams-client", "1.0");
            const auto no_arguments = json::object();
            asked = text_of(co_await client->call_tool("ask", no_arguments));
            // Each call waits until both have started: one at a time, neither would end.
            auto executor = co_await asio::this_coro::executor;
            auto done = std::make_shared<std::atomic<int>>(0);
            mcp_test::spawn_detached(executor, [&]() -> mcp::Task<void> {
                const auto arguments = json{{"tag", "1"}};
                first = text_of(co_await client->call_tool("wait", arguments));
                ++*done;
            });
            mcp_test::spawn_detached(executor, [&]() -> mcp::Task<void> {
                const auto arguments = json{{"tag", "2"}};
                second = text_of(co_await client->call_tool("wait", arguments));
                ++*done;
            });
            co_await wait_until([this] {
                std::lock_guard lock(shared_->mutex);
                return !shared_->servers.empty();
            });
            asio::steady_timer settle(executor, std::chrono::milliseconds(50));
            co_await settle.async_wait(asio::use_awaitable);
            shared_->released = true;
            co_await wait_until([done] { return done->load() == 2; });
            co_await shared_->last_server()->notify_tools_list_changed();
            notified = co_await wait_until([&list_changed] { return list_changed.load() == 1; });
        },
        [&] {
            client->close();
            manager->close();
        });

    EXPECT_EQ(asked, "mona");
    EXPECT_EQ(first, "done-1");
    EXPECT_EQ(second, "done-2");
    EXPECT_TRUE(notified);
}

// A GET opens an event stream only for a client that accepts one; any other is answered 406.
TEST_F(LegacyStreamTest, SessionManagerRefusesAGetThatDoesNotAcceptEventStreams) {
    constexpr unsigned short port = 19456;
    auto manager = make_manager(port);
    unsigned json_only = 0;
    unsigned no_accept = 0;
    run(
        [&]() -> mcp::Task<void> {
            const auto executor = io_ctx_.get_executor();
            const auto session = co_await start_session(executor, port);
            json_only = co_await get_status(executor, port, session, "application/json");
            no_accept = co_await get_status(executor, port, session, "");
        },
        [&] { manager->close(); });

    EXPECT_EQ(json_only, 406U);
    EXPECT_EQ(no_accept, 406U);
}

TEST_F(LegacyStreamTest, HttpServerTransportRefusesAGetThatDoesNotAcceptEventStreams) {
    auto transport = std::make_shared<mcp::HttpServerTransport>(io_ctx_.get_executor(), "127.0.0.1", 0);
    const auto port = transport->port();
    auto server = make_server(shared_);
    mcp_test::spawn_detached(io_ctx_, transport->listen());
    mcp_test::spawn_detached(
        io_ctx_, [&]() -> mcp::Task<void> { co_await server->run(transport, io_ctx_.get_executor()); });
    unsigned json_only = 0;
    unsigned no_accept = 0;
    run(
        [&]() -> mcp::Task<void> {
            const auto executor = io_ctx_.get_executor();
            const auto session = co_await start_session(executor, port);
            json_only = co_await get_status(executor, port, session, "application/json");
            no_accept = co_await get_status(executor, port, session, "");
        },
        [&] { transport->close(); });

    EXPECT_EQ(json_only, 406U);
    EXPECT_EQ(no_accept, 406U);
}

}  // namespace

}  // namespace transport_http_legacy_stream_test

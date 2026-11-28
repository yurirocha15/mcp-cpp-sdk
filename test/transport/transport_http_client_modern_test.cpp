#include "support/hang_guard.hpp"
#include "support/test_utils.hpp"

#include "mcp/client/client.hpp"
#include "mcp/core/async.hpp"
#include "mcp/detail/net/header_value.hpp"
#include "mcp/server/server.hpp"
#include "mcp/transport/http_client.hpp"
#include "mcp/transport/http_server.hpp"
#include "mcp/transport/http_session_manager.hpp"
#include "support/spawn.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
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
#include <cstdint>
#include <functional>
#include <map>
#include <mcp/core/constants.hpp>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace transport_http_client_modern_test {

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using nlohmann::json;
using Headers = std::vector<std::pair<std::string, std::string>>;

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char symbol) { return static_cast<char>(std::tolower(symbol)); });
    return text;
}

/// One HTTP request as the server saw it.
struct Recorded {
    std::string method;
    std::string target;
    Headers headers;
    std::string body;

    [[nodiscard]] std::optional<std::string> header(const std::string& name) const {
        for (const auto& [key, value] : headers) {
            if (lower(key) == lower(name)) {
                return value;
            }
        }
        return std::nullopt;
    }
    [[nodiscard]] json parsed() const { return json::parse(body, nullptr, false); }
    [[nodiscard]] std::string rpc_method() const {
        const auto message = parsed();
        return message.is_object() ? message.value("method", "") : "";
    }
};

/// A step of a streamed answer: bytes to send, or a condition to wait for before the next step.
struct Step {
    std::string chunk;
    std::function<bool()> wait_for;
};

struct Reply {
    unsigned int status{200};
    std::string content_type{"application/json"};
    std::string body;
    Headers headers;
    /// When not empty the answer is a chunked text/event-stream made of these steps.
    std::vector<Step> stream;
    /// When set, nothing of the answer is sent until this holds, as for a handler that stays quiet.
    std::function<bool()> hold_until;
    /// When true, the connection is closed after the stream's steps instead of ending the stream.
    bool drop_after_stream{false};
};

std::string sse_event(const json& message) { return "data: " + message.dump() + "\n\n"; }

mcp::Task<bool> wait_until(std::function<bool()> condition) {
    auto executor = co_await asio::this_coro::executor;
    const auto give_up = std::chrono::steady_clock::now() + g_hang_guard;
    while (!condition() && std::chrono::steady_clock::now() < give_up) {
        asio::steady_timer timer(executor, std::chrono::milliseconds(1));
        co_await timer.async_wait(asio::use_awaitable);
    }
    co_return condition();
}

/// A scripted HTTP server on an ephemeral port that records what it receives.
class RawServer {
   public:
    using Handler = std::function<Reply(const Recorded&)>;

    RawServer(asio::io_context& io, Handler handler)
        : io_(io),
          acceptor_(io, asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0)),
          handler_(std::move(handler)) {}

    [[nodiscard]] unsigned short port() const { return acceptor_.local_endpoint().port(); }
    [[nodiscard]] std::string url() const {
        return "http://127.0.0.1:" + std::to_string(port()) + "/mcp";
    }

    void start() { mcp_test::spawn_detached(io_, accept_loop(this)); }

    void close() {
        closing_ = true;
        beast::error_code ignored;
        (void)acceptor_.close(ignored);
        for (auto& weak : sockets_) {
            if (auto socket = weak.lock()) {
                (void)socket->close(ignored);
            }
        }
    }

    std::vector<Recorded> requests;
    /// Streams whose client went away before the stream was over.
    int streams_abandoned{0};
    int streams_started{0};
    /// Requests whose client went away before any of the answer was sent.
    int answers_abandoned{0};

    [[nodiscard]] std::vector<std::string> rpc_methods() const {
        std::vector<std::string> methods;
        for (const auto& request : requests) {
            methods.push_back(request.rpc_method());
        }
        return methods;
    }

   private:
    static mcp::Task<void> accept_loop(RawServer* self) {
        while (!self->closing_) {
            auto socket = std::make_shared<asio::ip::tcp::socket>(self->io_);
            try {
                co_await self->acceptor_.async_accept(*socket, asio::use_awaitable);
            } catch (const std::exception&) {
                co_return;
            }
            self->sockets_.push_back(socket);
            mcp_test::spawn_detached(self->io_, serve_connection(self, socket));
        }
    }

    /// Sets @p gone when the client ends the connection. Reads nothing, so the next request the
    /// client sends on the connection once the answer is over stays for serve_connection.
    static mcp::Task<void> watch_for_eof(std::shared_ptr<asio::ip::tcp::socket> socket,
                                         std::shared_ptr<bool> gone) {
        boost::system::error_code error;
        co_await socket->async_wait(asio::ip::tcp::socket::wait_read,
                                    asio::redirect_error(asio::use_awaitable, error));
        if (error) {
            *gone = true;
            co_return;
        }
        std::array<char, 1> byte{};
        socket->non_blocking(true, error);
        const auto peeked =
            socket->receive(asio::buffer(byte), asio::ip::tcp::socket::message_peek, error);
        boost::system::error_code ignored;
        socket->non_blocking(false, ignored);
        // Bytes waiting are the next request; would_block means serve_connection read them first.
        if (peeked == 0 && error != asio::error::would_block) {
            *gone = true;
        }
    }

    static std::string head(const Reply& reply, bool chunked) {
        std::string text = "HTTP/1.1 " + std::to_string(reply.status) + " " +
                           std::string(http::obsolete_reason(static_cast<http::status>(reply.status))) +
                           "\r\n";
        if (reply.status != 202 || !reply.body.empty()) {
            text += "Content-Type: " + reply.content_type + "\r\n";
        }
        if (chunked) {
            text += "Transfer-Encoding: chunked\r\n";
        } else {
            text += "Content-Length: " + std::to_string(reply.body.size()) + "\r\n";
        }
        for (const auto& [name, value] : reply.headers) {
            text += name + ": " + value + "\r\n";
        }
        return text + "\r\n";
    }

    static mcp::Task<void> stream_reply(RawServer* self, std::shared_ptr<asio::ip::tcp::socket> socket,
                                        Reply reply) {
        ++self->streams_started;
        auto gone = std::make_shared<bool>(false);
        mcp_test::spawn_detached(self->io_, watch_for_eof(socket, gone));
        auto written = std::make_shared<std::string>(head(reply, true));
        co_await asio::async_write(*socket, asio::buffer(*written), asio::use_awaitable);
        for (const auto& step : reply.stream) {
            if (step.wait_for) {
                const auto condition = step.wait_for;
                const auto ready = [condition, gone] { return *gone || condition(); };
                co_await wait_until(ready);
            }
            if (*gone) {
                ++self->streams_abandoned;
                co_return;
            }
            if (!step.chunk.empty()) {
                auto frame = std::make_shared<std::string>();
                std::array<char, 24> size{};
                std::snprintf(size.data(), size.size(), "%zx", step.chunk.size());
                *frame = std::string(size.data()) + "\r\n" + step.chunk + "\r\n";
                co_await asio::async_write(*socket, asio::buffer(*frame), asio::use_awaitable);
            }
        }
        if (reply.drop_after_stream) {
            beast::error_code ignored;
            (void)socket->shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
            (void)socket->close(ignored);
            co_return;
        }
        auto last = std::make_shared<std::string>("0\r\n\r\n");
        co_await asio::async_write(*socket, asio::buffer(*last), asio::use_awaitable);
    }

    static mcp::Task<void> serve_connection(RawServer* self,
                                            std::shared_ptr<asio::ip::tcp::socket> socket) {
        for (;;) {
            beast::flat_buffer buffer;
            http::request<http::string_body> request;
            try {
                co_await http::async_read(*socket, buffer, request, asio::use_awaitable);
            } catch (const std::exception&) {
                co_return;
            }
            Recorded recorded;
            recorded.method = std::string(request.method_string());
            recorded.target = std::string(request.target());
            for (const auto& field : request) {
                recorded.headers.emplace_back(std::string(field.name_string()),
                                              std::string(field.value()));
            }
            recorded.body = request.body();
            self->requests.push_back(recorded);

            Reply reply = self->handler_(recorded);
            try {
                if (reply.hold_until) {
                    auto gone = std::make_shared<bool>(false);
                    mcp_test::spawn_detached(self->io_, watch_for_eof(socket, gone));
                    const auto condition = reply.hold_until;
                    // Named: GCC 11 destroys a lambda temporary in a co_await operand twice.
                    const auto ready = [condition, gone] { return *gone || condition(); };
                    co_await wait_until(ready);
                    if (*gone) {
                        ++self->answers_abandoned;
                        co_return;
                    }
                }
                if (!reply.stream.empty()) {
                    try {
                        co_await stream_reply(self, socket, std::move(reply));
                    } catch (const std::exception&) {
                        // A write failed: the client left before the watcher noticed.
                        ++self->streams_abandoned;
                        co_return;
                    }
                } else {
                    auto text = std::make_shared<std::string>(head(reply, false) + reply.body);
                    co_await asio::async_write(*socket, asio::buffer(*text), asio::use_awaitable);
                }
            } catch (const std::exception&) {
                co_return;
            }
            if (!request.keep_alive()) {
                co_return;
            }
        }
    }

    asio::io_context& io_;
    asio::ip::tcp::acceptor acceptor_;
    Handler handler_;
    bool closing_{false};
    std::vector<std::weak_ptr<asio::ip::tcp::socket>> sockets_;
};

json discover_result() {
    return {{"resultType", "complete"},
            {"supportedVersions", json::array({"2026-07-28"})},
            {"capabilities", {{"tools", json::object()}}},
            {"_meta", {{"io.modelcontextprotocol/serverInfo", {{"name", "raw"}, {"version", "1"}}}}}};
}

json legacy_initialize_result() {
    return {{"protocolVersion", "2025-11-25"},
            {"capabilities", {{"tools", json::object()}}},
            {"serverInfo", {{"name", "raw-legacy"}, {"version", "1"}}}};
}

json rpc_result(const json& request, json result) {
    return {{"jsonrpc", "2.0"}, {"id", request.at("id")}, {"result", std::move(result)}};
}

json rpc_error(const json& request, int code, std::string message, std::optional<json> data = {}) {
    json error = {{"code", code}, {"message", std::move(message)}};
    if (data) {
        error["data"] = std::move(*data);
    }
    return {{"jsonrpc", "2.0"}, {"id", request.at("id")}, {"error", std::move(error)}};
}

Reply json_reply(const json& message, unsigned int status = 200) {
    Reply reply;
    reply.status = status;
    reply.body = message.dump();
    return reply;
}

/// The tool a raw server lists: header-annotated arguments of every type the specification allows.
json route_tool() {
    return {{"name", "route"},
            {"description", "Mirrors arguments into headers"},
            {"inputSchema",
             {{"type", "object"},
              {"properties",
               {{"region", {{"type", "string"}, {"x-mcp-header", "Region"}}},
                {"count", {{"type", "integer"}, {"x-mcp-header", "Count"}}},
                {"flag", {{"type", "boolean"}, {"x-mcp-header", "Flag"}}},
                {"query", {{"type", "string"}}},
                {"scope",
                 {{"type", "object"},
                  {"properties", {{"tenant", {{"type", "string"}, {"x-mcp-header", "Tenant"}}}}}}}}}}}};
}

/// What a modern server answers to the requests these tests make.
Reply modern_reply(const Recorded& request, const json& tools) {
    const auto message = request.parsed();
    const auto method = message.value("method", "");
    if (method == "server/discover") {
        return json_reply(rpc_result(message, discover_result()));
    }
    if (method == "tools/list") {
        return json_reply(rpc_result(message, {{"resultType", "complete"}, {"tools", tools}}));
    }
    if (method == "tools/call") {
        return json_reply(
            rpc_result(message, {{"resultType", "complete"},
                                 {"content", json::array({{{"type", "text"}, {"text", "ok"}}})}}));
    }
    if (method == "prompts/get") {
        return json_reply(
            rpc_result(message, {{"resultType", "complete"}, {"messages", json::array()}}));
    }
    if (method == "resources/read") {
        return json_reply(rpc_result(
            message,
            {{"resultType", "complete"},
             {"contents", json::array({{{"uri", message["params"]["uri"]}, {"text", "doc"}}})}}));
    }
    return json_reply(rpc_error(message, mcp::constants::jsonrpc::g_METHOD_NOT_FOUND, "no such method"),
                      404);
}

/// Header names a request carries beyond the ones every HTTP request has.
std::map<std::string, std::string> mcp_headers(const Recorded& request) {
    static const std::set<std::string> g_plain = {"host", "content-type", "content-length", "accept"};
    std::map<std::string, std::string> found;
    for (const auto& [name, value] : request.headers) {
        if (g_plain.count(lower(name)) == 0) {
            found[lower(name)] = value;
        }
    }
    return found;
}

using Expected = std::map<std::string, std::string>;

/// Runs @p body on the server's io_context with a guard against hangs, then closes @p raw.
void run_guarded(asio::io_context& io, RawServer& raw, std::function<mcp::Task<void>()> body) {
    asio::steady_timer guard(io);
    bool fired = false;
    guard.expires_after(g_hang_guard);
    guard.async_wait([&](const boost::system::error_code& error) {
        if (!error) {
            fired = true;
            io.stop();
        }
    });
    raw.start();
    mcp_test::spawn_detached(io, [&guard, &raw, body = std::move(body)]() -> mcp::Task<void> {
        co_await body();
        raw.close();
        guard.cancel();
    });
    io.run();
    EXPECT_FALSE(fired) << "the test did not finish";
}

mcp::ClientOptions modern_options() {
    mcp::ClientOptions options;
    options.protocol = mcp::ClientProtocol::pinned("2026-07-28");
    return options;
}

mcp::ClientOptions auto_options() {
    mcp::ClientOptions options;
    options.protocol = mcp::ClientProtocol::automatic();
    return options;
}

std::shared_ptr<mcp::HttpClientTransport> make_transport(asio::io_context& io, const std::string& url) {
    return std::make_shared<mcp::HttpClientTransport>(io.get_executor(), url);
}

/// The error code a task's exception carries, 0 when it completes.
template <typename Make>
mcp::Task<int> failure_code(Make make) {
    try {
        co_await make();
    } catch (const mcp::McpError& error) {
        co_return error.code();
    }
    co_return 0;
}

}  // namespace

TEST(HttpClientModern, HeadersExactPerSpecTable) {
    asio::io_context io;
    RawServer raw(
        io, [](const Recorded& request) { return modern_reply(request, json::array({route_tool()})); });
    auto transport = make_transport(io, raw.url());
    mcp::Client client(transport, io.get_executor(), modern_options());

    run_guarded(io, raw, [&]() -> mcp::Task<void> {
        co_await client.connect("headers", "1.0");
        co_await client.list_tools();
        json full_arguments = {{"region", "us-west1"},
                               {"count", 42},
                               {"flag", true},
                               {"query", "SELECT 1"},
                               {"scope", {{"tenant", "acme"}}}};
        co_await client.call_tool("route", full_arguments);
        json plain_arguments = {{"query", "x"}};
        co_await client.call_tool("route", plain_arguments);
        co_await client.get_prompt("greet");
        co_await client.read_resource("test://doc");
        client.close();
    });

    const std::string version = "2026-07-28";
    const std::vector<std::pair<std::string, Expected>> table = {
        {"server/discover", {{"mcp-protocol-version", version}, {"mcp-method", "server/discover"}}},
        {"tools/list", {{"mcp-protocol-version", version}, {"mcp-method", "tools/list"}}},
        {"tools/call",
         {{"mcp-protocol-version", version},
          {"mcp-method", "tools/call"},
          {"mcp-name", "route"},
          {"mcp-param-region", "us-west1"},
          {"mcp-param-count", "42"},
          {"mcp-param-flag", "true"},
          {"mcp-param-tenant", "acme"}}},
        {"tools/call",
         {{"mcp-protocol-version", version}, {"mcp-method", "tools/call"}, {"mcp-name", "route"}}},
        {"prompts/get",
         {{"mcp-protocol-version", version}, {"mcp-method", "prompts/get"}, {"mcp-name", "greet"}}},
        {"resources/read",
         {{"mcp-protocol-version", version},
          {"mcp-method", "resources/read"},
          {"mcp-name", "test://doc"}}},
    };
    ASSERT_EQ(raw.requests.size(), table.size());
    for (std::size_t index = 0; index < table.size(); ++index) {
        const auto& request = raw.requests[index];
        SCOPED_TRACE(request.body);
        EXPECT_EQ(request.method, "POST");
        EXPECT_EQ(request.rpc_method(), table[index].first);
        EXPECT_EQ(mcp_headers(request), table[index].second);
        EXPECT_EQ(request.header("Accept"), "application/json, text/event-stream");
        EXPECT_EQ(request.parsed()["params"]["_meta"]["io.modelcontextprotocol/protocolVersion"],
                  version);
    }
}

// The specification limits an integer header to the JavaScript safe range, so a value beyond it
// gets no Mcp-Param header, whether it is held as signed or as unsigned.
TEST(HttpClientModern, IntegerOutsideTheSafeRangeGetsNoParamHeader) {
    asio::io_context io;
    RawServer raw(
        io, [](const Recorded& request) { return modern_reply(request, json::array({route_tool()})); });
    auto transport = make_transport(io, raw.url());
    mcp::Client client(transport, io.get_executor(), modern_options());

    constexpr std::int64_t largest = (std::int64_t{1} << 53) - 1;
    run_guarded(io, raw, [&]() -> mcp::Task<void> {
        co_await client.connect("range", "1.0");
        co_await client.list_tools();
        const std::vector<json> counts = {json(largest), json(largest + 1), json(-largest),
                                          json(-largest - 1), json(std::uint64_t{1} << 53)};
        for (const auto& count : counts) {
            const json arguments = {{"count", count}};
            co_await client.call_tool("route", arguments);
        }
        client.close();
    });

    const std::vector<std::string> expected = {"9007199254740991", "", "-9007199254740991", "", ""};
    ASSERT_EQ(raw.requests.size(), 2 + expected.size());
    for (std::size_t index = 0; index < expected.size(); ++index) {
        const auto& request = raw.requests[2 + index];
        SCOPED_TRACE(request.body);
        EXPECT_EQ(request.rpc_method(), "tools/call");
        const auto headers = mcp_headers(request);
        const auto found = headers.find("mcp-param-count");
        EXPECT_EQ(found == headers.end() ? "" : found->second, expected[index]);
    }
}

// An integer argument written as a float with no fraction is sent as that integer, within the safe
// range; one with a fraction gets no header. A false boolean is sent as false.
TEST(HttpClientModern, IntegralFloatsAndFalseGetParamHeaders) {
    asio::io_context io;
    RawServer raw(
        io, [](const Recorded& request) { return modern_reply(request, json::array({route_tool()})); });
    auto transport = make_transport(io, raw.url());
    mcp::Client client(transport, io.get_executor(), modern_options());

    const std::vector<json> calls = {json{{"count", 42.0}}, json{{"count", -9007199254740991.0}},
                                     json{{"count", 1.5}}, json{{"count", 9007199254740992.0}},
                                     json{{"flag", false}}};
    run_guarded(io, raw, [&]() -> mcp::Task<void> {
        co_await client.connect("floats", "1.0");
        co_await client.list_tools();
        for (const auto& arguments : calls) {
            co_await client.call_tool("route", arguments);
        }
        client.close();
    });

    const std::vector<Expected> expected = {{{"mcp-param-count", "42"}},
                                            {{"mcp-param-count", "-9007199254740991"}},
                                            {},
                                            {},
                                            {{"mcp-param-flag", "false"}}};
    ASSERT_EQ(raw.requests.size(), 2 + expected.size());
    for (std::size_t index = 0; index < expected.size(); ++index) {
        const auto& request = raw.requests[2 + index];
        SCOPED_TRACE(request.body);
        auto params = mcp_headers(request);
        for (auto iter = params.begin(); iter != params.end();) {
            iter = iter->first.rfind("mcp-param-", 0) == 0 ? std::next(iter) : params.erase(iter);
        }
        EXPECT_EQ(params, expected[index]);
    }
}

TEST(HttpClientModern, Base64NameAndParam) {
    asio::io_context io;
    json tool = route_tool();
    tool["name"] = "héllo wörld";
    RawServer raw(io,
                  [&](const Recorded& request) { return modern_reply(request, json::array({tool})); });
    auto transport = make_transport(io, raw.url());
    mcp::Client client(transport, io.get_executor(), modern_options());

    const std::vector<std::string> regions = {"Hello, 世界", " padded ",
                                              "=?base64?literal?=", "line1\nline2", "plain"};
    run_guarded(io, raw, [&]() -> mcp::Task<void> {
        co_await client.connect("headers", "1.0");
        co_await client.list_tools();
        for (const auto& region : regions) {
            json arguments = {{"region", region}};
            co_await client.call_tool("héllo wörld", arguments);
        }
        client.close();
    });

    const std::vector<std::string> expected = {
        "=?base64?SGVsbG8sIOS4lueVjA==?=", "=?base64?IHBhZGRlZCA=?=",
        "=?base64?PT9iYXNlNjQ/bGl0ZXJhbD89?=", "=?base64?bGluZTEKbGluZTI=?=", "plain"};
    ASSERT_EQ(raw.requests.size(), 2 + regions.size());
    for (std::size_t index = 0; index < regions.size(); ++index) {
        const auto& request = raw.requests[2 + index];
        EXPECT_EQ(request.header("Mcp-Name"), "=?base64?aMOpbGxvIHfDtnJsZA==?=");
        EXPECT_EQ(request.header("Mcp-Param-Region"), expected[index]);
        // What the server decodes is what the body carries.
        EXPECT_EQ(mcp::detail::decode_header_value(*request.header("Mcp-Param-Region")),
                  regions[index]);
        EXPECT_EQ(request.parsed()["params"]["arguments"]["region"], regions[index]);
    }
}

// A bearer token over plain http:// to another host is readable on the path: the transport warns
// once, before its first request leaves. A loopback server gets no warning.
TEST(HttpClientModern, WarnsOnceAboutBearerTokensOverPlainHttpToAnotherHost) {
    asio::io_context io;
    const auto warnings_for = [&io](const std::string& url, bool with_token) {
        std::vector<std::string> warnings;
        auto transport = make_transport(io, url);
        transport->set_warning_handler(
            [&warnings](const std::string& text) { warnings.push_back(text); });
        if (with_token) {
            transport->set_bearer_token_provider([] { return std::string("secret"); });
        }
        // The provider is read where a request starts, so creating the task is enough; nothing is
        // sent, as the task never runs.
        {
            auto first = transport->write_message("{}");
        }
        {
            auto second = transport->write_message("{}");
        }
        return warnings;
    };

    const auto remote = warnings_for("http://mcp.example.test/mcp", true);
    ASSERT_EQ(remote.size(), 1U);
    EXPECT_NE(remote[0].find("mcp.example.test"), std::string::npos) << remote[0];
    EXPECT_NE(remote[0].find("https://"), std::string::npos) << remote[0];
    EXPECT_EQ(remote[0].find("secret"), std::string::npos) << remote[0];

    EXPECT_TRUE(warnings_for("http://mcp.example.test/mcp", false).empty());
    EXPECT_TRUE(warnings_for("http://localhost:8080/mcp", true).empty());
    EXPECT_TRUE(warnings_for("http://127.0.0.1:8080/mcp", true).empty());
    EXPECT_TRUE(warnings_for("http://[::1]:8080/mcp", true).empty());
    EXPECT_TRUE(warnings_for("https://mcp.example.test/mcp", true).empty());
}

TEST(HttpClientModern, InvalidXMcpHeaderToolsExcluded) {
    const auto tool = [](const std::string& name, const json& properties) {
        return json{{"name", name}, {"inputSchema", {{"type", "object"}, {"properties", properties}}}};
    };
    const json tools = json::array(
        {tool("good", {{"region", {{"type", "string"}, {"x-mcp-header", "Region"}}}}),
         tool("bad_number", {{"ratio", {{"type", "number"}, {"x-mcp-header", "Ratio"}}}}),
         tool("bad_duplicate", {{"a", {{"type", "string"}, {"x-mcp-header", "Same"}}},
                                {"b", {{"type", "string"}, {"x-mcp-header", "SAME"}}}}),
         tool("bad_token", {{"a", {{"type", "string"}, {"x-mcp-header", "has space"}}}}),
         tool("bad_empty", {{"a", {{"type", "string"}, {"x-mcp-header", ""}}}}),
         tool("bad_array",
              {{"a", {{"type", "array"}, {"items", {{"type", "string"}, {"x-mcp-header", "Item"}}}}}}),
         tool("plain", {{"query", {{"type", "string"}}}})});
    asio::io_context io;
    RawServer raw(io, [&](const Recorded& request) { return modern_reply(request, tools); });
    auto transport = make_transport(io, raw.url());
    std::vector<std::string> warnings;
    transport->set_warning_handler([&warnings](const std::string& text) { warnings.push_back(text); });
    mcp::Client client(transport, io.get_executor(), modern_options());

    std::vector<std::string> listed;
    run_guarded(io, raw, [&]() -> mcp::Task<void> {
        co_await client.connect("headers", "1.0");
        const auto result = co_await client.list_tools();
        for (const auto& listed_tool : result.tools) {
            listed.push_back(listed_tool.name);
        }
        json arguments = {{"region", "eu"}};
        co_await client.call_tool("good", arguments);
        client.close();
    });

    EXPECT_EQ(listed, (std::vector<std::string>{"good", "plain"}));
    ASSERT_EQ(warnings.size(), 5u);
    for (const char* name : {"bad_number", "bad_duplicate", "bad_token", "bad_empty", "bad_array"}) {
        EXPECT_EQ(std::count_if(warnings.begin(), warnings.end(),
                                [&](const std::string& text) {
                                    return text.find(std::string("\"") + name + "\"") !=
                                           std::string::npos;
                                }),
                  1)
            << name;
    }
    ASSERT_EQ(raw.requests.size(), 3u);
    EXPECT_EQ(raw.requests[2].header("Mcp-Param-Region"), "eu");
}

// A tools/list the server answers with an error reaches the caller as that error, and the header
// annotations of the last listing that succeeded stay in use.
TEST(HttpClientModern, AFailedToolsListingKeepsTheKnownHeaders) {
    int listings = 0;
    asio::io_context io;
    RawServer raw(io, [&](const Recorded& request) {
        if (request.rpc_method() == "tools/list" && ++listings == 2) {
            return json_reply(rpc_error(request.parsed(), mcp::constants::jsonrpc::g_INTERNAL_ERROR,
                                        "listing failed"));
        }
        return modern_reply(request, json::array({route_tool()}));
    });
    mcp::Client client(make_transport(io, raw.url()), io.get_executor(), modern_options());

    std::optional<int> code;
    run_guarded(io, raw, [&]() -> mcp::Task<void> {
        co_await client.connect("failed-listing", "1.0");
        co_await client.list_tools();
        try {
            co_await client.list_tools();
        } catch (const mcp::McpError& failure) {
            code = failure.code();
        }
        const json arguments = {{"region", "eu"}};
        co_await client.call_tool("route", arguments);
        client.close();
    });

    EXPECT_EQ(code, mcp::constants::jsonrpc::g_INTERNAL_ERROR);
    ASSERT_EQ(raw.requests.size(), 4U);
    EXPECT_EQ(raw.requests[3].rpc_method(), "tools/call");
    EXPECT_EQ(raw.requests[3].header("Mcp-Param-Region"), "eu");
}

// The header annotations of a tools/list read in pages add up across the pages, and a listing that
// starts over without a cursor forgets the tools it no longer lists.
TEST(HttpClientModern, ToolHeadersAddUpAcrossPagesAndStartOverWithANewListing) {
    const auto header_tool = [](const std::string& name, const std::string& argument,
                                const std::string& header) {
        return json{{"name", name},
                    {"inputSchema",
                     {{"type", "object"},
                      {"properties", {{argument, {{"type", "string"}, {"x-mcp-header", header}}}}}}}};
    };
    int listings = 0;
    asio::io_context io;
    RawServer raw(io, [&](const Recorded& request) {
        if (request.rpc_method() != "tools/list") {
            return modern_reply(request, json::array());
        }
        const auto message = request.parsed();
        json result = {{"resultType", "complete"}};
        if (message.value("params", json::object()).contains("cursor")) {
            result["tools"] = json::array({header_tool("b", "zone", "Zone")});
        } else if (++listings == 1) {
            result["tools"] = json::array({header_tool("a", "region", "Region")});
            result["nextCursor"] = "2";
        } else {
            result["tools"] = json::array({header_tool("b", "zone", "Zone")});
        }
        return json_reply(rpc_result(message, result));
    });
    mcp::Client client(make_transport(io, raw.url()), io.get_executor(), modern_options());

    run_guarded(io, raw, [&]() -> mcp::Task<void> {
        co_await client.connect("pages", "1.0");
        co_await client.list_tools();
        const std::optional<std::string> next = "2";
        co_await client.list_tools(next);
        json region = {{"region", "eu"}};
        json zone = {{"zone", "z1"}};
        co_await client.call_tool("a", region);
        co_await client.call_tool("b", zone);
        co_await client.list_tools();
        co_await client.call_tool("a", region);
        co_await client.call_tool("b", zone);
        client.close();
    });

    ASSERT_EQ(raw.requests.size(), 8u);
    for (const std::size_t call : {3u, 4u, 6u, 7u}) {
        EXPECT_EQ(raw.requests[call].rpc_method(), "tools/call") << raw.requests[call].body;
    }
    EXPECT_EQ(raw.requests[3].header("Mcp-Param-Region"), "eu");
    EXPECT_EQ(raw.requests[4].header("Mcp-Param-Zone"), "z1");
    EXPECT_FALSE(raw.requests[6].header("Mcp-Param-Region").has_value());
    EXPECT_EQ(raw.requests[7].header("Mcp-Param-Zone"), "z1");
}

TEST(HttpClientModern, HeaderMismatchRefreshThenRetry) {
    asio::io_context io;
    RawServer raw(io, [](const Recorded& request) {
        const auto message = request.parsed();
        if (message.value("method", "") == "tools/call" && !request.header("Mcp-Param-Region")) {
            return json_reply(rpc_error(message, mcp::constants::jsonrpc::g_HEADER_MISMATCH,
                                        "Missing Mcp-Param-Region header"),
                              400);
        }
        return modern_reply(request, json::array({route_tool()}));
    });
    auto transport = make_transport(io, raw.url());
    mcp::Client client(transport, io.get_executor(), modern_options());

    std::string text;
    run_guarded(io, raw, [&]() -> mcp::Task<void> {
        co_await client.connect("headers", "1.0");
        json arguments = {{"region", "eu"}, {"count", 3}};
        const auto result = co_await client.call_tool("route", arguments);
        text = std::get<mcp::TextContent>(result.content.at(0)).text;
        client.close();
    });

    EXPECT_EQ(text, "ok");
    EXPECT_EQ(raw.rpc_methods(),
              (std::vector<std::string>{"server/discover", "tools/call", "tools/list", "tools/call"}));
    ASSERT_EQ(raw.requests.size(), 4u);
    EXPECT_FALSE(raw.requests[1].header("Mcp-Param-Region").has_value());
    EXPECT_EQ(raw.requests[3].header("Mcp-Param-Region"), "eu");
    EXPECT_EQ(raw.requests[3].header("Mcp-Param-Count"), "3");
    // The retry is the same request.
    EXPECT_EQ(raw.requests[1].body, raw.requests[3].body);
}

TEST(HttpClientModern, HeaderMismatchIsRetriedOnlyOnce) {
    asio::io_context io;
    RawServer raw(io, [](const Recorded& request) {
        const auto message = request.parsed();
        if (message.value("method", "") == "tools/call") {
            return json_reply(
                rpc_error(message, mcp::constants::jsonrpc::g_HEADER_MISMATCH, "Header mismatch"), 400);
        }
        return modern_reply(request, json::array({route_tool()}));
    });
    auto transport = make_transport(io, raw.url());
    mcp::Client client(transport, io.get_executor(), modern_options());

    int code = 0;
    run_guarded(io, raw, [&]() -> mcp::Task<void> {
        co_await client.connect("headers", "1.0");
        code = co_await failure_code([&]() -> mcp::Task<void> {
            json arguments = {{"region", "eu"}};
            co_await client.call_tool("route", arguments);
        });
        client.close();
    });

    EXPECT_EQ(code, mcp::constants::jsonrpc::g_HEADER_MISMATCH);
    EXPECT_EQ(raw.rpc_methods(),
              (std::vector<std::string>{"server/discover", "tools/call", "tools/list", "tools/call"}));
}

namespace {

/// An Auto client probing a raw server whose probe answer is @p probe: the client ends up legacy.
void expect_legacy_fallback(Reply probe) {
    asio::io_context io;
    RawServer raw(io, [probe = std::move(probe)](const Recorded& request) {
        const auto message = request.parsed();
        const auto method = message.value("method", "");
        if (method == "server/discover") {
            return probe;
        }
        if (method == "initialize") {
            return json_reply(rpc_result(message, legacy_initialize_result()));
        }
        Reply accepted;
        accepted.status = 202;
        return accepted;
    });
    auto transport = make_transport(io, raw.url());
    mcp::ClientOptions options = auto_options();
    std::vector<mcp::EraDecision> decisions;
    options.on_era_decision = [&decisions](const mcp::EraDecision& decision) {
        decisions.push_back(decision);
    };
    mcp::Client client(transport, io.get_executor(), options);

    mcp::ConnectResult connected;
    run_guarded(io, raw, [&]() -> mcp::Task<void> {
        connected = co_await client.connect("fallback", "1.0");
        client.close();
    });

    EXPECT_EQ(connected.era, mcp::ProtocolEra::eLegacy);
    EXPECT_EQ(connected.protocolVersion, "2025-11-25");
    ASSERT_EQ(decisions.size(), 1u);
    EXPECT_EQ(decisions[0].reason, mcp::EraReason::eFallbackNonModernError);
    ASSERT_GE(raw.requests.size(), 2u);
    // The probe is a proper modern request; initialize is not.
    EXPECT_EQ(raw.requests[0].rpc_method(), "server/discover");
    EXPECT_EQ(raw.requests[0].header("Mcp-Method"), "server/discover");
    EXPECT_EQ(raw.requests[0].header("MCP-Protocol-Version"), "2026-07-28");
    EXPECT_EQ(raw.requests[1].rpc_method(), "initialize");
    EXPECT_FALSE(raw.requests[1].header("Mcp-Method").has_value());
    EXPECT_EQ(raw.requests[1].header("MCP-Protocol-Version"), "2025-11-25");
}

Reply status_reply(unsigned int status, std::string body = {}) {
    Reply reply;
    reply.status = status;
    reply.body = std::move(body);
    return reply;
}

}  // namespace

TEST(HttpClientModern, Fallback400ModernBodyStaysModern) {
    asio::io_context io;
    int probes = 0;
    RawServer raw(io, [&probes](const Recorded& request) {
        const auto message = request.parsed();
        if (message.value("method", "") == "server/discover" && ++probes == 1) {
            return json_reply(
                rpc_error(message, mcp::constants::jsonrpc::g_UNSUPPORTED_PROTOCOL_VERSION,
                          "Unsupported protocol version",
                          json{{"supported", json::array({"2026-07-28"})}}),
                400);
        }
        return modern_reply(request, json::array());
    });
    auto transport = make_transport(io, raw.url());
    mcp::ClientOptions options = auto_options();
    std::vector<mcp::EraDecision> decisions;
    options.on_era_decision = [&decisions](const mcp::EraDecision& decision) {
        decisions.push_back(decision);
    };
    mcp::Client client(transport, io.get_executor(), options);

    mcp::ConnectResult connected;
    run_guarded(io, raw, [&]() -> mcp::Task<void> {
        connected = co_await client.connect("stays", "1.0");
        client.close();
    });

    EXPECT_EQ(connected.era, mcp::ProtocolEra::eModern);
    ASSERT_EQ(decisions.size(), 1u);
    EXPECT_EQ(decisions[0].reason, mcp::EraReason::eModernErrorRetry);
    EXPECT_EQ(raw.rpc_methods(), (std::vector<std::string>{"server/discover", "server/discover"}));
}

TEST(HttpClientModern, Fallback400HeaderMismatchBodyDoesNotFallBack) {
    asio::io_context io;
    RawServer raw(io, [](const Recorded& request) {
        return json_reply(
            rpc_error(request.parsed(), mcp::constants::jsonrpc::g_HEADER_MISMATCH, "Header mismatch"),
            400);
    });
    auto transport = make_transport(io, raw.url());
    mcp::Client client(transport, io.get_executor(), auto_options());

    int code = 0;
    run_guarded(io, raw, [&]() -> mcp::Task<void> {
        code = co_await failure_code(
            [&]() -> mcp::Task<void> { co_await client.connect("stays", "1.0"); });
        client.close();
    });

    EXPECT_EQ(code, mcp::constants::jsonrpc::g_HEADER_MISMATCH);
    EXPECT_EQ(raw.rpc_methods(), (std::vector<std::string>{"server/discover"}));
}

TEST(HttpClientModern, Fallback400EmptyBodyGoesLegacy) { expect_legacy_fallback(status_reply(400)); }

TEST(HttpClientModern, Fallback400NonJsonBodyGoesLegacy) {
    expect_legacy_fallback(status_reply(400, "Bad Request"));
}

TEST(HttpClientModern, Fallback400UnrecognizedJsonRpcErrorGoesLegacy) {
    const json error = {
        {"jsonrpc", "2.0"},
        {"id", nullptr},
        {"error",
         {{"code", mcp::constants::jsonrpc::g_INVALID_REQUEST}, {"message", "Missing session"}}}};
    expect_legacy_fallback(status_reply(400, error.dump()));
}

TEST(HttpClientModern, Fallback404Legacy) { expect_legacy_fallback(status_reply(404)); }

TEST(HttpClientModern, Fallback405Legacy) { expect_legacy_fallback(status_reply(405)); }

TEST(HttpClientModern, NoSessionIdSentInModern) {
    asio::io_context io;
    RawServer raw(io, [](const Recorded& request) {
        Reply reply = modern_reply(request, json::array({route_tool()}));
        reply.headers.emplace_back("Mcp-Session-Id", "offered-session");
        if (request.rpc_method() == "tools/call") {
            reply.content_type = "text/event-stream";
            reply.body = "id: 5\ndata: " + reply.body + "\n\n";
        }
        return reply;
    });
    auto transport = make_transport(io, raw.url());
    mcp::Client client(transport, io.get_executor(), modern_options());

    run_guarded(io, raw, [&]() -> mcp::Task<void> {
        co_await client.connect("sessionless", "1.0");
        co_await client.list_tools();
        json arguments = {{"region", "eu"}};
        co_await client.call_tool("route", arguments);
        co_await client.call_tool("route", arguments);
        client.close();
    });

    ASSERT_EQ(raw.requests.size(), 4u);
    for (const auto& request : raw.requests) {
        EXPECT_EQ(request.method, "POST");
        EXPECT_FALSE(request.header("Mcp-Session-Id").has_value()) << request.body;
        EXPECT_FALSE(request.header("Last-Event-ID").has_value()) << request.body;
    }
    EXPECT_EQ(transport->session_id(), "");
    EXPECT_EQ(transport->last_event_id(), "");
}

// A 2025-11-25 server may answer a POST with a whole SSE body. Its lines may end in CRLF, a field
// name may be followed by no space, one message may span several data lines, and the last event id
// is kept for a resume.
TEST(HttpClientModern, ALegacySseAnswerWithCrlfLinesAndSplitDataIsRead) {
    asio::io_context io;
    RawServer raw(io, [](const Recorded& request) {
        if (request.method != "POST") {
            return status_reply(405);
        }
        const auto message = request.parsed();
        const auto method = message.value("method", "");
        Reply reply;
        reply.content_type = "text/event-stream";
        if (method == "initialize") {
            reply.body = "event: message\r\nid:1\r\ndata:" +
                         rpc_result(message, legacy_initialize_result()).dump() + "\r\n\r\n";
        } else if (method == "tools/list") {
            const auto text = rpc_result(message, json{{"tools", json::array({route_tool()})}}).dump();
            const auto comma = text.find(',');
            reply.body = "id: 2\r\ndata: " + text.substr(0, comma + 1) +
                         "\r\ndata: " + text.substr(comma + 1) + "\r\n\r\n";
        } else {
            reply = status_reply(202);
        }
        return reply;
    });
    auto transport = make_transport(io, raw.url());
    mcp::ClientOptions options;
    options.protocol = mcp::ClientProtocol::pinned("2025-11-25");
    mcp::Client client(transport, io.get_executor(), options);

    mcp::ConnectResult connected;
    std::vector<std::string> tool_names;
    run_guarded(io, raw, [&]() -> mcp::Task<void> {
        connected = co_await client.connect("legacy-sse", "1.0");
        for (const auto& tool : (co_await client.list_tools()).tools) {
            tool_names.push_back(tool.name);
        }
        client.close();
    });

    EXPECT_EQ(connected.protocolVersion, "2025-11-25");
    EXPECT_EQ(tool_names, std::vector<std::string>{"route"});
    EXPECT_EQ(transport->last_event_id(), "2");
}

TEST(HttpClientModern, StreamedNotificationsArriveBeforeTheResult) {
    asio::io_context io;
    bool progress_seen = false;
    RawServer raw(io, [&progress_seen](const Recorded& request) {
        if (request.rpc_method() != "tools/call") {
            return modern_reply(request, json::array());
        }
        const auto message = request.parsed();
        Reply reply;
        reply.content_type = "text/event-stream";
        const json progress = {{"jsonrpc", "2.0"},
                               {"method", "notifications/progress"},
                               {"params", {{"progressToken", "t1"}, {"progress", 1}, {"total", 2}}}};
        reply.stream.push_back({sse_event(progress), nullptr});
        // The result is held back until the client has handled the progress notification, which it
        // can only have done if the notification was read before the stream ended.
        const json result =
            rpc_result(message, {{"resultType", "complete"},
                                 {"content", json::array({{{"type", "text"}, {"text", "done"}}})}});
        reply.stream.push_back({sse_event(result), [&progress_seen] { return progress_seen; }});
        return reply;
    });
    auto transport = make_transport(io, raw.url());
    mcp::Client client(transport, io.get_executor(), modern_options());
    client.on_progress([&progress_seen](const mcp::ProgressNotificationParams& params) {
        if (params.progress == 1) {
            progress_seen = true;
        }
    });

    json result;
    run_guarded(io, raw, [&]() -> mcp::Task<void> {
        co_await client.connect("stream", "1.0");
        json params = {
            {"name", "slow"}, {"arguments", json::object()}, {"_meta", {{"progressToken", "t1"}}}};
        result = co_await client.send_request("tools/call", params);
        client.close();
    });

    EXPECT_TRUE(progress_seen);
    EXPECT_EQ(result["content"][0]["text"], "done");
}

// In the event stream of a 2025-11-25 session a field name may be followed by no space, an id that
// holds NUL is ignored, and so is a retry that is not a short decimal number, as the SSE standard
// has it.
TEST(HttpClientModern, StreamFieldsAreReadAsTheSseStandardHasIt) {
    asio::io_context io;
    RawServer raw(io, [](const Recorded& request) {
        if (request.method != "POST") {
            return status_reply(405);
        }
        const auto message = request.parsed();
        const auto method = message.value("method", "");
        if (method == "initialize") {
            return json_reply(rpc_result(message, legacy_initialize_result()));
        }
        if (method != "tools/call") {
            return status_reply(202);
        }
        const json result =
            rpc_result(message, {{"content", json::array({{{"type", "text"}, {"text", "done"}}})}});
        std::string events = "retry: soon\nretry:1234567890\nid:7\n: comment\n\nid: a";
        events.push_back('\0');
        events += "b\ndata:" + result.dump() + "\n\n";
        Reply reply;
        reply.content_type = "text/event-stream";
        reply.body = std::move(events);
        return reply;
    });
    auto transport = make_transport(io, raw.url());
    mcp::ClientOptions options;
    options.protocol = mcp::ClientProtocol::pinned("2025-11-25");
    mcp::Client client(transport, io.get_executor(), options);

    json result;
    run_guarded(io, raw, [&]() -> mcp::Task<void> {
        co_await client.connect("fields", "1.0");
        json params = {{"name", "slow"}, {"arguments", json::object()}};
        result = co_await client.send_request("tools/call", params);
        client.close();
    });

    EXPECT_EQ(result["content"][0]["text"], "done");
    EXPECT_EQ(transport->last_event_id(), "7");
}

TEST(HttpClientModern, CancelClosesTheResponseStream) {
    asio::io_context io;
    RawServer raw(io, [](const Recorded& request) {
        if (request.rpc_method() != "tools/call") {
            return modern_reply(request, json::array());
        }
        Reply reply;
        reply.content_type = "text/event-stream";
        reply.stream.push_back({": keep-alive\n\n", nullptr});
        reply.stream.push_back({"", [] { return false; }});
        return reply;
    });
    auto transport = make_transport(io, raw.url());
    mcp::Client client(transport, io.get_executor(), modern_options());

    int code = -1;
    bool finished = false;
    run_guarded(io, raw, [&]() -> mcp::Task<void> {
        co_await client.connect("cancel", "1.0");
        mcp_test::spawn_detached(io, [&]() -> mcp::Task<void> {
            code = co_await failure_code([&]() -> mcp::Task<void> {
                json arguments = json::object();
                co_await client.call_tool("slow", arguments);
            });
            finished = true;
        });
        co_await wait_until([&raw] { return raw.streams_started == 1; });
        const auto id = raw.requests.back().parsed().at("id").get<std::string>();
        co_await client.cancel(mcp::RequestId{id});
        co_await wait_until([&] { return finished && raw.streams_abandoned == 1; });
        client.close();
    });

    EXPECT_TRUE(finished);
    EXPECT_EQ(code, mcp::constants::jsonrpc::g_REQUEST_CANCELLED);
    EXPECT_EQ(raw.streams_abandoned, 1);
    // Closing the stream is the cancellation: no notification went over the wire.
    const auto methods = raw.rpc_methods();
    EXPECT_EQ(std::count(methods.begin(), methods.end(), "notifications/cancelled"), 0);
}

// A request the client stops waiting for is cancelled the same way: its response stream closes.
TEST(HttpClientModern, TimeoutClosesTheResponseStream) {
    asio::io_context io;
    RawServer raw(io, [](const Recorded& request) {
        if (request.rpc_method() != "tools/call") {
            return modern_reply(request, json::array());
        }
        Reply reply;
        reply.content_type = "text/event-stream";
        reply.stream.push_back({": keep-alive\n\n", nullptr});
        reply.stream.push_back({"", [] { return false; }});
        return reply;
    });
    auto transport = make_transport(io, raw.url());
    auto options = modern_options();
    options.request_timeout = std::chrono::milliseconds(300);
    mcp::Client client(transport, io.get_executor(), options);

    int code = -1;
    run_guarded(io, raw, [&]() -> mcp::Task<void> {
        co_await client.connect("timeout", "1.0");
        code = co_await failure_code([&]() -> mcp::Task<void> {
            json arguments = json::object();
            co_await client.call_tool("slow", arguments);
        });
        co_await wait_until([&raw] { return raw.streams_abandoned == 1; });
        client.close();
    });

    EXPECT_EQ(code, mcp::constants::jsonrpc::g_REQUEST_TIMEOUT);
    EXPECT_EQ(raw.streams_started, 1);
    EXPECT_EQ(raw.streams_abandoned, 1);
    const auto methods = raw.rpc_methods();
    EXPECT_EQ(std::count(methods.begin(), methods.end(), "notifications/cancelled"), 0);
}

TEST(HttpClientModern, CloseEndsTheOpenResponseStream) {
    asio::io_context io;
    RawServer raw(io, [](const Recorded& request) {
        if (request.rpc_method() != "tools/call") {
            return modern_reply(request, json::array());
        }
        Reply reply;
        reply.content_type = "text/event-stream";
        reply.stream.push_back({": keep-alive\n\n", nullptr});
        reply.stream.push_back({"", [] { return false; }});
        return reply;
    });
    auto transport = make_transport(io, raw.url());
    mcp::Client client(transport, io.get_executor(), modern_options());

    int code = -1;
    bool finished = false;
    run_guarded(io, raw, [&]() -> mcp::Task<void> {
        co_await client.connect("close", "1.0");
        mcp_test::spawn_detached(io, [&]() -> mcp::Task<void> {
            code = co_await failure_code([&]() -> mcp::Task<void> {
                json arguments = json::object();
                co_await client.call_tool("slow", arguments);
            });
            finished = true;
        });
        co_await wait_until([&raw] { return raw.streams_started == 1; });
        client.close();
        co_await wait_until([&] { return finished && raw.streams_abandoned == 1; });
    });

    EXPECT_TRUE(finished);
    EXPECT_NE(code, 0);
    EXPECT_EQ(raw.streams_abandoned, 1);
}

namespace {

/// Answers tools/call for `slow` only when @p release holds, and everything else at once.
RawServer::Handler holds_slow_tool(const bool& release) {
    return [&release](const Recorded& request) {
        if (request.rpc_method() != "tools/call" ||
            request.parsed()["params"].value("name", "") != "slow") {
            return modern_reply(request, json::array());
        }
        Reply reply = json_reply(
            rpc_result(request.parsed(),
                       {{"resultType", "complete"}, {"content", json::array()}, {"isError", false}}));
        reply.hold_until = [&release] { return release; };
        return reply;
    };
}

mcp::Task<void> call_named(mcp::Client& client, std::string name) {
    json arguments = json::object();
    co_await client.call_tool(name, arguments);
}

}  // namespace

// The server sends nothing for a handler that stays quiet, so the slow call's write is still going.
TEST(HttpClientModern, QuietHandlerDoesNotHoldBackTheNextCall) {
    asio::io_context io;
    bool release = false;
    RawServer raw(io, holds_slow_tool(release));
    auto transport = make_transport(io, raw.url());
    mcp::Client client(transport, io.get_executor(), modern_options());

    bool slow_done = false;
    bool slow_done_when_fast_returned = true;
    run_guarded(io, raw, [&]() -> mcp::Task<void> {
        co_await client.connect("quiet", "1.0");
        mcp_test::spawn_detached(io, [&]() -> mcp::Task<void> {
            co_await call_named(client, "slow");
            slow_done = true;
        });
        co_await wait_until([&raw] { return raw.rpc_methods().back() == "tools/call"; });
        co_await call_named(client, "fast");
        slow_done_when_fast_returned = slow_done;
        release = true;
        co_await wait_until([&] { return slow_done; });
        client.close();
    });

    EXPECT_FALSE(slow_done_when_fast_returned);
    EXPECT_TRUE(slow_done);
}

TEST(HttpClientModern, CancelReachesAQuietHandler) {
    asio::io_context io;
    bool release = false;
    RawServer raw(io, holds_slow_tool(release));
    auto transport = make_transport(io, raw.url());
    mcp::Client client(transport, io.get_executor(), modern_options());

    int code = -1;
    bool finished = false;
    bool handler_running_at_cancel_return = false;
    run_guarded(io, raw, [&]() -> mcp::Task<void> {
        co_await client.connect("cancel", "1.0");
        mcp_test::spawn_detached(io, [&]() -> mcp::Task<void> {
            code = co_await failure_code([&] { return call_named(client, "slow"); });
            finished = true;
        });
        co_await wait_until([&raw] { return raw.rpc_methods().back() == "tools/call"; });
        const auto id = raw.requests.back().parsed().at("id").get<std::string>();
        co_await client.cancel(mcp::RequestId{id});
        handler_running_at_cancel_return = raw.answers_abandoned == 0;
        co_await wait_until([&] { return finished && raw.answers_abandoned == 1; });
        client.close();
    });

    EXPECT_TRUE(finished);
    EXPECT_EQ(code, mcp::constants::jsonrpc::g_REQUEST_CANCELLED);
    EXPECT_EQ(raw.answers_abandoned, 1);
    EXPECT_TRUE(handler_running_at_cancel_return);
}

TEST(HttpClientModern, TimeoutReachesAQuietHandler) {
    asio::io_context io;
    bool release = false;
    RawServer raw(io, holds_slow_tool(release));
    auto transport = make_transport(io, raw.url());
    auto options = modern_options();
    options.request_timeout = std::chrono::milliseconds(300);
    mcp::Client client(transport, io.get_executor(), options);

    int code = -1;
    run_guarded(io, raw, [&]() -> mcp::Task<void> {
        co_await client.connect("timeout", "1.0");
        code = co_await failure_code([&] { return call_named(client, "slow"); });
        co_await wait_until([&raw] { return raw.answers_abandoned == 1; });
        client.close();
    });

    EXPECT_EQ(code, mcp::constants::jsonrpc::g_REQUEST_TIMEOUT);
    EXPECT_EQ(raw.answers_abandoned, 1);
}

TEST(HttpClientModern, SilentDiscoverFallsBackAtTheProbeTimeout) {
    asio::io_context io;
    RawServer raw(io, [](const Recorded& request) {
        if (request.rpc_method() == "server/discover") {
            Reply reply = status_reply(404);
            reply.hold_until = [] { return false; };
            return reply;
        }
        if (request.rpc_method() == "initialize") {
            return json_reply(rpc_result(request.parsed(), legacy_initialize_result()));
        }
        return Reply{202, "application/json", "", {}, {}, nullptr};
    });
    auto transport = make_transport(io, raw.url());
    auto options = auto_options();
    options.probe_timeout = std::chrono::milliseconds(200);
    options.request_timeout = g_hang_guard;
    mcp::Client client(transport, io.get_executor(), options);

    run_guarded(io, raw, [&]() -> mcp::Task<void> {
        co_await client.connect("silent", "1.0");
        // Nothing of the abandoned probe stays open.
        co_await wait_until([&raw] { return raw.answers_abandoned == 1; });
        client.close();
    });

    EXPECT_EQ(raw.answers_abandoned, 1);
    const auto methods = raw.rpc_methods();
    EXPECT_NE(std::find(methods.begin(), methods.end(), "initialize"), methods.end());
    // Giving the probe up closes its connection; it adds nothing to the wire.
    EXPECT_EQ(std::count(methods.begin(), methods.end(), "notifications/cancelled"), 0);
}

TEST(HttpClientModern, ListenWithoutAnAnswerTimesOut) {
    asio::io_context io;
    RawServer raw(io, [](const Recorded& request) {
        if (request.rpc_method() != "subscriptions/listen") {
            return modern_reply(request, json::array());
        }
        Reply reply = status_reply(500);
        reply.hold_until = [] { return false; };
        return reply;
    });
    auto transport = make_transport(io, raw.url());
    auto options = modern_options();
    options.request_timeout = std::chrono::milliseconds(300);
    mcp::Client client(transport, io.get_executor(), options);

    int code = -1;
    run_guarded(io, raw, [&]() -> mcp::Task<void> {
        co_await client.connect("listen", "1.0");
        code = co_await failure_code(
            [&]() -> mcp::Task<void> { (void)co_await client.listen(mcp::SubscriptionFilter{}); });
        co_await wait_until([&raw] { return raw.answers_abandoned == 1; });
        client.close();
    });

    EXPECT_EQ(code, mcp::constants::jsonrpc::g_REQUEST_TIMEOUT);
    EXPECT_EQ(raw.answers_abandoned, 1);
}

// A stream carries one request's response: another id does not end the exchange.
TEST(HttpClientModern, ResponseWithAnotherIdDoesNotEndTheStream) {
    asio::io_context io;
    RawServer raw(io, [](const Recorded& request) {
        if (request.rpc_method() != "tools/call") {
            return modern_reply(request, json::array());
        }
        const auto message = request.parsed();
        Reply reply;
        reply.content_type = "text/event-stream";
        reply.stream.push_back(
            {sse_event(rpc_result({{"id", "other"}}, {{"resultType", "complete"}})), nullptr});
        reply.stream.push_back(
            {sse_event(rpc_result(message,
                                  {{"resultType", "complete"},
                                   {"content", json::array({{{"type", "text"}, {"text", "real"}}})}})),
             nullptr});
        return reply;
    });
    auto transport = make_transport(io, raw.url());
    mcp::Client client(transport, io.get_executor(), modern_options());

    json result;
    run_guarded(io, raw, [&]() -> mcp::Task<void> {
        co_await client.connect("ids", "1.0");
        json arguments = json::object();
        const auto answer = co_await client.call_tool("any", arguments);
        result = answer;
        client.close();
    });

    EXPECT_EQ(result["content"][0]["text"], "real") << result;
}

// A response stream that breaks before the answer ends the call with g_CONNECTION_CLOSED. The message
// alone describes the connection's failure, without the source location Boost adds to it.
TEST(HttpClientModern, ABrokenResponseStreamEndsTheCallWithItsReason) {
    asio::io_context io;
    RawServer raw(io, [](const Recorded& request) {
        if (request.rpc_method() != "tools/call") {
            return modern_reply(request, json::array());
        }
        Reply reply;
        reply.content_type = "text/event-stream";
        const json progress = {{"jsonrpc", "2.0"},
                               {"method", "notifications/progress"},
                               {"params", {{"progressToken", "t1"}, {"progress", 1}, {"total", 2}}}};
        reply.stream.push_back({sse_event(progress), nullptr});
        reply.drop_after_stream = true;
        return reply;
    });
    auto transport = make_transport(io, raw.url());
    mcp::Client client(transport, io.get_executor(), modern_options());

    int code = 0;
    std::string message;
    bool nested = false;
    run_guarded(io, raw, [&]() -> mcp::Task<void> {
        co_await client.connect("drop", "1.0");
        try {
            json params = {
                {"name", "slow"}, {"arguments", json::object()}, {"_meta", {{"progressToken", "t1"}}}};
            (void)co_await client.send_request("tools/call", params);
        } catch (const mcp::McpError& error) {
            code = error.code();
            message = error.what();
            try {
                std::rethrow_if_nested(error);
            } catch (...) {
                nested = true;
            }
        }
        client.close();
    });

    EXPECT_EQ(code, mcp::constants::jsonrpc::g_CONNECTION_CLOSED);
    EXPECT_FALSE(nested);
    EXPECT_NE(message.find("HttpClientTransport lost its connection: "), std::string::npos) << message;
    EXPECT_EQ(message.find('['), std::string::npos) << message;
}

TEST(HttpClientModern, ListenStreamThatBreaksEndsAsDropped) {
    asio::io_context io;
    RawServer raw(io, [](const Recorded& request) {
        if (request.rpc_method() != "subscriptions/listen") {
            return modern_reply(request, json::array());
        }
        const json acknowledged = {
            {"jsonrpc", "2.0"},
            {"method", "notifications/subscriptions/acknowledged"},
            {"params",
             {{"notifications", json::object()},
              {"_meta", {{"io.modelcontextprotocol/subscriptionId", request.parsed().at("id")}}}}}};
        Reply reply;
        reply.content_type = "text/event-stream";
        reply.stream.push_back({sse_event(acknowledged), nullptr});
        return reply;
    });
    auto transport = make_transport(io, raw.url());
    mcp::Client client(transport, io.get_executor(), modern_options());

    std::optional<mcp::SubscriptionEnd> end;
    std::optional<mcp::Error> error;
    run_guarded(io, raw, [&]() -> mcp::Task<void> {
        co_await client.connect("drop", "1.0");
        auto subscription = co_await client.listen(mcp::SubscriptionFilter{});
        while (co_await subscription.next()) {
        }
        end = subscription.end_reason();
        error = subscription.end_error();
        client.close();
    });

    EXPECT_EQ(end, mcp::SubscriptionEnd::eDropped);
    ASSERT_TRUE(error.has_value());
    EXPECT_EQ(error->code, mcp::constants::jsonrpc::g_CONNECTION_CLOSED);
}

TEST(HttpClientModern, ArgumentTooLongForItsHeaderIsRefused) {
    asio::io_context io;
    const json tools = json::array({route_tool()});
    RawServer raw(io, [&tools](const Recorded& request) { return modern_reply(request, tools); });
    auto transport = make_transport(io, raw.url());
    mcp::Client client(transport, io.get_executor(), modern_options());

    int code = -1;
    std::size_t calls_on_the_wire = 0;
    run_guarded(io, raw, [&]() -> mcp::Task<void> {
        co_await client.connect("long", "1.0");
        co_await client.list_tools();
        code = co_await failure_code([&]() -> mcp::Task<void> {
            json arguments = {{"region", std::string(70000, 'x')}};
            co_await client.call_tool("route", arguments);
        });
        const auto methods = raw.rpc_methods();
        calls_on_the_wire = std::count(methods.begin(), methods.end(), "tools/call");
        client.close();
    });

    EXPECT_EQ(code, mcp::constants::jsonrpc::g_INVALID_PARAMS);
    EXPECT_EQ(calls_on_the_wire, 0u);
}

namespace {

std::unique_ptr<mcp::Server> make_sdk_server() {
    mcp::ServerCapabilities capabilities;
    capabilities.tools = mcp::ServerCapabilities::ToolsCapability{};
    capabilities.logging = json::object();
    auto server = std::make_unique<mcp::Server>(mcp::Implementation{"sdk-server", "1.0"}, capabilities);
    server->add_tool<json, json>("whoami", "Reports the request's per-request fields",
                                 json{{"type", "object"}},
                                 [](mcp::Context& ctx, json /*args*/) -> mcp::Task<json> {
                                     co_return json{{"modern", ctx.request_meta() != nullptr}};
                                 });
    server->add_tool<json, json>("route", "Mirrors arguments into headers",
                                 json{{"type", "object"},
                                      {"properties",
                                       {{"region", {{"type", "string"}, {"x-mcp-header", "Region"}}},
                                        {"count", {{"type", "integer"}, {"x-mcp-header", "Count"}}}}}},
                                 [](json args) -> mcp::Task<json> { co_return args; });
    // Tool names are limited to header-safe characters, prompt names are not: a prompt carries the
    // name that needs Base64 in its Mcp-Name header. The registration declares the capability.
    server->add_text_prompt("héllo wörld", "A name outside the header-safe characters", {},
                            [](const auto& /*arguments*/) { return std::string("ok"); });
    server->add_tool<json, json>("progress", "Reports progress, then returns", json{{"type", "object"}},
                                 [](mcp::Context& ctx, json /*args*/) -> mcp::Task<json> {
                                     co_await ctx.report_progress(1, 2);
                                     co_return json{{"done", true}};
                                 });
    return server;
}

struct SdkOutcome {
    mcp::ConnectResult connected;
    json whoami;
    json routed;
    json encoded;
    int progress_events{0};
    std::vector<std::string> order;
};

/// The exchange every SDK-server test makes: connect, ask who answers, mirror a header, send a
/// name that needs Base64, and (optionally) take a progress notification.
mcp::Task<void> sdk_exchange(mcp::Client& client, SdkOutcome& outcome, bool with_progress) {
    outcome.connected = co_await client.connect("sdk-client", "1.0");
    co_await client.list_tools();
    json no_arguments = json::object();
    const auto whoami = co_await client.call_tool("whoami", no_arguments);
    outcome.whoami = *whoami.structuredContent;
    json route_arguments = {{"region", "Hello, 世界"}, {"count", 7}};
    const auto routed = co_await client.call_tool("route", route_arguments);
    outcome.routed = *routed.structuredContent;
    const auto encoded = co_await client.get_prompt("héllo wörld", {});
    outcome.encoded = json(encoded);
    if (with_progress) {
        json params = {
            {"name", "progress"}, {"arguments", json::object()}, {"_meta", {{"progressToken", "p1"}}}};
        co_await client.send_request("tools/call", params);
        outcome.order.push_back("result");
    }
}

}  // namespace

TEST(HttpClientModern, AgainstDualEraSessionManager) {
    constexpr unsigned short port = 20530;
    asio::io_context io;
    asio::steady_timer guard(io);
    bool fired = false;
    mcp::StreamableHttpSessionManager manager(io.get_executor(), "127.0.0.1", port,
                                              [](const mcp::Executor&) { return make_sdk_server(); });
    auto transport = make_transport(io, "http://127.0.0.1:" + std::to_string(port) + "/mcp");
    mcp::Client client(transport, io.get_executor(), auto_options());
    SdkOutcome outcome;
    client.on_progress([&outcome](const mcp::ProgressNotificationParams&) {
        ++outcome.progress_events;
        outcome.order.push_back("progress");
    });

    std::string error;
    std::size_t sessions = 0;
    guard.expires_after(g_hang_guard);
    guard.async_wait([&](const boost::system::error_code& code) {
        if (!code) {
            fired = true;
            io.stop();
        }
    });
    mcp_test::spawn_detached(io, manager.listen());
    mcp_test::spawn_detached(io, [&]() -> mcp::Task<void> {
        try {
            co_await sdk_exchange(client, outcome, true);
        } catch (const std::exception& failure) {
            error = failure.what();
        }
        sessions = manager.session_count();
        client.close();
        manager.close();
        guard.cancel();
    });
    io.run();

    EXPECT_FALSE(fired);
    EXPECT_EQ(error, "");
    EXPECT_EQ(outcome.connected.era, mcp::ProtocolEra::eModern);
    EXPECT_EQ(outcome.whoami.at("modern"), true);
    EXPECT_EQ(outcome.routed.at("region"), "Hello, 世界");
    EXPECT_EQ(outcome.encoded.at("messages").at(0).at("content").at("text"), "ok");
    EXPECT_EQ(sessions, 0u);
    EXPECT_EQ(transport->session_id(), "");
    // The progress notification came over the call's own stream, ahead of its result.
    EXPECT_EQ(outcome.order, (std::vector<std::string>{"progress", "result"}));
}

TEST(HttpClientModern, AgainstRunHttpJsonOnly) {
    asio::io_context io;
    asio::steady_timer guard(io);
    bool fired = false;
    auto server_transport =
        std::make_shared<mcp::HttpServerTransport>(io.get_executor(), "127.0.0.1", 0);
    server_transport->set_json_only(true);
    auto server = make_sdk_server();
    auto transport =
        make_transport(io, "http://127.0.0.1:" + std::to_string(server_transport->port()) + "/mcp");
    mcp::Client client(transport, io.get_executor(), modern_options());
    SdkOutcome outcome;

    std::string error;
    guard.expires_after(g_hang_guard);
    guard.async_wait([&](const boost::system::error_code& code) {
        if (!code) {
            fired = true;
            io.stop();
        }
    });
    mcp_test::spawn_detached(io, server_transport->listen());
    mcp_test::spawn_detached(io, [&]() -> mcp::Task<void> {
        try {
            co_await server->run(server_transport, io.get_executor());
        } catch (...) {
        }
    });
    mcp_test::spawn_detached(io, [&]() -> mcp::Task<void> {
        try {
            co_await sdk_exchange(client, outcome, false);
        } catch (const std::exception& failure) {
            error = failure.what();
        }
        client.close();
        server_transport->close();
        guard.cancel();
    });
    io.run();

    EXPECT_FALSE(fired);
    EXPECT_EQ(error, "");
    EXPECT_EQ(outcome.connected.era, mcp::ProtocolEra::eModern);
    EXPECT_EQ(outcome.whoami.at("modern"), true);
    EXPECT_EQ(outcome.routed.at("count"), 7);
    EXPECT_EQ(outcome.encoded.at("messages").at(0).at("content").at("text"), "ok");
    EXPECT_FALSE(server->is_initialized());
}

namespace {

/// Forwards each connection to @p upstream and keeps the bytes the client sent.
class RecordingRelay {
   public:
    RecordingRelay(asio::io_context& io, unsigned short upstream)
        : io_(io),
          upstream_(upstream),
          acceptor_(io, asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0)) {}

    [[nodiscard]] std::string url() const {
        return "http://127.0.0.1:" + std::to_string(acceptor_.local_endpoint().port()) + "/mcp";
    }

    void start() { mcp_test::spawn_detached(io_, accept_loop(this)); }

    void close() {
        closing_ = true;
        beast::error_code ignored;
        (void)acceptor_.close(ignored);
        for (auto& weak : sockets_) {
            if (auto socket = weak.lock()) {
                (void)socket->close(ignored);
            }
        }
    }

    /// The requests the client sent, parsed out of the captured bytes.
    [[nodiscard]] std::vector<Recorded> requests() const {
        std::vector<Recorded> found;
        std::size_t offset = 0;
        for (;;) {
            const auto end = captured_.find("\r\n\r\n", offset);
            if (end == std::string::npos) {
                break;
            }
            Recorded recorded;
            std::size_t line_start = captured_.find("\r\n", offset);
            std::size_t length = 0;
            while (line_start < end) {
                line_start += 2;
                const auto line_end = captured_.find("\r\n", line_start);
                const auto line = captured_.substr(line_start, line_end - line_start);
                const auto colon = line.find(':');
                if (colon != std::string::npos) {
                    auto value = line.substr(colon + 1);
                    value.erase(0, value.find_first_not_of(' '));
                    recorded.headers.emplace_back(line.substr(0, colon), value);
                    if (lower(line.substr(0, colon)) == "content-length") {
                        length = std::stoul(value);
                    }
                }
                line_start = line_end;
            }
            recorded.method = captured_.substr(offset, captured_.find(' ', offset) - offset);
            recorded.body = captured_.substr(end + 4, length);
            found.push_back(std::move(recorded));
            offset = end + 4 + length;
        }
        return found;
    }

   private:
    static mcp::Task<void> pipe(std::shared_ptr<asio::ip::tcp::socket> from,
                                std::shared_ptr<asio::ip::tcp::socket> to, std::string* capture) {
        std::array<char, 4096> bytes{};
        try {
            for (;;) {
                const auto count =
                    co_await from->async_read_some(asio::buffer(bytes), asio::use_awaitable);
                if (capture != nullptr) {
                    capture->append(bytes.data(), count);
                }
                co_await asio::async_write(*to, asio::buffer(bytes.data(), count), asio::use_awaitable);
            }
        } catch (const std::exception&) {
            beast::error_code ignored;
            (void)to->shutdown(asio::ip::tcp::socket::shutdown_send, ignored);
        }
    }

    static mcp::Task<void> accept_loop(RecordingRelay* self) {
        while (!self->closing_) {
            auto client = std::make_shared<asio::ip::tcp::socket>(self->io_);
            try {
                co_await self->acceptor_.async_accept(*client, asio::use_awaitable);
            } catch (const std::exception&) {
                co_return;
            }
            auto server = std::make_shared<asio::ip::tcp::socket>(self->io_);
            try {
                co_await server->async_connect(
                    asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), self->upstream_),
                    asio::use_awaitable);
            } catch (const std::exception&) {
                continue;
            }
            self->sockets_.push_back(client);
            self->sockets_.push_back(server);
            mcp_test::spawn_detached(self->io_, pipe(client, server, &self->captured_));
            mcp_test::spawn_detached(self->io_, pipe(server, client, nullptr));
        }
    }

    asio::io_context& io_;
    unsigned short upstream_;
    asio::ip::tcp::acceptor acceptor_;
    bool closing_{false};
    std::string captured_;
    std::vector<std::weak_ptr<asio::ip::tcp::socket>> sockets_;
};

enum class ServerKind {
    eSessionManager,
    eServerTransport
};

struct PinnedOutcome {
    mcp::ConnectResult connected;
    std::vector<Recorded> requests;
    std::string error;
};

/// Connects a client pinned to @p pinned to one of the SDK's servers through a relay, lists the
/// tools, and returns what the client sent.
PinnedOutcome run_pinned(ServerKind kind, unsigned short manager_port, const std::string& pinned,
                         const std::optional<mcp::ServerProtocolPolicy>& policy) {
    asio::io_context io;
    asio::steady_timer guard(io);
    bool fired = false;
    PinnedOutcome outcome;

    std::unique_ptr<mcp::StreamableHttpSessionManager> manager;
    std::shared_ptr<mcp::HttpServerTransport> server_transport;
    std::unique_ptr<mcp::Server> server;
    unsigned short upstream = manager_port;
    if (kind == ServerKind::eSessionManager) {
        manager = std::make_unique<mcp::StreamableHttpSessionManager>(
            io.get_executor(), "127.0.0.1", manager_port,
            [](const mcp::Executor&) { return make_sdk_server(); });
        if (policy) {
            manager->set_protocol_policy(*policy);
        }
        mcp_test::spawn_detached(io, manager->listen());
    } else {
        server_transport =
            std::make_shared<mcp::HttpServerTransport>(io.get_executor(), "127.0.0.1", 0);
        server = make_sdk_server();
        if (policy) {
            server->set_protocol_policy(*policy);
        }
        upstream = server_transport->port();
        mcp_test::spawn_detached(io, server_transport->listen());
        mcp_test::spawn_detached(io, [&]() -> mcp::Task<void> {
            try {
                co_await server->run(server_transport, io.get_executor());
            } catch (...) {
            }
        });
    }

    RecordingRelay relay(io, upstream);
    relay.start();
    auto transport = make_transport(io, relay.url());
    mcp::ClientOptions options;
    options.protocol = mcp::ClientProtocol::pinned(pinned);
    mcp::Client client(transport, io.get_executor(), options);

    guard.expires_after(g_hang_guard);
    guard.async_wait([&](const boost::system::error_code& code) {
        if (!code) {
            fired = true;
            io.stop();
        }
    });
    mcp_test::spawn_detached(io, [&]() -> mcp::Task<void> {
        try {
            outcome.connected = co_await client.connect("pinned", "1.0");
            co_await client.list_tools();
            json arguments = json::object();
            co_await client.call_tool("whoami", arguments);
        } catch (const std::exception& failure) {
            outcome.error = failure.what();
        }
        outcome.requests = relay.requests();
        client.close();
        relay.close();
        if (manager) {
            manager->close();
        }
        if (server_transport) {
            server_transport->close();
        }
        guard.cancel();
    });
    io.run();
    EXPECT_FALSE(fired) << "the test did not finish";
    return outcome;
}

}  // namespace

TEST(HttpClientPinned, HeaderAndInitializeCarryPinnedVersion) {
    unsigned short port = 20531;
    for (const auto kind : {ServerKind::eSessionManager, ServerKind::eServerTransport}) {
        for (const std::string version : {"2024-11-05", "2025-03-26", "2025-06-18", "2025-11-25"}) {
            SCOPED_TRACE(version + (kind == ServerKind::eSessionManager ? " session manager"
                                                                        : " server transport"));
            const auto outcome = run_pinned(kind, port++, version, std::nullopt);

            EXPECT_EQ(outcome.error, "");
            EXPECT_EQ(outcome.connected.era, mcp::ProtocolEra::eLegacy);
            EXPECT_EQ(outcome.connected.protocolVersion, version);
            ASSERT_GE(outcome.requests.size(), 4u);
            EXPECT_EQ(outcome.requests[0].rpc_method(), "initialize");
            EXPECT_EQ(outcome.requests[0].parsed()["params"]["protocolVersion"], version);
            for (const auto& request : outcome.requests) {
                EXPECT_EQ(request.header("MCP-Protocol-Version"), version) << request.body;
                EXPECT_FALSE(request.header("Mcp-Method").has_value()) << request.body;
            }
        }
    }
}

TEST(HttpClientPinned, ServerRestrictedPolicyNegotiatesUpAndHeadersFollow) {
    mcp::ServerProtocolPolicy policy;
    policy.legacy_versions = std::vector<std::string>{"2025-11-25"};
    const auto outcome = run_pinned(ServerKind::eSessionManager, 20539, "2025-03-26", policy);

    EXPECT_EQ(outcome.error, "");
    EXPECT_EQ(outcome.connected.protocolVersion, "2025-11-25");
    ASSERT_GE(outcome.requests.size(), 4u);
    EXPECT_EQ(outcome.requests[0].rpc_method(), "initialize");
    EXPECT_EQ(outcome.requests[0].header("MCP-Protocol-Version"), "2025-03-26");
    for (std::size_t index = 1; index < outcome.requests.size(); ++index) {
        EXPECT_EQ(outcome.requests[index].header("MCP-Protocol-Version"), "2025-11-25")
            << outcome.requests[index].body;
    }
}

namespace {

/// A legacy client connecting to a raw server that answers initialize with @p status and @p body.
std::string legacy_connect_error(unsigned int status, std::function<json(const json&)> body) {
    asio::io_context io;
    RawServer raw(io,
                  [&](const Recorded& request) { return json_reply(body(request.parsed()), status); });
    mcp::ClientOptions options;
    options.protocol = mcp::ClientProtocol::legacy();
    mcp::Client client(make_transport(io, raw.url()), io.get_executor(), options);
    std::string error;
    run_guarded(io, raw, [&]() -> mcp::Task<void> {
        try {
            co_await client.connect("legacy", "1.0");
        } catch (const std::exception& failure) {
            error = failure.what();
        }
        client.close();
    });
    return error;
}

}  // namespace

// A server may refuse initialize with an HTTP error status and a JSON-RPC error body; the caller
// gets that error, not just the status.
TEST(HttpClientLegacy, InitializeRefusedWithAnErrorStatusReportsTheJsonRpcError) {
    for (const unsigned int status : {400u, 404u}) {
        SCOPED_TRACE(status);
        const auto error = legacy_connect_error(status, [](const json& request) {
            return rpc_error(request, -32601,
                             "Method not found: initialize; this server serves protocol versions "
                             "2026-07-28");
        });
        EXPECT_NE(error.find("-32601"), std::string::npos) << error;
        EXPECT_NE(error.find("2026-07-28"), std::string::npos) << error;
    }
}

// The body of such an error may leave the id out or null, and the jsonrpc member out; it still
// answers the initialize.
TEST(HttpClientLegacy, InitializeErrorWithoutAnIdStillReachesTheCaller) {
    const json error_object = {{"code", -32600}, {"message", "Bad Request: unsupported version"}};
    for (const auto& body : {json{{"jsonrpc", "2.0"}, {"id", nullptr}, {"error", error_object}},
                             json{{"error", error_object}}}) {
        SCOPED_TRACE(body.dump());
        const auto error = legacy_connect_error(400, [&body](const json&) { return body; });
        EXPECT_NE(error.find("unsupported version"), std::string::npos) << error;
    }
}

// A body that is not a JSON-RPC error, or whose error has no integer code, leaves the plain HTTP
// status error.
TEST(HttpClientLegacy, InitializeRefusedWithoutAJsonRpcErrorReportsTheStatus) {
    for (const auto& body : {json{{"detail", "no"}}, json::array({1, 2}), json{{"error", "refused"}},
                             json{{"error", {{"code", "-32600"}, {"message", "refused"}}}},
                             json{{"error", {{"code", -32600.5}, {"message", "refused"}}}}}) {
        SCOPED_TRACE(body.dump());
        const auto error = legacy_connect_error(400, [&body](const json&) { return body; });
        EXPECT_NE(error.find("status 400"), std::string::npos) << error;
    }
}

/// The HTTP status of the HttpStatusError nested in @p error, the McpError a client call threw.
std::optional<unsigned int> nested_http_status(const mcp::McpError& error) {
    try {
        std::rethrow_if_nested(error);
    } catch (const mcp::HttpStatusError& failure) {
        return failure.status();
    } catch (...) {
        // Another nested type has no status.
    }
    return std::nullopt;
}

// Inside a session an error status keeps its HTTP meaning, a 404 there saying the session is gone:
// the call throws McpError with the status in its data and the HttpStatusError nested in it.
TEST(HttpClientLegacy, ErrorStatusInsideASessionKeepsItsHttpStatus) {
    asio::io_context io;
    RawServer raw(io, [](const Recorded& request) {
        const auto message = request.parsed();
        const auto method = message.value("method", "");
        if (method == "initialize") {
            auto reply = json_reply(rpc_result(message, legacy_initialize_result()));
            reply.headers.emplace_back("Mcp-Session-Id", "session-1");
            return reply;
        }
        if (method == "tools/list") {
            return json_reply(rpc_error(message, -32600, "Session not found"), 404);
        }
        Reply accepted;
        accepted.status = 202;
        return accepted;
    });
    mcp::ClientOptions options;
    options.protocol = mcp::ClientProtocol::legacy();
    mcp::Client client(make_transport(io, raw.url()), io.get_executor(), options);
    std::optional<int> code;
    std::optional<unsigned int> status;
    unsigned int data_status = 0;
    std::string other;
    run_guarded(io, raw, [&]() -> mcp::Task<void> {
        try {
            co_await client.connect("legacy", "1.0");
            co_await client.list_tools();
        } catch (const mcp::McpError& failure) {
            code = failure.code();
            status = nested_http_status(failure);
            data_status = failure.data() ? failure.data()->value("httpStatus", 0u) : 0u;
        } catch (const std::exception& failure) {
            other = failure.what();
        }
        client.close();
    });
    EXPECT_EQ(code, mcp::constants::jsonrpc::g_CONNECTION_CLOSED) << other;
    EXPECT_EQ(status, 404u);
    EXPECT_EQ(data_status, 404u);
}

// A 2026-07-28 request answered 400, 404 or 405 with a JSON-RPC error fails with that error, even
// when the body leaves out the request's id or the "jsonrpc" member. A body whose error is not a
// JSON-RPC error object leaves the HTTP status as the failure.
TEST(HttpClientModern, AnErrorStatusCarriesTheServersJsonRpcError) {
    asio::io_context io;
    RawServer raw(io, [](const Recorded& request) {
        if (request.rpc_method() != "tools/call") {
            return modern_reply(request, json::array({route_tool()}));
        }
        const auto region = request.parsed()["params"]["arguments"].value("region", "");
        if (region == "bare") {
            return status_reply(404, R"({"error":{"code":-32602,"message":"Unknown region"}})");
        }
        if (region == "text") {
            return status_reply(400, R"({"error":"Unknown region"})");
        }
        return status_reply(400, R"({"jsonrpc":"2.0","id":null,"error":{"code":"bad","message":"x"}})");
    });
    mcp::Client client(make_transport(io, raw.url()), io.get_executor(), modern_options());

    std::vector<int> codes;
    std::vector<std::optional<unsigned int>> statuses;
    std::string message;
    run_guarded(io, raw, [&]() -> mcp::Task<void> {
        co_await client.connect("errors", "1.0");
        const std::array<std::string_view, 3> regions{"bare", "text", "code"};
        for (const auto region : regions) {
            try {
                const json arguments = {{"region", std::string(region)}};
                co_await client.call_tool("route", arguments);
                codes.push_back(0);
            } catch (const mcp::McpError& failure) {
                codes.push_back(failure.code());
                statuses.push_back(nested_http_status(failure));
                if (region == "bare") {
                    message = failure.what();
                }
            }
        }
        client.close();
    });

    ASSERT_EQ(codes.size(), 3U);
    EXPECT_EQ(codes[0], mcp::constants::jsonrpc::g_INVALID_PARAMS);
    EXPECT_NE(message.find("Unknown region"), std::string::npos) << message;
    ASSERT_EQ(statuses.size(), 3U);
    EXPECT_EQ(statuses[0], std::nullopt);
    EXPECT_EQ(statuses[1], 400U);
    EXPECT_EQ(statuses[2], 400U);
}

// A new session started in place of a forgotten one that answers with another version and other
// capabilities fails the request, and its version reaches no request.
TEST(HttpClientLegacy, NewSessionWithOtherTermsFailsTheRequest) {
    asio::io_context io;
    int initializes = 0;
    int pings = 0;
    RawServer raw(io, [&initializes, &pings](const Recorded& request) {
        const auto message = request.parsed();
        const auto method = request.rpc_method();
        if (method == "initialize") {
            ++initializes;
            auto result = legacy_initialize_result();
            if (initializes > 1) {
                result["protocolVersion"] = "2025-03-26";
                result["capabilities"] = json::object();
            }
            auto reply = json_reply(rpc_result(message, result));
            reply.headers.emplace_back("Mcp-Session-Id", "session-" + std::to_string(initializes));
            return reply;
        }
        if (method == "ping") {
            ++pings;
            if (pings == 2) {
                return json_reply(rpc_error(message, -32600, "Session not found"), 404);
            }
            return json_reply(rpc_result(message, json::object()));
        }
        Reply accepted;
        accepted.status = 202;
        return accepted;
    });
    mcp::ClientOptions options;
    options.protocol = mcp::ClientProtocol::legacy();
    mcp::Client client(make_transport(io, raw.url()), io.get_executor(), options);
    bool first_answered = false;
    std::string failure;
    run_guarded(io, raw, [&]() -> mcp::Task<void> {
        co_await client.connect("legacy", "1.0");
        co_await client.ping();
        first_answered = true;
        try {
            co_await client.ping();
        } catch (const std::exception& error) {
            failure = error.what();
        }
        client.close();
    });
    EXPECT_TRUE(first_answered);
    EXPECT_EQ(initializes, 2);
    EXPECT_NE(failure.find("another protocol version"), std::string::npos) << failure;
    for (const auto& request : raw.requests) {
        EXPECT_NE(request.header("MCP-Protocol-Version"), std::optional<std::string>("2025-03-26"))
            << request.body;
    }
}

/// A legacy server whose session is forgotten when the second ping arrives: that ping is answered
/// 404. Every initialize starts session-<n>.
RawServer forgetting_server(asio::io_context& io, int& initializes) {
    return RawServer(io, [&initializes, pings = 0](const Recorded& request) mutable {
        const auto message = request.parsed();
        const auto method = request.rpc_method();
        if (method == "initialize") {
            ++initializes;
            auto reply = json_reply(rpc_result(message, legacy_initialize_result()));
            reply.headers.emplace_back("Mcp-Session-Id", "session-" + std::to_string(initializes));
            return reply;
        }
        if (method == "ping") {
            ++pings;
            if (pings == 2) {
                return json_reply(rpc_error(message, -32600, "Session not found"), 404);
            }
            return json_reply(rpc_result(message, json::object()));
        }
        Reply accepted;
        accepted.status = 202;
        return accepted;
    });
}

// A new session is started as the first was: initialize, then notifications/initialized, and only
// then the request the old session lost.
TEST(HttpClientLegacy, NewSessionIsInitializedBeforeTheRequestIsSentAgain) {
    asio::io_context io;
    int initializes = 0;
    RawServer raw = forgetting_server(io, initializes);
    mcp::ClientOptions options;
    options.protocol = mcp::ClientProtocol::legacy();
    mcp::Client client(make_transport(io, raw.url()), io.get_executor(), options);
    run_guarded(io, raw, [&]() -> mcp::Task<void> {
        co_await client.connect("legacy", "1.0");
        co_await client.ping();
        co_await client.ping();
        client.close();
    });
    EXPECT_EQ(initializes, 2);
    std::vector<std::string> in_new_session;
    for (const auto& request : raw.requests) {
        if (request.header("Mcp-Session-Id") == std::optional<std::string>("session-2") &&
            !request.rpc_method().empty()) {
            in_new_session.push_back(request.rpc_method());
        }
    }
    ASSERT_GE(in_new_session.size(), 2U);
    EXPECT_EQ(in_new_session[0], "notifications/initialized");
    EXPECT_EQ(in_new_session[1], "ping");
}

// With set_reinitialize_on_expired_session(false), a forgotten session fails the request with the
// 404 and no new session is started.
TEST(HttpClientLegacy, ForgottenSessionIsNotReplacedWhenTurnedOff) {
    asio::io_context io;
    int initializes = 0;
    RawServer raw = forgetting_server(io, initializes);
    auto transport = make_transport(io, raw.url());
    transport->set_reinitialize_on_expired_session(false);
    mcp::ClientOptions options;
    options.protocol = mcp::ClientProtocol::legacy();
    mcp::Client client(transport, io.get_executor(), options);
    std::optional<unsigned int> status;
    std::string other;
    run_guarded(io, raw, [&]() -> mcp::Task<void> {
        co_await client.connect("legacy", "1.0");
        co_await client.ping();
        try {
            co_await client.ping();
        } catch (const mcp::McpError& failure) {
            status = nested_http_status(failure);
        } catch (const std::exception& failure) {
            other = failure.what();
        }
        client.close();
    });
    EXPECT_EQ(status, 404u) << other;
    EXPECT_EQ(initializes, 1);
}

namespace {

/// Nesting far past the depth a peer may send, and past what recursive JSON code survives on a stack.
constexpr std::size_t g_hostile_depth = 400000;

std::string deep_array() {
    return std::string(g_hostile_depth, '[') + std::string(g_hostile_depth, ']');
}

std::string deep_properties() {
    std::string text;
    for (std::size_t level = 0; level < g_hostile_depth / 8; ++level) {
        text += R"({"properties":{"a":)";
    }
    text += "{}";
    for (std::size_t level = 0; level < g_hostile_depth / 8; ++level) {
        text += "}}";
    }
    return text;
}

/// The error of listing one tool whose input schema is @p input_schema, which is written out rather
/// than built as JSON because recursive JSON code cannot hold this much nesting.
int code_of_listing(const std::string& input_schema) {
    asio::io_context io;
    RawServer raw(io, [&input_schema](const Recorded& request) {
        if (request.rpc_method() != "tools/list") {
            return modern_reply(request, json::array());
        }
        Reply reply;
        reply.body = R"({"jsonrpc":"2.0","id":)" + request.parsed().at("id").dump() +
                     R"(,"result":{"resultType":"complete","tools":[{"name":"t","inputSchema":)" +
                     input_schema + "}]}}";
        return reply;
    });
    auto transport = make_transport(io, raw.url());
    mcp::ClientOptions options = modern_options();
    options.request_timeout = std::chrono::milliseconds(100);
    mcp::Client client(transport, io.get_executor(), options);

    int code = 0;
    run_guarded(io, raw, [&]() -> mcp::Task<void> {
        co_await client.connect("hostile", "1.0");
        code = co_await failure_code([&]() -> mcp::Task<void> { co_await client.list_tools(); });
        client.close();
    });
    return code;
}

}  // namespace

TEST(HttpClientModern, DeeplyNestedToolSchemaIsRefused) {
    EXPECT_EQ(code_of_listing(R"({"type":"object","default":)" + deep_array() + "}"),
              mcp::constants::jsonrpc::g_REQUEST_TIMEOUT);
}

TEST(HttpClientModern, DeeplyNestedToolPropertiesAreRefused) {
    EXPECT_EQ(code_of_listing(deep_properties()), mcp::constants::jsonrpc::g_REQUEST_TIMEOUT);
}

TEST(HttpClientModern, DeeplyNestedErrorBodyGoesLegacy) {
    expect_legacy_fallback(
        status_reply(400, R"({"jsonrpc":"2.0","id":1,"error":{"code":-32602,"message":"m","data":)" +
                              deep_array() + "}}"));
}

TEST(HttpClientModern, EventWithoutAnEndIsCutOffAtTheMessageLimit) {
    asio::io_context io;
    RawServer raw(io, [](const Recorded& request) {
        if (request.rpc_method() != "tools/call") {
            return modern_reply(request, json::array());
        }
        Reply reply;
        reply.content_type = "text/event-stream";
        // Short lines, each under the line limit, and never the blank line that ends an event.
        std::string lines;
        for (int line = 0; line < 1024; ++line) {
            lines += "data: " + std::string(1024, 'x') + "\n";
        }
        for (int mebibyte = 0; mebibyte < 17; ++mebibyte) {
            reply.stream.push_back({lines, nullptr});
        }
        return reply;
    });
    auto transport = make_transport(io, raw.url());
    auto options = modern_options();
    // The call ends at the message limit; its timeout only reports a stream that is never cut off.
    options.request_timeout = g_hang_guard;
    mcp::Client client(transport, io.get_executor(), options);

    std::string failure;
    run_guarded(io, raw, [&]() -> mcp::Task<void> {
        co_await client.connect("stream", "1.0");
        try {
            json arguments = json::object();
            co_await client.call_tool("slow", arguments);
        } catch (const std::exception& error) {
            failure = error.what();
        }
        client.close();
    });

    EXPECT_NE(failure.find("too long"), std::string::npos) << failure;
}

// One line without its end is bounded on its own, before it can become part of an event: the
// call ends at the message limit instead of buffering the line for as long as the server sends.
TEST(HttpClientModern, LineWithoutAnEndIsCutOffAtTheMessageLimit) {
    asio::io_context io;
    RawServer raw(io, [](const Recorded& request) {
        if (request.rpc_method() != "tools/call") {
            return modern_reply(request, json::array());
        }
        Reply reply;
        reply.content_type = "text/event-stream";
        reply.stream.push_back({"data: ", nullptr});
        const std::string mebibyte(std::size_t{1024} * 1024, 'x');
        for (int sent = 0; sent < 17; ++sent) {
            reply.stream.push_back({mebibyte, nullptr});
        }
        return reply;
    });
    auto transport = make_transport(io, raw.url());
    auto options = modern_options();
    // The call ends at the message limit; its timeout only reports a stream that is never cut off.
    options.request_timeout = g_hang_guard;
    mcp::Client client(transport, io.get_executor(), options);

    std::string failure;
    run_guarded(io, raw, [&]() -> mcp::Task<void> {
        co_await client.connect("stream", "1.0");
        try {
            json arguments = json::object();
            co_await client.call_tool("slow", arguments);
        } catch (const std::exception& error) {
            failure = error.what();
        }
        client.close();
    });

    EXPECT_NE(failure.find("An SSE line from the server is too long"), std::string::npos) << failure;
}

// The stream of a response is read as the SSE standard reads one: comment and unknown fields are
// skipped, CRLF, LF and CR each end a line, a line or a CRLF may arrive in pieces, and the data
// lines of one event are joined with a newline. A last event the stream ends without its blank line
// is dropped, so the call then fails for want of a response.
TEST(HttpClientModern, StreamLinesEndAsTheSseStandardHasIt) {
    for (const bool ended : {true, false}) {
        SCOPED_TRACE(ended ? "ended" : "unended");
        asio::io_context io;
        RawServer raw(io, [ended](const Recorded& request) {
            if (request.rpc_method() != "tools/call") {
                return modern_reply(request, json::array());
            }
            const auto message = request.parsed();
            const auto progress =
                json{{"jsonrpc", "2.0"},
                     {"method", "notifications/progress"},
                     {"params", {{"progressToken", "t1"}, {"progress", 1}, {"total", 2}}}}
                    .dump();
            const auto result =
                rpc_result(message, {{"resultType", "complete"},
                                     {"content", json::array({{{"type", "text"}, {"text", "done"}}})}})
                    .dump();
            const auto half = progress.size() / 2;
            const auto comma = result.find(',');
            Reply reply;
            reply.content_type = "text/event-stream";
            reply.stream.push_back({": keep-alive\r\n\r\n", nullptr});
            reply.stream.push_back(
                {"event: message\rretry: 1000\r\ndata: " + progress.substr(0, half), nullptr});
            reply.stream.push_back({progress.substr(half) + "\r", nullptr});
            reply.stream.push_back({"\n\r\n", nullptr});
            const auto split_result =
                "data: " + result.substr(0, comma + 1) + "\rdata: " + result.substr(comma + 1);
            reply.stream.push_back({ended ? split_result + "\r\r" : split_result, nullptr});
            return reply;
        });
        auto transport = make_transport(io, raw.url());
        mcp::Client client(transport, io.get_executor(), modern_options());
        int progress_seen = 0;
        client.on_progress([&progress_seen](const mcp::ProgressNotificationParams& params) {
            if (params.progress == 1 && params.total == 2) {
                ++progress_seen;
            }
        });

        json result;
        std::string failure;
        run_guarded(io, raw, [&]() -> mcp::Task<void> {
            co_await client.connect("stream", "1.0");
            json params = {
                {"name", "slow"}, {"arguments", json::object()}, {"_meta", {{"progressToken", "t1"}}}};
            try {
                result = co_await client.send_request("tools/call", params);
            } catch (const std::exception& error) {
                failure = error.what();
            }
            client.close();
        });

        EXPECT_EQ(progress_seen, 1);
        if (ended) {
            ASSERT_TRUE(result.is_object()) << failure;
            EXPECT_EQ(result["content"][0]["text"], "done");
        } else {
            EXPECT_TRUE(result.is_null()) << result;
            EXPECT_NE(failure.find("ended without a response"), std::string::npos) << failure;
        }
    }
}

// A legacy server may answer a POST with its whole event stream as the body. That body is read
// the same way: CRLF and lone CR line ends, and an event whose data spans several lines.
TEST(HttpClientLegacy, EventStreamBodyWithCrlfCrAndMultiLineDataIsRead) {
    asio::io_context io;
    RawServer raw(io, [](const Recorded& request) {
        const auto message = request.parsed();
        const auto method = message.is_object() ? message.value("method", "") : "";
        if (method == "initialize") {
            auto reply = json_reply(rpc_result(message, legacy_initialize_result()));
            reply.headers.emplace_back("Mcp-Session-Id", "session-1");
            return reply;
        }
        if (method != "tools/call") {
            Reply accepted;
            accepted.status = 202;
            return accepted;
        }
        const auto result =
            rpc_result(message, {{"content", json::array({{{"type", "text"}, {"text", "done"}}})}})
                .dump();
        const auto comma = result.find(',');
        Reply reply;
        reply.content_type = "text/event-stream";
        reply.body = ": comment\r\nevent: message\r\nid: 7\rdata: " + result.substr(0, comma + 1) +
                     "\r\ndata: " + result.substr(comma + 1) + "\r\n\r\n";
        return reply;
    });
    mcp::ClientOptions options;
    options.protocol = mcp::ClientProtocol::legacy();
    mcp::Client client(make_transport(io, raw.url()), io.get_executor(), options);

    std::string text;
    std::string failure;
    run_guarded(io, raw, [&]() -> mcp::Task<void> {
        try {
            co_await client.connect("legacy", "1.0");
            json arguments = json::object();
            const auto result = co_await client.call_tool("slow", arguments);
            if (!result.content.empty() &&
                std::holds_alternative<mcp::TextContent>(result.content[0])) {
                text = std::get<mcp::TextContent>(result.content[0]).text;
            }
        } catch (const std::exception& error) {
            failure = error.what();
        }
        client.close();
    });

    EXPECT_EQ(text, "done") << failure;
}

namespace {

/// A legacy server whose every GET stream carries `stream_text` and then ends. `gets` records when
/// each GET arrived.
struct EndingStreams {
    std::string stream_text;
    std::vector<std::chrono::steady_clock::time_point> gets;
};

/// Connects a legacy Client to an EndingStreams server and waits for @p wanted GETs.
void run_ending_streams(EndingStreams& streams, std::size_t wanted) {
    asio::io_context io;
    RawServer raw(io, [&streams](const Recorded& request) {
        Reply reply;
        reply.headers = {{"Mcp-Session-Id", "s1"}};
        if (request.method == "GET") {
            streams.gets.push_back(std::chrono::steady_clock::now());
            reply.content_type = "text/event-stream";
            reply.stream = {Step{streams.stream_text, {}}};
            return reply;
        }
        const auto message = request.parsed();
        if (message.value("method", "") == "initialize") {
            reply.body = rpc_result(message, legacy_initialize_result()).dump();
            return reply;
        }
        reply.status = 202;
        return reply;
    });
    auto transport = make_transport(io, raw.url());
    mcp::ClientOptions options;
    options.protocol = mcp::ClientProtocol::legacy();
    mcp::Client client(transport, io.get_executor(), options);
    run_guarded(io, raw, [&]() -> mcp::Task<void> {
        co_await client.connect("streams", "1.0");
        co_await wait_until([&streams, wanted] { return streams.gets.size() >= wanted; });
        client.close();
    });
}

}  // namespace

// A request's stream that breaks inside an event is resumed after the last event that arrived
// whole: the id of the broken one counts only once its blank line arrives.
TEST(HttpClientLegacyStream, AStreamBrokenInsideAnEventResumesAfterTheLastWholeEvent) {
    asio::io_context io;
    json call;
    std::optional<std::string> resumed_after;
    RawServer raw(io, [&call, &resumed_after](const Recorded& request) {
        Reply reply;
        reply.headers = {{"Mcp-Session-Id", "s1"}};
        if (request.method == "GET") {
            // The session's own GET stream names no event; the resume of the call's stream does.
            const auto last_event_id = request.header("Last-Event-ID");
            if (!last_event_id) {
                reply.status = 405;
                return reply;
            }
            resumed_after = last_event_id;
            reply.content_type = "text/event-stream";
            const auto result = rpc_result(call, {{"content", json::array()}}).dump();
            reply.stream = {Step{"id: 2\ndata: " + result + "\n\n", {}}};
            return reply;
        }
        const auto message = request.parsed();
        const auto method = message.value("method", "");
        if (method == "initialize") {
            reply.body = rpc_result(message, legacy_initialize_result()).dump();
            return reply;
        }
        if (method != "tools/call") {
            reply.status = 202;
            return reply;
        }
        call = message;
        const json note = {{"jsonrpc", "2.0"},
                           {"method", "notifications/message"},
                           {"params", {{"level", "info"}, {"data", "tick"}}}};
        reply.content_type = "text/event-stream";
        reply.stream = {Step{"id: 1\n" + sse_event(note) + "id: 2\ndata: {\"jsonrpc\"", {}}};
        return reply;
    });
    auto transport = make_transport(io, raw.url());
    mcp::ClientOptions options;
    options.protocol = mcp::ClientProtocol::legacy();
    mcp::Client client(transport, io.get_executor(), options);

    json result;
    std::string failure;
    run_guarded(io, raw, [&]() -> mcp::Task<void> {
        co_await client.connect("resume", "1.0");
        json params = {{"name", "slow"}, {"arguments", json::object()}};
        try {
            result = co_await client.send_request("tools/call", params);
        } catch (const std::exception& error) {
            failure = error.what();
        }
        client.close();
    });

    EXPECT_EQ(resumed_after, "1");
    EXPECT_TRUE(result.contains("content")) << failure;
}

// A legacy request whose event stream stays quiet past set_response_timeout() ends with
// g_CONNECTION_CLOSED, and the message says it timed out rather than that the transport is closed.
TEST(HttpClientLegacyStream, AStreamQuietPastTheResponseTimeoutEndsAsATimeout) {
    asio::io_context io;
    bool released = false;
    RawServer raw(io, [&released](const Recorded& request) {
        if (request.method == "GET") {
            return status_reply(405);
        }
        const auto message = request.parsed();
        const auto method = message.value("method", "");
        if (method == "initialize") {
            auto reply = json_reply(rpc_result(message, legacy_initialize_result()));
            reply.headers.emplace_back("Mcp-Session-Id", "session-1");
            return reply;
        }
        Reply reply;
        if (method != "ping") {
            reply.status = 202;
            return reply;
        }
        reply.content_type = "text/event-stream";
        reply.stream = {Step{": open\n\n", nullptr}, Step{"", [&released] { return released; }}};
        return reply;
    });
    auto transport = make_transport(io, raw.url());
    transport->set_response_timeout(std::chrono::milliseconds(100));
    mcp::ClientOptions options;
    options.protocol = mcp::ClientProtocol::legacy();
    mcp::Client client(transport, io.get_executor(), options);

    int code = 0;
    std::string message;
    run_guarded(io, raw, [&]() -> mcp::Task<void> {
        co_await client.connect("quiet", "1.0");
        try {
            co_await client.ping();
        } catch (const mcp::McpError& error) {
            code = error.code();
            message = error.what();
        }
        released = true;
        client.close();
    });

    EXPECT_EQ(code, mcp::constants::jsonrpc::g_CONNECTION_CLOSED) << message;
    EXPECT_NE(message.find("timeout"), std::string::npos) << message;
}

// A GET stream the server ends at once is opened again no sooner than 100 ms later, whatever its
// `retry` asks for, and the pause doubles each time a stream ends again carrying nothing. Only lower
// bounds are checked, so a slow machine cannot fail the test.
TEST(HttpClientLegacyStream, StreamsThatEndEmptyAreOpenedLessAndLessOften) {
    EndingStreams streams{"retry: 0\n\n", {}};
    run_ending_streams(streams, 4);
    ASSERT_GE(streams.gets.size(), 4u);
    EXPECT_GE(streams.gets[1] - streams.gets[0], std::chrono::milliseconds(100));
    EXPECT_GE(streams.gets[2] - streams.gets[1], std::chrono::milliseconds(200));
    EXPECT_GE(streams.gets[3] - streams.gets[2], std::chrono::milliseconds(400));
}

// A stream that carries an event before it ends is opened again after its `retry`, and no sooner
// than 100 ms.
TEST(HttpClientLegacyStream, StreamWithAnEventIsOpenedAgainNoSoonerThanTheFloor) {
    const json note = {{"jsonrpc", "2.0"},
                       {"method", "notifications/message"},
                       {"params", {{"level", "info"}, {"data", "tick"}}}};
    EndingStreams streams{"retry: 0\nid: e1\n" + sse_event(note), {}};
    run_ending_streams(streams, 4);
    ASSERT_GE(streams.gets.size(), 4u);
    for (std::size_t index = 1; index < 4; ++index) {
        EXPECT_GE(streams.gets[index] - streams.gets[index - 1], std::chrono::milliseconds(100))
            << "between GET " << index << " and the one before";
    }
}

}  // namespace transport_http_client_modern_test

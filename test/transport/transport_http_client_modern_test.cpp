#include "../test_utils.hpp"

#include "../../src/detail/header_value.hpp"
#include "mcp/client/client.hpp"
#include "mcp/server/server.hpp"
#include "mcp/transport/http_client.hpp"
#include "mcp/transport/http_server.hpp"
#include "mcp/transport/http_session_manager.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using nlohmann::json;
using Headers = std::vector<std::pair<std::string, std::string>>;

/// Only a guard against a hang: no test asserts that anything finishes within it.
constexpr auto g_hang_guard = std::chrono::seconds(30);

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

    void start() { asio::co_spawn(io_, accept_loop(this), asio::detached); }

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
            asio::co_spawn(self->io_, serve_connection(self, socket), asio::detached);
        }
    }

    static mcp::Task<void> watch_for_eof(std::shared_ptr<asio::ip::tcp::socket> socket,
                                         std::shared_ptr<bool> gone) {
        std::array<char, 64> sink{};
        try {
            for (;;) {
                co_await socket->async_read_some(asio::buffer(sink), asio::use_awaitable);
            }
        } catch (const std::exception&) {
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
        asio::co_spawn(self->io_, watch_for_eof(socket, gone), asio::detached);
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
                if (!reply.stream.empty()) {
                    co_await stream_reply(self, socket, std::move(reply));
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
    return json_reply(rpc_error(message, mcp::g_METHOD_NOT_FOUND, "no such method"), 404);
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
    asio::co_spawn(
        io,
        [&guard, &raw, body = std::move(body)]() -> mcp::Task<void> {
            co_await body();
            raw.close();
            guard.cancel();
        },
        asio::detached);
    io.run();
    EXPECT_FALSE(fired) << "the test did not finish";
}

mcp::ClientOptions modern_options() {
    mcp::ClientOptions options;
    options.protocol = mcp::ClientProtocol::Version("2026-07-28");
    return options;
}

mcp::ClientOptions auto_options() {
    mcp::ClientOptions options;
    options.protocol = mcp::ClientProtocol::Auto();
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

// ---------------------------------------------------------------------------
// Headers of a modern request
// ---------------------------------------------------------------------------

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

TEST(HttpClientModern, HeaderMismatchRefreshThenRetry) {
    asio::io_context io;
    RawServer raw(io, [](const Recorded& request) {
        const auto message = request.parsed();
        if (message.value("method", "") == "tools/call" && !request.header("Mcp-Param-Region")) {
            return json_reply(
                rpc_error(message, mcp::g_HEADER_MISMATCH, "Missing Mcp-Param-Region header"), 400);
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
            return json_reply(rpc_error(message, mcp::g_HEADER_MISMATCH, "Header mismatch"), 400);
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

    EXPECT_EQ(code, mcp::g_HEADER_MISMATCH);
    EXPECT_EQ(raw.rpc_methods(),
              (std::vector<std::string>{"server/discover", "tools/call", "tools/list", "tools/call"}));
}

// ---------------------------------------------------------------------------
// Auto detection over HTTP
// ---------------------------------------------------------------------------

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
                rpc_error(message, mcp::g_UNSUPPORTED_PROTOCOL_VERSION, "Unsupported protocol version",
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
        return json_reply(rpc_error(request.parsed(), mcp::g_HEADER_MISMATCH, "Header mismatch"), 400);
    });
    auto transport = make_transport(io, raw.url());
    mcp::Client client(transport, io.get_executor(), auto_options());

    int code = 0;
    run_guarded(io, raw, [&]() -> mcp::Task<void> {
        code = co_await failure_code(
            [&]() -> mcp::Task<void> { co_await client.connect("stays", "1.0"); });
        client.close();
    });

    EXPECT_EQ(code, mcp::g_HEADER_MISMATCH);
    EXPECT_EQ(raw.rpc_methods(), (std::vector<std::string>{"server/discover"}));
}

TEST(HttpClientModern, Fallback400EmptyBodyGoesLegacy) { expect_legacy_fallback(status_reply(400)); }

TEST(HttpClientModern, Fallback400NonJsonBodyGoesLegacy) {
    expect_legacy_fallback(status_reply(400, "Bad Request"));
}

TEST(HttpClientModern, Fallback400UnrecognizedJsonRpcErrorGoesLegacy) {
    const json error = {{"jsonrpc", "2.0"},
                        {"id", nullptr},
                        {"error", {{"code", mcp::g_INVALID_REQUEST}, {"message", "Missing session"}}}};
    expect_legacy_fallback(status_reply(400, error.dump()));
}

TEST(HttpClientModern, Fallback404Legacy) { expect_legacy_fallback(status_reply(404)); }

TEST(HttpClientModern, Fallback405Legacy) { expect_legacy_fallback(status_reply(405)); }

// ---------------------------------------------------------------------------
// No session in the modern era
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// Streamed answers
// ---------------------------------------------------------------------------

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
        asio::co_spawn(
            io,
            [&]() -> mcp::Task<void> {
                code = co_await failure_code([&]() -> mcp::Task<void> {
                    json arguments = json::object();
                    co_await client.call_tool("slow", arguments);
                });
                finished = true;
            },
            asio::detached);
        co_await wait_until([&raw] { return raw.streams_started == 1; });
        const auto id = raw.requests.back().parsed().at("id").get<std::string>();
        co_await client.cancel(mcp::RequestId{id});
        co_await wait_until([&] { return finished && raw.streams_abandoned == 1; });
        client.close();
    });

    EXPECT_TRUE(finished);
    EXPECT_EQ(code, mcp::g_REQUEST_CANCELLED);
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

    EXPECT_EQ(code, mcp::g_REQUEST_TIMEOUT);
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
        asio::co_spawn(
            io,
            [&]() -> mcp::Task<void> {
                code = co_await failure_code([&]() -> mcp::Task<void> {
                    json arguments = json::object();
                    co_await client.call_tool("slow", arguments);
                });
                finished = true;
            },
            asio::detached);
        co_await wait_until([&raw] { return raw.streams_started == 1; });
        client.close();
        co_await wait_until([&] { return finished && raw.streams_abandoned == 1; });
    });

    EXPECT_TRUE(finished);
    EXPECT_NE(code, 0);
    EXPECT_EQ(raw.streams_abandoned, 1);
}

// ---------------------------------------------------------------------------
// Against the SDK's own servers
// ---------------------------------------------------------------------------

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
    server->add_tool<json, json>(
        "héllo wörld", "A name outside the header-safe characters", json{{"type", "object"}},
        [](json /*args*/) -> mcp::Task<json> { co_return json{{"ok", true}}; });
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
    const auto encoded = co_await client.call_tool("héllo wörld", no_arguments);
    outcome.encoded = *encoded.structuredContent;
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
    mcp::StreamableHttpSessionManager manager(
        io.get_executor(), "127.0.0.1", port,
        [](const asio::any_io_executor&) { return make_sdk_server(); });
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
    asio::co_spawn(io, manager.listen(), asio::detached);
    asio::co_spawn(
        io,
        [&]() -> mcp::Task<void> {
            try {
                co_await sdk_exchange(client, outcome, true);
            } catch (const std::exception& failure) {
                error = failure.what();
            }
            sessions = manager.session_count();
            client.close();
            manager.close();
            guard.cancel();
        },
        asio::detached);
    io.run();

    EXPECT_FALSE(fired);
    EXPECT_EQ(error, "");
    EXPECT_EQ(outcome.connected.era, mcp::ProtocolEra::eModern);
    EXPECT_EQ(outcome.whoami.at("modern"), true);
    EXPECT_EQ(outcome.routed.at("region"), "Hello, 世界");
    EXPECT_EQ(outcome.encoded.at("ok"), true);
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
    asio::co_spawn(io, server_transport->listen(), asio::detached);
    asio::co_spawn(
        io,
        [&]() -> mcp::Task<void> {
            try {
                co_await server->run(server_transport, io.get_executor());
            } catch (...) {
            }
        },
        asio::detached);
    asio::co_spawn(
        io,
        [&]() -> mcp::Task<void> {
            try {
                co_await sdk_exchange(client, outcome, false);
            } catch (const std::exception& failure) {
                error = failure.what();
            }
            client.close();
            server_transport->close();
            guard.cancel();
        },
        asio::detached);
    io.run();

    EXPECT_FALSE(fired);
    EXPECT_EQ(error, "");
    EXPECT_EQ(outcome.connected.era, mcp::ProtocolEra::eModern);
    EXPECT_EQ(outcome.whoami.at("modern"), true);
    EXPECT_EQ(outcome.routed.at("count"), 7);
    EXPECT_EQ(outcome.encoded.at("ok"), true);
    EXPECT_FALSE(server->is_initialized());
}

// ---------------------------------------------------------------------------
// Pinned legacy versions
// ---------------------------------------------------------------------------

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

    void start() { asio::co_spawn(io_, accept_loop(this), asio::detached); }

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
            asio::co_spawn(self->io_, pipe(client, server, &self->captured_), asio::detached);
            asio::co_spawn(self->io_, pipe(server, client, nullptr), asio::detached);
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
            [](const asio::any_io_executor&) { return make_sdk_server(); });
        if (policy) {
            manager->set_protocol_policy(*policy);
        }
        asio::co_spawn(io, manager->listen(), asio::detached);
    } else {
        server_transport =
            std::make_shared<mcp::HttpServerTransport>(io.get_executor(), "127.0.0.1", 0);
        server = make_sdk_server();
        if (policy) {
            server->set_protocol_policy(*policy);
        }
        upstream = server_transport->port();
        asio::co_spawn(io, server_transport->listen(), asio::detached);
        asio::co_spawn(
            io,
            [&]() -> mcp::Task<void> {
                try {
                    co_await server->run(server_transport, io.get_executor());
                } catch (...) {
                }
            },
            asio::detached);
    }

    RecordingRelay relay(io, upstream);
    relay.start();
    auto transport = make_transport(io, relay.url());
    mcp::ClientOptions options;
    options.protocol = mcp::ClientProtocol::Version(pinned);
    mcp::Client client(transport, io.get_executor(), options);

    guard.expires_after(g_hang_guard);
    guard.async_wait([&](const boost::system::error_code& code) {
        if (!code) {
            fired = true;
            io.stop();
        }
    });
    asio::co_spawn(
        io,
        [&]() -> mcp::Task<void> {
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
        },
        asio::detached);
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

// ---------------------------------------------------------------------------
// Errors a legacy client gets with an HTTP error status
// ---------------------------------------------------------------------------

namespace {

/// A legacy client connecting to a raw server that answers initialize with @p status and @p body.
std::string legacy_connect_error(unsigned int status, std::function<json(const json&)> body) {
    asio::io_context io;
    RawServer raw(io,
                  [&](const Recorded& request) { return json_reply(body(request.parsed()), status); });
    mcp::ClientOptions options;
    options.protocol = mcp::ClientProtocol::Legacy();
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

// The body of such an error may leave the id out or null; it still answers the initialize.
TEST(HttpClientLegacy, InitializeErrorWithoutAnIdStillReachesTheCaller) {
    const auto error = legacy_connect_error(400, [](const json&) {
        return json{{"jsonrpc", "2.0"},
                    {"id", nullptr},
                    {"error", {{"code", -32600}, {"message", "Bad Request: unsupported version"}}}};
    });
    EXPECT_NE(error.find("unsupported version"), std::string::npos) << error;
}

// A body that is not a JSON-RPC error leaves the plain HTTP status error.
TEST(HttpClientLegacy, InitializeRefusedWithoutAJsonRpcErrorReportsTheStatus) {
    const auto error = legacy_connect_error(400, [](const json&) { return json{{"detail", "no"}}; });
    EXPECT_NE(error.find("status 400"), std::string::npos) << error;
}

// Inside a session an error status keeps its HTTP meaning: a 404 there says the session is gone.
TEST(HttpClientLegacy, ErrorStatusInsideASessionStaysAnHttpStatusError) {
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
    options.protocol = mcp::ClientProtocol::Legacy();
    mcp::Client client(make_transport(io, raw.url()), io.get_executor(), options);
    std::optional<unsigned int> status;
    std::string other;
    run_guarded(io, raw, [&]() -> mcp::Task<void> {
        try {
            co_await client.connect("legacy", "1.0");
            co_await client.list_tools();
        } catch (const mcp::HttpStatusError& failure) {
            status = failure.status();
        } catch (const std::exception& failure) {
            other = failure.what();
        }
        client.close();
    });
    EXPECT_EQ(status, 404u) << other;
}

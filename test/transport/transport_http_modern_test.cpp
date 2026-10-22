#include "../test_utils.hpp"

#include "../../src/detail/http_request_metadata.hpp"
#include "mcp/server/server.hpp"
#include "mcp/transport/http_session_manager.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
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
    std::string content_type;
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
        request.set(name, value);
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
    reply.content_type = std::string(response[http::field::content_type]);
    reply.allow = std::string(response[http::field::allow]);
    if (auto session = response.find("Mcp-Session-Id"); session != response.end()) {
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
    } else if (method == "resources/read") {
        headers.emplace_back("Mcp-Name", body["params"]["uri"]);
    }
    return headers;
}

Headers without(Headers headers, const std::string& name) {
    std::erase_if(headers, [&name](const auto& header) { return header.first == name; });
    return headers;
}

Headers with(Headers headers, const std::string& name, const std::string& value) {
    headers = without(std::move(headers), name);
    headers.emplace_back(name, value);
    return headers;
}

json call_tool(const json& id, const std::string& name) {
    return make_modern_request(id, "tools/call", {{"name", name}, {"arguments", json::object()}});
}

std::unique_ptr<mcp::Server> make_server(const std::string& name) {
    mcp::ServerCapabilities caps;
    caps.tools = mcp::ServerCapabilities::ToolsCapability{};
    caps.resources = mcp::ServerCapabilities::ResourcesCapability{};
    auto server = std::make_unique<mcp::Server>(mcp::Implementation{name, "1.0"}, caps);
    server->add_tool<json, json>("echo", "Echoes its arguments", json{{"type", "object"}},
                                 [](json args) -> mcp::Task<json> { co_return args; });
    server->add_tool<json, json>(
        "héllo wörld", "A name outside the header-safe characters", json{{"type", "object"}},
        [](json /*args*/) -> mcp::Task<json> { co_return json{{"ok", true}}; });
    mcp::Resource resource;
    resource.uri = "test://doc";
    resource.name = "doc";
    server->add_resource<json, json>(resource, [](json /*params*/) -> mcp::Task<json> {
        co_return json{{"contents", json::array({{{"uri", "test://doc"}, {"text", "doc"}}})}};
    });
    return server;
}

class HttpModernTest : public ::testing::Test {
   protected:
    asio::io_context io_ctx_;
    std::atomic<int> factory_calls_{0};

    mcp::StreamableHttpSessionManager::ServerFactory factory() {
        return [this](const asio::any_io_executor&) {
            factory_calls_.fetch_add(1);
            return make_server("factory-server");
        };
    }

    /// Runs @p exchange against a manager listening on @p port, then closes the manager.
    template <typename Exchange>
    void run(mcp::StreamableHttpSessionManager& manager, Exchange exchange) {
        io_ctx_.restart();
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

    mcp::Task<Reply> post(unsigned short port, const json& body, Headers headers) {
        return send(io_ctx_.get_executor(), port, http::verb::post, body.dump(), std::move(headers));
    }

    mcp::Task<Reply> post_modern(unsigned short port, const json& body) {
        return post(port, body, modern_headers(body));
    }

    /// Posts @p body with @p headers to a fresh manager and returns the single reply.
    Reply exchange_one(unsigned short port, const json& body, const Headers& headers) {
        mcp::StreamableHttpSessionManager manager(io_ctx_.get_executor(), "127.0.0.1", port, factory());
        Reply reply;
        run(manager, [&]() -> mcp::Task<void> { reply = co_await post(port, body, headers); });
        return reply;
    }

    static void expect_error(const Reply& reply, unsigned int status, int code) {
        EXPECT_EQ(reply.status, status) << reply.body;
        const auto body = json::parse(reply.body);
        ASSERT_TRUE(body.contains("error")) << reply.body;
        EXPECT_EQ(body["error"]["code"], code) << reply.body;
    }
};

TEST(HttpModernStatus, ErrorCodesMapToTheSpecifiedStatuses) {
    using mcp::detail::http_status_for_error;
    EXPECT_EQ(http_status_for_error(mcp::g_INVALID_PARAMS), http::status::bad_request);
    EXPECT_EQ(http_status_for_error(mcp::g_HEADER_MISMATCH), http::status::bad_request);
    EXPECT_EQ(http_status_for_error(mcp::g_MISSING_REQUIRED_CLIENT_CAPABILITY),
              http::status::bad_request);
    EXPECT_EQ(http_status_for_error(mcp::g_UNSUPPORTED_PROTOCOL_VERSION), http::status::bad_request);
    EXPECT_EQ(http_status_for_error(mcp::g_METHOD_NOT_FOUND), http::status::not_found);
    EXPECT_EQ(http_status_for_error(mcp::g_INTERNAL_ERROR), http::status::ok);
    EXPECT_EQ(http_status_for_error(mcp::g_INVALID_REQUEST), http::status::ok);
}

TEST(HttpModernHeaderValue, Base64SentinelIsDecoded) {
    using mcp::detail::decode_header_value;
    EXPECT_EQ(decode_header_value("us-west1"), "us-west1");
    EXPECT_EQ(decode_header_value("=?base64?SGVsbG8sIOS4lueVjA==?="), "Hello, 世界");
    EXPECT_EQ(decode_header_value("=?base64?IHBhZGRlZCA=?="), " padded ");
    EXPECT_EQ(decode_header_value("=?base64?PT9iYXNlNjQ/bGl0ZXJhbD89?="), "=?base64?literal?=");
    EXPECT_EQ(decode_header_value("=?BASE64?SGk=?="), "=?BASE64?SGk=?=");  // markers are lowercase
    EXPECT_EQ(decode_header_value("=?base64?not base64?="), std::nullopt);
    EXPECT_EQ(decode_header_value("=?base64?SGk=SGk=?="), std::nullopt);
    EXPECT_EQ(decode_header_value("caf\xC3\xA9"), std::nullopt);
    EXPECT_EQ(decode_header_value("a\x01"), std::nullopt);
}

TEST_F(HttpModernTest, ServesAModernRequestWithoutASession) {
    const unsigned short port = 20400;
    mcp::StreamableHttpSessionManager manager(io_ctx_.get_executor(), "127.0.0.1", port, factory());
    Reply reply;
    std::size_t sessions = 0;
    run(manager, [&]() -> mcp::Task<void> {
        reply = co_await post_modern(port, call_tool(1, "echo"));
        sessions = manager.session_count();
    });

    EXPECT_EQ(reply.status, 200u) << reply.body;
    EXPECT_EQ(reply.content_type, "application/json");
    EXPECT_FALSE(reply.session_id.has_value());
    EXPECT_EQ(sessions, 0u);
    EXPECT_TRUE(reply.parsed().contains("result")) << reply.body;
}

TEST_F(HttpModernTest, MissingProtocolVersionHeaderIsHeaderMismatch) {
    const auto body = call_tool(1, "echo");
    expect_error(exchange_one(20401, body, without(modern_headers(body), "MCP-Protocol-Version")), 400,
                 mcp::g_HEADER_MISMATCH);
}

TEST_F(HttpModernTest, ProtocolVersionHeaderMustMatchTheBody) {
    const auto body = call_tool(1, "echo");
    expect_error(
        exchange_one(20402, body, with(modern_headers(body), "MCP-Protocol-Version", "2026-07-29")),
        400, mcp::g_HEADER_MISMATCH);
}

TEST_F(HttpModernTest, McpMethodIsRequiredAndMustMatch) {
    const auto body = make_modern_request(1, "tools/list");
    expect_error(exchange_one(20403, body, without(modern_headers(body), "Mcp-Method")), 400,
                 mcp::g_HEADER_MISMATCH);
    expect_error(exchange_one(20404, body, with(modern_headers(body), "Mcp-Method", "Tools/List")), 400,
                 mcp::g_HEADER_MISMATCH);
}

TEST_F(HttpModernTest, McpNameIsRequiredForTheThreeNamedMethodsOnly) {
    const unsigned short port = 20405;
    mcp::StreamableHttpSessionManager manager(io_ctx_.get_executor(), "127.0.0.1", port, factory());
    Reply call;
    Reply read;
    Reply list;
    run(manager, [&]() -> mcp::Task<void> {
        const auto call_body = call_tool(1, "echo");
        call = co_await post(port, call_body, without(modern_headers(call_body), "Mcp-Name"));
        const auto read_body = make_modern_request(2, "resources/read", {{"uri", "test://doc"}});
        read = co_await post(port, read_body, without(modern_headers(read_body), "Mcp-Name"));
        const auto list_body = make_modern_request(3, "tools/list");
        list = co_await post(port, list_body, modern_headers(list_body));
    });

    expect_error(call, 400, mcp::g_HEADER_MISMATCH);
    expect_error(read, 400, mcp::g_HEADER_MISMATCH);
    EXPECT_EQ(list.status, 200u) << list.body;
}

TEST_F(HttpModernTest, McpNameMustMatchTheBody) {
    const auto body = call_tool(1, "echo");
    expect_error(exchange_one(20406, body, with(modern_headers(body), "Mcp-Name", "other")), 400,
                 mcp::g_HEADER_MISMATCH);
}

TEST_F(HttpModernTest, McpNameInTheBase64SentinelIsDecodedBeforeComparing) {
    const unsigned short port = 20407;
    mcp::StreamableHttpSessionManager manager(io_ctx_.get_executor(), "127.0.0.1", port, factory());
    Reply encoded;
    Reply undecodable;
    run(manager, [&]() -> mcp::Task<void> {
        const auto body = call_tool(1, "héllo wörld");
        encoded = co_await post(
            port, body, with(modern_headers(body), "Mcp-Name", "=?base64?aMOpbGxvIHfDtnJsZA==?="));
        undecodable =
            co_await post(port, body, with(modern_headers(body), "Mcp-Name", "=?base64?!!?="));
    });

    EXPECT_EQ(encoded.status, 200u) << encoded.body;
    EXPECT_TRUE(encoded.parsed().contains("result")) << encoded.body;
    expect_error(undecodable, 400, mcp::g_HEADER_MISMATCH);
}

TEST_F(HttpModernTest, HeaderNamesAreCaseInsensitive) {
    const auto body = call_tool(1, "echo");
    const Headers lower = {
        {"mcp-protocol-version", "2026-07-28"}, {"mcp-method", "tools/call"}, {"mcp-name", "echo"}};
    const auto reply = exchange_one(20408, body, lower);
    EXPECT_EQ(reply.status, 200u) << reply.body;
}

TEST_F(HttpModernTest, UnsupportedVersionIs400WithTheServedVersions) {
    auto body = make_modern_request(1, "tools/list");
    body["params"]["_meta"]["io.modelcontextprotocol/protocolVersion"] = "1900-01-01";
    const auto reply = exchange_one(20409, body, modern_headers(body));

    expect_error(reply, 400, mcp::g_UNSUPPORTED_PROTOCOL_VERSION);
    EXPECT_EQ(reply.parsed()["error"]["data"]["requested"], "1900-01-01");
    EXPECT_EQ(reply.parsed()["error"]["data"]["supported"],
              (json{"2024-11-05", "2025-03-26", "2025-06-18", "2025-11-25", "2026-07-28"}));
}

TEST_F(HttpModernTest, UnknownMethodIs404WithAJsonRpcBody) {
    const auto body = make_modern_request(7, "no/such/method");
    const auto reply = exchange_one(20410, body, modern_headers(body));
    expect_error(reply, 404, mcp::g_METHOD_NOT_FOUND);
    EXPECT_EQ(reply.parsed()["id"], 7);
}

TEST_F(HttpModernTest, UnknownToolAndMissingCapabilitiesAre400InvalidParams) {
    const unsigned short port = 20411;
    mcp::StreamableHttpSessionManager manager(io_ctx_.get_executor(), "127.0.0.1", port, factory());
    Reply unknown_tool;
    Reply missing_capabilities;
    run(manager, [&]() -> mcp::Task<void> {
        unknown_tool = co_await post_modern(port, call_tool(1, "missing"));
        auto body = make_modern_request(2, "tools/list");
        body["params"]["_meta"].erase("io.modelcontextprotocol/clientCapabilities");
        missing_capabilities = co_await post_modern(port, body);
    });
    expect_error(unknown_tool, 400, mcp::g_INVALID_PARAMS);
    expect_error(missing_capabilities, 400, mcp::g_INVALID_PARAMS);
}

TEST_F(HttpModernTest, NotificationPostIsAccepted) {
    const json notification = {{"jsonrpc", "2.0"},
                               {"method", "notifications/cancelled"},
                               {"params", {{"requestId", 1}, {"_meta", make_modern_meta()}}}};
    const auto reply = exchange_one(20412, notification, {{"MCP-Protocol-Version", "2026-07-28"}});
    EXPECT_EQ(reply.status, 202u) << reply.body;
    EXPECT_TRUE(reply.body.empty());
}

TEST_F(HttpModernTest, SessionAndEventIdHeadersAreIgnoredAndNeverEchoed) {
    const unsigned short port = 20413;
    mcp::StreamableHttpSessionManager manager(io_ctx_.get_executor(), "127.0.0.1", port, factory());
    Reply reply;
    std::size_t sessions = 1;
    run(manager, [&]() -> mcp::Task<void> {
        const auto body = call_tool(1, "echo");
        auto headers = modern_headers(body);
        headers.emplace_back("Mcp-Session-Id", "no-such-session");
        headers.emplace_back("Last-Event-ID", "42");
        reply = co_await post(port, body, headers);
        sessions = manager.session_count();
    });
    EXPECT_EQ(reply.status, 200u) << reply.body;
    EXPECT_FALSE(reply.session_id.has_value());
    EXPECT_EQ(sessions, 0u);
}

TEST_F(HttpModernTest, GetAndDeleteWithAModernHeaderAndNoSessionAre405) {
    const unsigned short port = 20414;
    mcp::StreamableHttpSessionManager manager(io_ctx_.get_executor(), "127.0.0.1", port, factory());
    Reply get;
    Reply del;
    Reply legacy_get;
    const Headers modern = {{"MCP-Protocol-Version", "2026-07-28"}};
    const Headers legacy = {{"MCP-Protocol-Version", "2025-11-25"}};
    run(manager, [&]() -> mcp::Task<void> {
        get = co_await send(io_ctx_.get_executor(), port, http::verb::get, "", modern);
        del = co_await send(io_ctx_.get_executor(), port, http::verb::delete_, "", modern);
        legacy_get = co_await send(io_ctx_.get_executor(), port, http::verb::get, "", legacy);
    });
    EXPECT_EQ(get.status, 405u);
    EXPECT_EQ(get.allow, "POST");
    EXPECT_EQ(del.status, 405u);
    EXPECT_EQ(del.allow, "POST");
    EXPECT_EQ(legacy_get.status, 400u) << legacy_get.body;  // unchanged: missing Mcp-Session-Id
}

TEST_F(HttpModernTest, LegacySessionAndModernRequestsShareTheEndpoint) {
    const unsigned short port = 20415;
    mcp::StreamableHttpSessionManager manager(io_ctx_.get_executor(), "127.0.0.1", port, factory());
    Reply initialize;
    Reply modern;
    Reply legacy_call;
    const Headers legacy = {{"MCP-Protocol-Version", "2025-11-25"}};
    run(manager, [&]() -> mcp::Task<void> {
        initialize = co_await post(port, make_initialize_request("i"), legacy);
        const Headers session = {{"MCP-Protocol-Version", "2025-11-25"},
                                 {"Mcp-Session-Id", initialize.session_id.value_or("")}};
        co_await post(port, make_initialized_notification(), session);
        modern = co_await post_modern(port, call_tool(1, "echo"));
        legacy_call = co_await post(port, make_tool_call_request("c", "echo", json::object()), session);
    });

    ASSERT_EQ(initialize.status, 200u) << initialize.body;
    ASSERT_TRUE(initialize.session_id.has_value());
    // A legacy session may answer as an SSE stream, so look for the negotiated version in the text.
    EXPECT_NE(initialize.body.find(R"("protocolVersion":"2025-11-25")"), std::string::npos)
        << initialize.body;
    EXPECT_EQ(modern.status, 200u) << modern.body;
    EXPECT_FALSE(modern.session_id.has_value());
    EXPECT_EQ(legacy_call.status, 200u) << legacy_call.body;
    EXPECT_EQ(legacy_call.session_id, initialize.session_id);
}

// 2026-07-28 has no initialize: with that version in its header, initialize is a method the
// endpoint does not implement. The error names the versions served. A version the endpoint does
// not know at all keeps the legacy refusal.
TEST_F(HttpModernTest, InitializeWithAModernHeaderIsMethodNotFound) {
    const unsigned short port = 20416;
    mcp::StreamableHttpSessionManager manager(io_ctx_.get_executor(), "127.0.0.1", port, factory());
    Reply modern;
    Reply unknown;
    std::size_t sessions = 1;
    const Headers modern_header = {{"MCP-Protocol-Version", "2026-07-28"}};
    const Headers unknown_header = {{"MCP-Protocol-Version", "2099-01-01"}};
    run(manager, [&]() -> mcp::Task<void> {
        modern = co_await post(port, make_initialize_request("i"), modern_header);
        unknown = co_await post(port, make_initialize_request("u"), unknown_header);
        sessions = manager.session_count();
    });

    expect_error(modern, 404, mcp::g_METHOD_NOT_FOUND);
    EXPECT_EQ(modern.parsed()["id"], "i");
    EXPECT_EQ(modern.parsed()["error"]["data"]["supported"],
              (json{"2024-11-05", "2025-03-26", "2025-06-18", "2025-11-25", "2026-07-28"}));
    EXPECT_FALSE(modern.session_id.has_value());
    EXPECT_EQ(unknown.status, 400u);
    EXPECT_EQ(unknown.body, R"({"error":"Invalid MCP-Protocol-Version header"})");
    EXPECT_EQ(sessions, 0u);
}

TEST_F(HttpModernTest, StatelessJsonModeServesModernRequests) {
    const unsigned short port = 20417;
    mcp::StreamableHttpSessionManager manager(io_ctx_.get_executor(), "127.0.0.1", port, factory());
    manager.set_stateless_json_mode(true);
    Reply reply;
    run(manager,
        [&]() -> mcp::Task<void> { reply = co_await post_modern(port, call_tool(1, "echo")); });
    EXPECT_EQ(reply.status, 200u) << reply.body;
    EXPECT_TRUE(reply.parsed().contains("result"));
}

TEST_F(HttpModernTest, OneModernServerAnswersEveryModernRequest) {
    const unsigned short port = 20418;
    mcp::StreamableHttpSessionManager manager(io_ctx_.get_executor(), "127.0.0.1", port, factory());
    std::vector<unsigned int> statuses;
    std::shared_ptr<mcp::Server> before;
    std::shared_ptr<mcp::Server> after;
    run(manager, [&]() -> mcp::Task<void> {
        for (int id = 0; id < 3; ++id) {
            const auto reply = co_await post_modern(port, make_modern_request(id, "tools/list"));
            statuses.push_back(reply.status);
        }
        before = manager.modern_server();
        const auto discover = co_await post_modern(port, make_discover_request(9));
        statuses.push_back(discover.status);
        after = manager.modern_server();
    });
    EXPECT_EQ(statuses, (std::vector<unsigned int>{200, 200, 200, 200}));
    EXPECT_EQ(before, after);
    EXPECT_EQ(factory_calls_.load(), 1);
}

TEST_F(HttpModernTest, SetModernServerReplacesTheFactoryBuiltOne) {
    const unsigned short port = 20419;
    mcp::StreamableHttpSessionManager manager(io_ctx_.get_executor(), "127.0.0.1", port, factory());
    std::shared_ptr<mcp::Server> custom = make_server("custom-server");
    manager.set_modern_server(custom);
    Reply reply;
    run(manager,
        [&]() -> mcp::Task<void> { reply = co_await post_modern(port, make_discover_request(1)); });

    EXPECT_EQ(reply.parsed()["result"]["_meta"]["io.modelcontextprotocol/serverInfo"]["name"],
              "custom-server");
    EXPECT_EQ(manager.modern_server(), custom);
    EXPECT_EQ(factory_calls_.load(), 0);
    EXPECT_THROW(manager.set_modern_server(nullptr), std::invalid_argument);
}

TEST_F(HttpModernTest, PolicyWithoutLegacyRefusesInitializeWithoutCreatingASession) {
    const unsigned short port = 20420;
    mcp::StreamableHttpSessionManager manager(io_ctx_.get_executor(), "127.0.0.1", port, factory());
    mcp::ServerProtocolPolicy policy;
    policy.serve_legacy = false;
    manager.set_protocol_policy(policy);
    Reply initialize;
    Reply modern;
    std::size_t sessions = 1;
    const Headers legacy = {{"MCP-Protocol-Version", "2025-11-25"}};
    run(manager, [&]() -> mcp::Task<void> {
        initialize = co_await post(port, make_initialize_request("i"), legacy);
        sessions = manager.session_count();
        modern = co_await post_modern(port, make_modern_request(1, "tools/list"));
    });

    expect_error(initialize, 404, mcp::g_METHOD_NOT_FOUND);
    EXPECT_EQ(initialize.parsed()["error"]["data"]["supported"], (json{"2026-07-28"}));
    EXPECT_FALSE(initialize.session_id.has_value());
    EXPECT_EQ(sessions, 0u);
    EXPECT_EQ(modern.status, 200u) << modern.body;
}

// Where no legacy client is served no session can exist, so GET and DELETE are not part of any
// protocol the endpoint speaks, whatever the request carries.
TEST_F(HttpModernTest, PolicyWithoutLegacyAnswersGetAndDeleteWith405) {
    const unsigned short port = 20443;
    mcp::StreamableHttpSessionManager manager(io_ctx_.get_executor(), "127.0.0.1", port, factory());
    mcp::ServerProtocolPolicy policy;
    policy.serve_legacy = false;
    manager.set_protocol_policy(policy);
    std::vector<Reply> replies;
    const std::vector<std::pair<http::verb, Headers>> requests = {
        {http::verb::get, {}},
        {http::verb::delete_, {}},
        {http::verb::get, {{"Mcp-Session-Id", "abc"}, {"Last-Event-ID", "1"}}},
        {http::verb::delete_, {{"Mcp-Session-Id", "abc"}}},
        {http::verb::get, {{"MCP-Protocol-Version", "2025-11-25"}}},
    };
    run(manager, [&]() -> mcp::Task<void> {
        for (const auto& [verb, headers] : requests) {
            auto reply = co_await send(io_ctx_.get_executor(), port, verb, "", headers);
            replies.push_back(std::move(reply));
        }
    });

    ASSERT_EQ(replies.size(), requests.size());
    for (const auto& reply : replies) {
        EXPECT_EQ(reply.status, 405u) << reply.body;
        EXPECT_EQ(reply.allow, "POST");
    }
}

TEST_F(HttpModernTest, PolicyIsValidatedAndFixedBeforeListening) {
    mcp::StreamableHttpSessionManager manager(io_ctx_.get_executor(), "127.0.0.1", 20421, factory());
    mcp::ServerProtocolPolicy neither;
    neither.serve_modern = false;
    neither.serve_legacy = false;
    EXPECT_THROW(manager.set_protocol_policy(neither), std::invalid_argument);
    run(manager, []() -> mcp::Task<void> { co_return; });
    EXPECT_THROW(manager.set_protocol_policy(mcp::ServerProtocolPolicy{}), std::logic_error);
}

TEST_F(HttpModernTest, HeaderMismatchesAreCounted) {
    const unsigned short port = 20422;
    mcp::StreamableHttpSessionManager manager(io_ctx_.get_executor(), "127.0.0.1", port, factory());
    run(manager, [&]() -> mcp::Task<void> {
        const auto body = call_tool(1, "echo");
        co_await post(port, body, without(modern_headers(body), "Mcp-Method"));
        co_await post(port, body, with(modern_headers(body), "Mcp-Name", "other"));
        co_await post_modern(port, body);
    });
    EXPECT_EQ(manager.header_mismatch_rejections(), 2u);
}

TEST_F(HttpModernTest, OriginAndBearerChecksStillComeFirst) {
    const unsigned short port = 20423;
    mcp::StreamableHttpSessionManager manager(io_ctx_.get_executor(), "127.0.0.1", port, factory());
    manager.set_bearer_token_validator([](std::string_view token) { return token == "good"; });
    Reply bad_origin;
    Reply no_token;
    Reply good;
    run(manager, [&]() -> mcp::Task<void> {
        const auto body = call_tool(1, "echo");
        bad_origin = co_await post(port, body,
                                   with(with(modern_headers(body), "Origin", "https://evil.example"),
                                        "Authorization", "Bearer good"));
        no_token = co_await post_modern(port, body);
        good = co_await post(port, body, with(modern_headers(body), "Authorization", "Bearer good"));
    });
    EXPECT_EQ(bad_origin.status, 403u);
    EXPECT_EQ(no_token.status, 401u);
    EXPECT_EQ(good.status, 200u) << good.body;
}

}  // namespace

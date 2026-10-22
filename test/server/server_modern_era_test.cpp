#include "../test_utils.hpp"

#include "../../src/detail/modern_methods.hpp"
#include "mcp/server/server.hpp"
#include "mcp/transport/websocket.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/use_future.hpp>
#include <chrono>
#include <future>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace {

using nlohmann::json;

const std::vector<std::string> g_all_versions = {"2024-11-05", "2025-03-26", "2025-06-18", "2025-11-25",
                                                 "2026-07-28"};

/// Waits on `ctx` until the request is cancelled, polling so the server's strand can deliver
/// the notification. Gives up after 30 s so a broken cancellation fails the test, not the run.
mcp::Task<bool> wait_for_cancel(mcp::Context& ctx) {
    auto executor = co_await boost::asio::this_coro::executor;
    const auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!ctx.is_cancelled() && std::chrono::steady_clock::now() < give_up) {
        boost::asio::steady_timer timer(executor, std::chrono::milliseconds(1));
        co_await timer.async_wait(boost::asio::use_awaitable);
    }
    co_return ctx.is_cancelled();
}

/// Waits until `flag` is set, polling like wait_for_cancel(), and gives up after 30 s.
mcp::Task<bool> wait_for_flag(const std::atomic<bool>& flag) {
    auto executor = co_await boost::asio::this_coro::executor;
    const auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!flag.load() && std::chrono::steady_clock::now() < give_up) {
        boost::asio::steady_timer timer(executor, std::chrono::milliseconds(1));
        co_await timer.async_wait(boost::asio::use_awaitable);
    }
    co_return flag.load();
}

class ServerModernEraTest : public ::testing::Test {
   protected:
    boost::asio::io_context io_ctx_;

    /// A server with one of everything a 2026-07-28 request can reach.
    static std::unique_ptr<mcp::Server> make_server() {
        mcp::ServerCapabilities caps;
        caps.tools = mcp::ServerCapabilities::ToolsCapability{};
        caps.resources = mcp::ServerCapabilities::ResourcesCapability{};
        caps.prompts = mcp::ServerCapabilities::PromptsCapability{};
        caps.completions = json::object();
        caps.logging = json::object();
        auto server = std::make_unique<mcp::Server>(mcp::Implementation{"modern-server", "1.0"}, caps);

        server->add_tool<json, json>(
            "whoami", "Reports the request's per-request fields", json{{"type", "object"}},
            [](mcp::Context& ctx, json /*args*/) -> mcp::Task<json> {
                const auto* meta = ctx.request_meta();
                if (meta == nullptr) {
                    co_return json{{"modern", false}};
                }
                co_return json{{"modern", true},
                               {"version", meta->protocolVersion},
                               {"client", meta->clientInfo ? meta->clientInfo->name : ""},
                               {"roots", meta->clientCapabilities.roots.has_value()},
                               {"meta", meta->meta}};
            });
        server->add_tool<json, json>("log", "Logs one message at each of error and debug",
                                     json{{"type", "object"}},
                                     [](mcp::Context& ctx, json /*args*/) -> mcp::Task<json> {
                                         co_await ctx.log(mcp::LoggingLevel::eError, "error message");
                                         co_await ctx.log(mcp::LoggingLevel::eDebug, "debug message");
                                         co_return json{{"logged", true}};
                                     });
        server->add_tool<json, json>("sample", "Asks the client to sample", json{{"type", "object"}},
                                     [](mcp::Context& ctx, json /*args*/) -> mcp::Task<json> {
                                         mcp::CreateMessageRequestParams params;
                                         params.maxTokens = 1;
                                         co_await ctx.sample_llm(params);
                                         co_return json{{"sampled", true}};
                                     });
        server->add_tool<json, json>("wait_for_cancel", "Returns once cancelled",
                                     json{{"type", "object"}},
                                     [](mcp::Context& ctx, json /*args*/) -> mcp::Task<json> {
                                         const bool cancelled = co_await wait_for_cancel(ctx);
                                         co_return json{{"cancelled", cancelled}};
                                     });

        mcp::Resource resource;
        resource.uri = "test://doc";
        resource.name = "doc";
        server->add_resource<json, json>(
            resource, [](mcp::Context& ctx, json /*params*/) -> mcp::Task<json> {
                const bool modern = ctx.request_meta() != nullptr;
                co_return json{{"contents", json::array({{{"uri", "test://doc"},
                                                          {"text", modern ? "modern" : "legacy"}}})}};
            });

        mcp::Prompt prompt;
        prompt.name = "greet";
        server->add_prompt<json, json>(
            prompt, [](mcp::Context& ctx, json /*params*/) -> mcp::Task<json> {
                const bool modern = ctx.request_meta() != nullptr;
                co_return json{
                    {"messages",
                     json::array(
                         {{{"role", "user"},
                           {"content", {{"type", "text"}, {"text", modern ? "modern" : "legacy"}}}}})}};
            });

        server->set_completion_provider(
            [](const mcp::CompleteParams& /*params*/) -> mcp::Task<mcp::CompleteResult> {
                mcp::CompleteResult result;
                result.completion.values = {"done"};
                co_return result;
            });
        return server;
    }

    json dispatch(mcp::Server& server, json request) {
        json response;
        boost::asio::co_spawn(io_ctx_, server.dispatch_request_direct(std::move(request)),
                              [&response](std::exception_ptr error, std::string wire) {
                                  EXPECT_EQ(error, nullptr);
                                  if (!error) {
                                      response = json::parse(wire);
                                  }
                              });
        io_ctx_.restart();
        io_ctx_.run();
        return response;
    }

    /// Runs `server` on a scripted session fed `messages` and returns what it writes, once
    /// `expected` messages have been written. A server that writes fewer is stopped after 30 s,
    /// so the test fails on the count instead of hanging.
    std::vector<json> run_session(mcp::Server& server, const std::vector<json>& messages,
                                  std::size_t expected) {
        auto transport = std::make_shared<ScriptedTransport>(io_ctx_.get_executor());
        auto* raw_transport = transport.get();
        auto deadline = std::make_shared<boost::asio::steady_timer>(io_ctx_, std::chrono::seconds(30));
        deadline->async_wait([raw_transport](const boost::system::error_code& error) {
            if (!error) {
                raw_transport->close();
            }
        });
        std::vector<json> written;
        raw_transport->set_on_write(
            [&written, raw_transport, expected, deadline](std::string_view msg) {
                written.push_back(json::parse(msg));
                if (written.size() == expected) {
                    deadline->cancel();
                    raw_transport->close();
                }
            });
        for (const auto& message : messages) {
            raw_transport->enqueue_message(message.dump());
        }
        boost::asio::co_spawn(
            io_ctx_,
            [&server, transport, this]() -> mcp::Task<void> {
                co_await server.run(transport, io_ctx_.get_executor());
            },
            boost::asio::detached);
        io_ctx_.restart();
        io_ctx_.run();
        return written;
    }

    static json modern_with_meta(const json& id, std::string_view method, json meta,
                                 json params = json::object()) {
        params["_meta"] = std::move(meta);
        return {{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", std::move(params)}};
    }

    static json call_tool(const json& id, std::string_view tool) {
        return make_modern_request(id, "tools/call", {{"name", tool}, {"arguments", json::object()}});
    }

    static json legacy_call_tool(const json& id, std::string_view tool) {
        return {{"jsonrpc", "2.0"},
                {"id", id},
                {"method", "tools/call"},
                {"params", {{"name", tool}, {"arguments", json::object()}}}};
    }

    static void expect_error(const json& response, int code) {
        ASSERT_TRUE(response.contains("error")) << response;
        EXPECT_EQ(response["error"]["code"], code) << response;
    }
};

TEST_F(ServerModernEraTest, ToolsListWithoutInitialize) {
    auto server = make_server();
    auto written = run_session(*server, {make_modern_request(1, "tools/list")}, 1);

    ASSERT_EQ(written.size(), 1u);
    ASSERT_TRUE(written[0].contains("result")) << written[0];
    EXPECT_EQ(written[0]["id"], 1);
    EXPECT_EQ(written[0]["result"]["tools"].size(), 4u);
    EXPECT_FALSE(server->is_initialized());
}

TEST_F(ServerModernEraTest, EveryModernMethodIsServedWithoutInitialize) {
    auto server = make_server();
    std::vector<json> requests = {
        make_discover_request(1),
        make_modern_request(2, "tools/list"),
        call_tool(3, "whoami"),
        make_modern_request(4, "resources/list"),
        make_modern_request(5, "resources/read", {{"uri", "test://doc"}}),
        make_modern_request(6, "resources/templates/list"),
        make_modern_request(7, "prompts/list"),
        make_modern_request(8, "prompts/get", {{"name", "greet"}}),
        make_modern_request(9, "completion/complete",
                            {{"ref", {{"type", "ref/prompt"}, {"name", "greet"}}},
                             {"argument", {{"name", "a"}, {"value", "d"}}}}),
    };
    auto written = run_session(*server, requests, requests.size());

    ASSERT_EQ(written.size(), requests.size());
    std::set<int> answered;
    for (const auto& response : written) {
        EXPECT_TRUE(response.contains("result")) << response;
        answered.insert(response["id"].get<int>());
    }
    EXPECT_EQ(answered.size(), requests.size());
    EXPECT_FALSE(server->is_initialized());
}

TEST_F(ServerModernEraTest, HandlersReadTheRequestsPerRequestFields) {
    auto server = make_server();
    auto meta = make_modern_meta();
    meta["io.modelcontextprotocol/clientCapabilities"] = {{"roots", json::object()}};
    meta["traceparent"] = "00-0af7651916cd43dd8448eb211c80319c-00f067aa0ba902b7-01";
    meta["tracestate"] = "vendor=value";
    meta["baggage"] = "user=alice";
    auto response = dispatch(
        *server,
        modern_with_meta(1, "tools/call", meta, {{"name", "whoami"}, {"arguments", json::object()}}));

    ASSERT_TRUE(response.contains("result")) << response;
    const auto& seen = response["result"]["structuredContent"];
    ASSERT_TRUE(seen.value("modern", false)) << seen;
    EXPECT_EQ(seen["version"], "2026-07-28");
    EXPECT_EQ(seen["client"], "test-client");
    EXPECT_TRUE(seen["roots"].get<bool>());
    EXPECT_EQ(seen["meta"]["traceparent"], meta["traceparent"]);
    EXPECT_EQ(seen["meta"]["tracestate"], "vendor=value");
    EXPECT_EQ(seen["meta"]["baggage"], "user=alice");

    auto resource =
        dispatch(*server, make_modern_request(2, "resources/read", {{"uri", "test://doc"}}));
    EXPECT_EQ(resource["result"]["contents"][0]["text"], "modern") << resource;
    auto prompt = dispatch(*server, make_modern_request(3, "prompts/get", {{"name", "greet"}}));
    EXPECT_EQ(prompt["result"]["messages"][0]["content"]["text"], "modern") << prompt;
}

TEST_F(ServerModernEraTest, LegacyHandlersSeeNoPerRequestFields) {
    auto server = make_server();
    auto written = run_session(*server,
                               {make_initialize_request("i"), make_initialized_notification(),
                                legacy_call_tool("c", "whoami")},
                               2);

    ASSERT_EQ(written.size(), 2u);
    ASSERT_TRUE(written[1].contains("result")) << written[1];
    EXPECT_FALSE(written[1]["result"]["structuredContent"]["modern"].get<bool>());
}

TEST_F(ServerModernEraTest, MissingClientCapabilitiesIsInvalidParams) {
    auto server = make_server();
    auto meta = make_modern_meta();
    meta.erase("io.modelcontextprotocol/clientCapabilities");
    expect_error(dispatch(*server, modern_with_meta(1, "tools/list", meta)), mcp::g_INVALID_PARAMS);

    meta["io.modelcontextprotocol/clientCapabilities"] = "none";
    expect_error(dispatch(*server, modern_with_meta(2, "tools/list", meta)), mcp::g_INVALID_PARAMS);

    EXPECT_EQ(server->protocol_rejection_stats().invalid_metadata, 2u);
}

TEST_F(ServerModernEraTest, MalformedOptionalFieldsAreInvalidParams) {
    auto server = make_server();
    auto bad_info = make_modern_meta();
    bad_info["io.modelcontextprotocol/clientInfo"] = {{"name", "no-version"}};
    expect_error(dispatch(*server, modern_with_meta(1, "tools/list", bad_info)), mcp::g_INVALID_PARAMS);

    auto bad_level = make_modern_meta();
    bad_level["io.modelcontextprotocol/logLevel"] = "loud";
    expect_error(dispatch(*server, modern_with_meta(2, "tools/list", bad_level)),
                 mcp::g_INVALID_PARAMS);

    auto no_info = make_modern_meta();
    no_info.erase("io.modelcontextprotocol/clientInfo");
    EXPECT_TRUE(dispatch(*server, modern_with_meta(3, "tools/list", no_info)).contains("result"));
}

TEST_F(ServerModernEraTest, NonStringProtocolVersionIsInvalidParams) {
    auto server = make_server();
    auto meta = make_modern_meta();
    meta["io.modelcontextprotocol/protocolVersion"] = 20260728;
    expect_error(dispatch(*server, modern_with_meta(1, "tools/list", meta)), mcp::g_INVALID_PARAMS);
}

TEST_F(ServerModernEraTest, UnservedVersionIsUnsupportedWithExactData) {
    auto server = make_server();
    auto response =
        dispatch(*server, modern_with_meta(7, "tools/list", make_modern_meta("1900-01-01")));

    ASSERT_TRUE(response.contains("error")) << response;
    EXPECT_EQ(response["id"], 7);
    EXPECT_EQ(response["error"]["code"], mcp::g_UNSUPPORTED_PROTOCOL_VERSION);
    EXPECT_EQ(response["error"]["data"],
              (json{{"supported", g_all_versions}, {"requested", "1900-01-01"}}));
    EXPECT_EQ(server->protocol_rejection_stats().unsupported_version, 1u);
}

// A legacy version names a revision served only through initialize.
TEST_F(ServerModernEraTest, LegacyVersionInMetaIsUnsupported) {
    auto server = make_server();
    auto response =
        dispatch(*server, modern_with_meta(1, "tools/list", make_modern_meta("2025-11-25")));

    expect_error(response, mcp::g_UNSUPPORTED_PROTOCOL_VERSION);
    EXPECT_EQ(response["error"]["data"]["supported"], g_all_versions);
    EXPECT_EQ(response["error"]["data"]["requested"], "2025-11-25");
}

// The version check runs first: a version the server does not serve may define the method or
// the fields differently.
TEST_F(ServerModernEraTest, UnsupportedVersionIsReportedBeforeMethodAndFields) {
    auto server = make_server();
    expect_error(dispatch(*server, modern_with_meta(
                                       1, "no/such/method",
                                       {{"io.modelcontextprotocol/protocolVersion", "2099-01-01"}})),
                 mcp::g_UNSUPPORTED_PROTOCOL_VERSION);
}

TEST_F(ServerModernEraTest, RequestWithoutMetaBeforeInitializeIsStillInvalidRequest) {
    auto server = make_server();
    auto written = run_session(
        *server,
        {{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}, {"params", json::object()}}}, 1);

    ASSERT_EQ(written.size(), 1u);
    expect_error(written[0], mcp::g_INVALID_REQUEST);
    EXPECT_EQ(server->protocol_rejection_stats().legacy_shaped, 0u);
}

TEST_F(ServerModernEraTest, MethodsRemovedIn2026AreNotFoundButServedToLegacySessions) {
    auto server = make_server();
    for (const auto* method :
         {"ping", "logging/setLevel", "resources/subscribe", "resources/unsubscribe", "shutdown"}) {
        expect_error(dispatch(*server, make_modern_request(1, method,
                                                           {{"uri", "test://doc"}, {"level", "info"}})),
                     mcp::g_METHOD_NOT_FOUND);
    }

    auto written = run_session(*server,
                               {make_initialize_request("i"),
                                make_initialized_notification(),
                                {{"jsonrpc", "2.0"}, {"id", "p"}, {"method", "ping"}},
                                {{"jsonrpc", "2.0"},
                                 {"id", "s"},
                                 {"method", "resources/subscribe"},
                                 {"params", {{"uri", "test://doc"}}}}},
                               3);
    ASSERT_EQ(written.size(), 3u);
    EXPECT_TRUE(written[1].contains("result")) << written[1];
    EXPECT_TRUE(written[2].contains("result")) << written[2];
}

TEST_F(ServerModernEraTest, UnknownMethodIsNotFound) {
    auto server = make_server();
    expect_error(dispatch(*server, make_modern_request(1, "no/such/method")), mcp::g_METHOD_NOT_FOUND);
}

TEST_F(ServerModernEraTest, InitializeIsAlwaysLegacy) {
    auto server = make_server();
    auto initialize = make_initialize_request("i");
    initialize["params"]["_meta"] = make_modern_meta();
    auto written = run_session(*server, {initialize}, 1);

    ASSERT_EQ(written.size(), 1u);
    ASSERT_TRUE(written[0].contains("result")) << written[0];
    EXPECT_EQ(written[0]["result"]["protocolVersion"], "2025-11-25");
    EXPECT_TRUE(server->is_initialized());
}

TEST_F(ServerModernEraTest, ModernRequestsLeaveTheLegacyLifecycleAlone) {
    auto server = make_server();
    auto written = run_session(*server,
                               {make_modern_request(1, "tools/list"),
                                make_initialize_request("i"),
                                make_initialized_notification(),
                                {{"jsonrpc", "2.0"}, {"id", "l"}, {"method", "tools/list"}}},
                               3);

    ASSERT_EQ(written.size(), 3u);
    EXPECT_TRUE(written[0].contains("result")) << written[0];
    EXPECT_TRUE(written[1].contains("result")) << written[1];
    EXPECT_TRUE(written[2].contains("result")) << written[2];
    EXPECT_TRUE(server->is_initialized());
}

TEST_F(ServerModernEraTest, ModernRequestsLeaveTheSessionLogLevelAlone) {
    auto server = make_server();
    auto set_level = make_modern_request(1, "logging/setLevel", {{"level", "emergency"}});
    auto meta = make_modern_meta();
    meta["io.modelcontextprotocol/logLevel"] = "emergency";
    auto written = run_session(*server, {set_level, modern_with_meta(2, "tools/list", meta)}, 2);

    ASSERT_EQ(written.size(), 2u);
    expect_error(written[0], mcp::g_METHOD_NOT_FOUND);
    EXPECT_EQ(server->get_log_level(), mcp::LoggingLevel::eDebug);
}

// A 2026-07-28 request is logged only at the level it asks for, never at the session's level,
// which only a legacy client can set.
TEST_F(ServerModernEraTest, LoggingFollowsTheRequestsOwnLevel) {
    auto server = make_server();
    auto without_level = run_session(*server, {call_tool(1, "log")}, 1);
    ASSERT_EQ(without_level.size(), 1u);
    EXPECT_EQ(without_level[0]["id"], 1) << without_level[0];

    auto meta = make_modern_meta();
    meta["io.modelcontextprotocol/logLevel"] = "warning";
    auto with_level = run_session(
        *server,
        {modern_with_meta(2, "tools/call", meta, {{"name", "log"}, {"arguments", json::object()}})}, 2);
    ASSERT_EQ(with_level.size(), 2u);
    EXPECT_EQ(with_level[0]["method"], "notifications/message") << with_level[0];
    EXPECT_EQ(with_level[0]["params"]["level"], "error");
    EXPECT_EQ(with_level[1]["id"], 2) << with_level[1];
}

// A 2026-07-28 server sends no requests of its own; a handler's question needs the client to have
// declared the capability it uses, here sampling.
TEST_F(ServerModernEraTest, ServerToClientRequestsNeedTheClientsCapability) {
    auto server = make_server();
    auto response = dispatch(*server, call_tool(1, "sample"));

    expect_error(response, mcp::g_MISSING_REQUIRED_CLIENT_CAPABILITY);
    EXPECT_EQ(response["error"]["data"]["requiredCapabilities"], (json{{"sampling", json::object()}}))
        << response;
}

TEST_F(ServerModernEraTest, ModernMethodListMatchesTheSchemasClientRequests) {
    // ClientRequest in the 2026-07-28 schema.
    const std::set<std::string_view> expected = {
        "server/discover",     "resources/list", "resources/templates/list",
        "resources/read",      "prompts/list",   "prompts/get",
        "tools/list",          "tools/call",     "completion/complete",
        "subscriptions/listen"};
    const std::set<std::string_view> served(mcp::detail::g_MODERN_METHODS.begin(),
                                            mcp::detail::g_MODERN_METHODS.end());
    EXPECT_EQ(served, expected);
}

TEST_F(ServerModernEraTest, UnknownToolAndResourceAreInvalidParams) {
    auto server = make_server();
    expect_error(dispatch(*server, call_tool(1, "missing")), mcp::g_INVALID_PARAMS);
    expect_error(
        dispatch(*server, make_modern_request(2, "resources/read", {{"uri", "test://missing"}})),
        mcp::g_INVALID_PARAMS);
    expect_error(dispatch(*server, make_modern_request(3, "prompts/get", {{"name", "missing"}})),
                 mcp::g_INVALID_PARAMS);
    EXPECT_EQ(server->protocol_rejection_stats().invalid_metadata, 0u);
}

TEST_F(ServerModernEraTest, DiscoverListsEveryServedVersion) {
    auto server = make_server();
    auto response = dispatch(*server, make_discover_request("d"));

    ASSERT_TRUE(response.contains("result")) << response;
    EXPECT_EQ(response["result"]["supportedVersions"], g_all_versions);
}

TEST_F(ServerModernEraTest, PolicyWithoutModernRefusesEveryModernRequest) {
    auto server = make_server();
    mcp::ServerProtocolPolicy policy;
    policy.serve_modern = false;
    server->set_protocol_policy(policy);

    auto response = dispatch(*server, make_discover_request(1));
    expect_error(response, mcp::g_UNSUPPORTED_PROTOCOL_VERSION);
    EXPECT_EQ(response["error"]["data"]["supported"],
              (json{"2024-11-05", "2025-03-26", "2025-06-18", "2025-11-25"}));
    EXPECT_EQ(response["error"]["data"]["requested"], "2026-07-28");

    auto written = run_session(*server, {make_initialize_request("i")}, 1);
    ASSERT_EQ(written.size(), 1u);
    EXPECT_EQ(written[0]["result"]["protocolVersion"], "2025-11-25") << written[0];
}

TEST_F(ServerModernEraTest, PolicyWithoutLegacyRefusesInitializeNamingTheServedVersions) {
    auto server = make_server();
    mcp::ServerProtocolPolicy policy;
    policy.serve_legacy = false;
    server->set_protocol_policy(policy);

    auto written = run_session(*server,
                               {make_initialize_request("i"),
                                {{"jsonrpc", "2.0"}, {"id", "l"}, {"method", "tools/list"}},
                                make_modern_request("m", "tools/list")},
                               3);

    ASSERT_EQ(written.size(), 3u);
    expect_error(written[0], mcp::g_METHOD_NOT_FOUND);
    EXPECT_EQ(written[0]["error"]["data"], (json{{"supported", {"2026-07-28"}}}));
    EXPECT_NE(written[0]["error"]["message"].get<std::string>().find("2026-07-28"), std::string::npos)
        << written[0];
    expect_error(written[1], mcp::g_INVALID_PARAMS);
    EXPECT_TRUE(written[2].contains("result")) << written[2];
    EXPECT_FALSE(server->is_initialized());
    EXPECT_EQ(server->protocol_rejection_stats().legacy_shaped, 2u);

    auto discover = dispatch(*server, make_discover_request("d"));
    EXPECT_EQ(discover["result"]["supportedVersions"], (json{"2026-07-28"}));
}

TEST_F(ServerModernEraTest, PolicyLegacyVersionsRestrictNegotiation) {
    for (const auto& [requested, negotiated] :
         std::vector<std::pair<std::string, std::string>>{{"2024-11-05", "2025-06-18"},
                                                          {"2025-03-26", "2025-03-26"},
                                                          {"2025-06-18", "2025-06-18"},
                                                          {"2025-11-25", "2025-06-18"},
                                                          {"2026-07-28", "2025-06-18"}}) {
        auto server = make_server();
        mcp::ServerProtocolPolicy policy;
        policy.legacy_versions = std::vector<std::string>{"2025-03-26", "2025-06-18"};
        server->set_protocol_policy(policy);

        auto initialize = make_initialize_request("i");
        initialize["params"]["protocolVersion"] = requested;
        auto written = run_session(*server, {initialize}, 1);
        ASSERT_EQ(written.size(), 1u);
        EXPECT_EQ(written[0]["result"]["protocolVersion"], negotiated) << requested;

        auto discover = dispatch(*server, make_discover_request("d"));
        EXPECT_EQ(discover["result"]["supportedVersions"],
                  (json{"2025-03-26", "2025-06-18", "2026-07-28"}));
    }
}

TEST_F(ServerModernEraTest, PolicyIsValidatedWhenSet) {
    auto server = make_server();
    mcp::ServerProtocolPolicy neither;
    neither.serve_modern = false;
    neither.serve_legacy = false;
    EXPECT_THROW(server->set_protocol_policy(neither), std::invalid_argument);

    mcp::ServerProtocolPolicy unknown;
    unknown.legacy_versions = std::vector<std::string>{"2025-11-26"};
    EXPECT_THROW(server->set_protocol_policy(unknown), std::invalid_argument);

    mcp::ServerProtocolPolicy modern_as_legacy;
    modern_as_legacy.legacy_versions = std::vector<std::string>{"2026-07-28"};
    EXPECT_THROW(server->set_protocol_policy(modern_as_legacy), std::invalid_argument);

    mcp::ServerProtocolPolicy empty;
    empty.legacy_versions = std::vector<std::string>{};
    EXPECT_THROW(server->set_protocol_policy(empty), std::invalid_argument);
}

TEST_F(ServerModernEraTest, PolicyCannotChangeOnceServing) {
    auto server = make_server();
    dispatch(*server, make_discover_request(1));
    EXPECT_THROW(server->set_protocol_policy(mcp::ServerProtocolPolicy{}), std::logic_error);
}

TEST_F(ServerModernEraTest, LegacyTrafficIsNeverCountedAsARejection) {
    auto server = make_server();
    auto written = run_session(*server,
                               {make_initialize_request("i"),
                                make_initialized_notification(),
                                {{"jsonrpc", "2.0"}, {"id", "l"}, {"method", "tools/list"}},
                                legacy_call_tool("c", "missing"),
                                {{"jsonrpc", "2.0"}, {"id", "p"}, {"method", "ping"}}},
                               4);

    ASSERT_EQ(written.size(), 4u);
    const auto stats = server->protocol_rejection_stats();
    EXPECT_EQ(stats.invalid_metadata, 0u);
    EXPECT_EQ(stats.unsupported_version, 0u);
    EXPECT_EQ(stats.legacy_shaped, 0u);
}

static json cancel_notification(const json& id) {
    return {{"jsonrpc", "2.0"}, {"method", "notifications/cancelled"}, {"params", {{"requestId", id}}}};
}

// The server sends nothing more for a cancelled 2026-07-28 request: neither the log message its
// handler sends after seeing the cancellation nor its response. Request 6 is answered only after
// that handler has returned, so its answer is the first thing written.
TEST_F(ServerModernEraTest, NotificationsCancelledSilencesAModernToolCall) {
    auto server = make_server();
    auto finished = std::make_shared<std::atomic<bool>>(false);
    auto saw_cancel = std::make_shared<std::atomic<bool>>(false);
    server->add_tool<json, json>(
        "log_after_cancel", "Logs once cancelled", json{{"type", "object"}},
        [finished, saw_cancel](mcp::Context& ctx, json /*args*/) -> mcp::Task<json> {
            const bool cancelled = co_await wait_for_cancel(ctx);
            saw_cancel->store(cancelled);
            co_await ctx.log(mcp::LoggingLevel::eError, "after cancel");
            finished->store(true);
            co_return json{{"cancelled", true}};
        });
    server->add_tool<json, json>("after_finished", "Returns once log_after_cancel has",
                                 json{{"type", "object"}},
                                 [finished](mcp::Context& /*ctx*/, json /*args*/) -> mcp::Task<json> {
                                     const bool done = co_await wait_for_flag(*finished);
                                     co_return json{{"finished", done}};
                                 });

    auto meta = make_modern_meta();
    meta["io.modelcontextprotocol/logLevel"] = "debug";
    auto written =
        run_session(*server,
                    {modern_with_meta(5, "tools/call", meta,
                                      {{"name", "log_after_cancel"}, {"arguments", json::object()}}),
                     cancel_notification(5), call_tool(6, "after_finished")},
                    1);

    ASSERT_EQ(written.size(), 1u);
    EXPECT_EQ(written[0]["id"], 6) << written[0];
    EXPECT_TRUE(written[0]["result"]["structuredContent"]["finished"].get<bool>()) << written[0];
    EXPECT_TRUE(saw_cancel->load());
}

// Cancellation reaches every request read from the session, not only tools/call.
TEST_F(ServerModernEraTest, NotificationsCancelledCancelsAModernPromptGet) {
    auto server = make_server();
    auto saw_cancel = std::make_shared<std::atomic<bool>>(false);
    auto finished = std::make_shared<std::atomic<bool>>(false);
    mcp::Prompt prompt;
    prompt.name = "wait";
    server->add_prompt<json, json>(
        prompt, [saw_cancel, finished](mcp::Context& ctx, json /*params*/) -> mcp::Task<json> {
            const bool cancelled = co_await wait_for_cancel(ctx);
            saw_cancel->store(cancelled);
            finished->store(true);
            co_return json{{"messages", json::array()}};
        });
    server->add_tool<json, json>("after_finished", "Returns once the prompt handler has",
                                 json{{"type", "object"}},
                                 [finished](mcp::Context& /*ctx*/, json /*args*/) -> mcp::Task<json> {
                                     const bool done = co_await wait_for_flag(*finished);
                                     co_return json{{"finished", done}};
                                 });

    auto written = run_session(*server,
                               {make_modern_request(7, "prompts/get", {{"name", "wait"}}),
                                cancel_notification(7), call_tool(8, "after_finished")},
                               1);

    ASSERT_EQ(written.size(), 1u);
    EXPECT_EQ(written[0]["id"], 8) << written[0];
    EXPECT_TRUE(saw_cancel->load());
}

// A legacy request's handler sees the cancellation too, and its response is still sent.
TEST_F(ServerModernEraTest, NotificationsCancelledCancelsALegacyPromptGet) {
    auto server = make_server();
    mcp::Prompt prompt;
    prompt.name = "wait";
    server->add_prompt<json, json>(prompt, [](mcp::Context& ctx, json /*params*/) -> mcp::Task<json> {
        const bool cancelled = co_await wait_for_cancel(ctx);
        co_return json{
            {"messages",
             json::array(
                 {{{"role", "user"},
                   {"content", {{"type", "text"}, {"text", cancelled ? "cancelled" : "not"}}}}})}};
    });

    auto written = run_session(
        *server,
        {make_initialize_request("i"),
         make_initialized_notification(),
         {{"jsonrpc", "2.0"}, {"id", "p"}, {"method", "prompts/get"}, {"params", {{"name", "wait"}}}},
         cancel_notification("p")},
        2);

    ASSERT_EQ(written.size(), 2u);
    EXPECT_EQ(written[1]["id"], "p");
    ASSERT_TRUE(written[1].contains("result")) << written[1];
    EXPECT_EQ(written[1]["result"]["messages"][0]["content"]["text"], "cancelled");
}

// A 2026-07-28 request given to dispatch_request_direct() did not come from the session, so its
// log messages must not reach the session's client. The direct call starts while session request
// 1 runs, and request 1 answers only once the direct call has returned.
TEST_F(ServerModernEraTest, DirectModernRequestSendsNothingThroughTheSession) {
    auto server = make_server();
    auto* raw_server = server.get();
    json direct;
    server->add_tool<json, json>(
        "run_direct", "Dispatches a logging request directly", json{{"type", "object"}},
        [this, raw_server, &direct](mcp::Context& /*ctx*/, json /*args*/) -> mcp::Task<json> {
            auto done = std::make_shared<std::atomic<bool>>(false);
            auto meta = make_modern_meta();
            meta["io.modelcontextprotocol/logLevel"] = "debug";
            boost::asio::co_spawn(
                io_ctx_,
                raw_server->dispatch_request_direct(modern_with_meta(
                    "d", "tools/call", meta, {{"name", "log"}, {"arguments", json::object()}})),
                [&direct, done](std::exception_ptr error, std::string wire) {
                    EXPECT_EQ(error, nullptr);
                    direct = json::parse(wire);
                    done->store(true);
                });
            const bool finished = co_await wait_for_flag(*done);
            co_return json{{"done", finished}};
        });

    auto written = run_session(*server, {call_tool(1, "run_direct")}, 1);

    ASSERT_EQ(written.size(), 1u);
    EXPECT_EQ(written[0]["id"], 1) << written[0];
    EXPECT_TRUE(written[0]["result"]["structuredContent"]["done"].get<bool>());
    ASSERT_TRUE(direct.contains("result")) << direct;
    EXPECT_TRUE(direct["result"]["structuredContent"]["logged"].get<bool>());
}

// Direct callers cancel through their own flags, so two calls that both use id 1 stay apart.
TEST_F(ServerModernEraTest, DirectCallsSharingAnIdAreCancelledIndependently) {
    auto server = make_server();
    auto first_flag = std::make_shared<std::atomic<bool>>(false);
    auto second_flag = std::make_shared<std::atomic<bool>>(false);

    json first;
    json second;
    boost::asio::co_spawn(io_ctx_,
                          server->dispatch_request_direct(call_tool(1, "wait_for_cancel"),
                                                          mcp::RequestContext{first_flag}),
                          [&first](std::exception_ptr error, std::string wire) {
                              EXPECT_EQ(error, nullptr);
                              first = json::parse(wire);
                          });
    boost::asio::co_spawn(
        io_ctx_,
        server->dispatch_request_direct(call_tool(1, "whoami"), mcp::RequestContext{second_flag}),
        [&second, first_flag](std::exception_ptr error, std::string wire) {
            EXPECT_EQ(error, nullptr);
            second = json::parse(wire);
            // Only now release the first call: it has been waiting all along.
            first_flag->store(true);
        });
    io_ctx_.run();

    ASSERT_TRUE(first.contains("result")) << first;
    EXPECT_TRUE(first["result"]["structuredContent"]["cancelled"].get<bool>());
    ASSERT_TRUE(second.contains("result")) << second;
    EXPECT_TRUE(second["result"]["structuredContent"]["modern"].get<bool>());
    EXPECT_FALSE(second_flag->load());
}

TEST_F(ServerModernEraTest, WebSocketServesAModernRequestWithoutInitialize) {
    auto server = make_server();
    boost::asio::ip::tcp::acceptor acceptor(io_ctx_, {boost::asio::ip::tcp::v4(), 0});
    const auto port = acceptor.local_endpoint().port();

    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            auto socket = co_await acceptor.async_accept(boost::asio::use_awaitable);
            auto transport = std::make_shared<mcp::WebSocketServerTransport>(std::move(socket));
            co_await server->run(transport, io_ctx_.get_executor());
        },
        boost::asio::detached);

    json response;
    boost::asio::co_spawn(
        io_ctx_,
        [&]() -> mcp::Task<void> {
            mcp::WebSocketClientTransport transport(io_ctx_.get_executor(), "127.0.0.1",
                                                    std::to_string(port));
            co_await transport.write_message(call_tool(1, "whoami").dump());
            response = json::parse(co_await transport.read_message());
            transport.close();
        },
        boost::asio::detached);
    io_ctx_.run();

    ASSERT_TRUE(response.contains("result")) << response;
    EXPECT_EQ(response["id"], 1);
    EXPECT_TRUE(response["result"]["structuredContent"]["modern"].get<bool>());
    EXPECT_FALSE(server->is_initialized());
}

TEST_F(ServerModernEraTest, ConcurrentModernRequestsOnAThreadPool) {
    auto server = make_server();
    boost::asio::thread_pool pool(4);
    std::vector<std::future<std::string>> responses;
    for (int index = 0; index < 64; ++index) {
        auto request =
            index % 2 == 0 ? call_tool(index, "whoami") : make_modern_request(index, "tools/list");
        responses.push_back(boost::asio::co_spawn(
            pool, server->dispatch_request_direct(std::move(request)), boost::asio::use_future));
    }
    for (int index = 0; index < 64; ++index) {
        auto response = json::parse(responses[static_cast<std::size_t>(index)].get());
        EXPECT_EQ(response["id"], index);
        EXPECT_TRUE(response.contains("result")) << response;
    }
    pool.join();
}

TEST_F(ServerModernEraTest, EveryModernResultIsCompleteAndNamesTheServer) {
    auto server = make_server();
    std::vector<json> requests = {
        make_discover_request(1),
        make_modern_request(2, "tools/list"),
        call_tool(3, "whoami"),
        make_modern_request(4, "resources/list"),
        make_modern_request(5, "resources/read", {{"uri", "test://doc"}}),
        make_modern_request(6, "resources/templates/list"),
        make_modern_request(7, "prompts/list"),
        make_modern_request(8, "prompts/get", {{"name", "greet"}}),
        make_modern_request(9, "completion/complete",
                            {{"ref", {{"type", "ref/prompt"}, {"name", "greet"}}},
                             {"argument", {{"name", "a"}, {"value", "d"}}}}),
    };
    for (const auto& request : requests) {
        auto response = dispatch(*server, request);
        ASSERT_TRUE(response.contains("result")) << response;
        const auto& result = response["result"];
        EXPECT_EQ(result.value("resultType", ""), "complete") << request["method"];
        EXPECT_EQ(result["_meta"]["io.modelcontextprotocol/serverInfo"],
                  (json{{"name", "modern-server"}, {"version", "1.0"}}))
            << request["method"];
    }
}

TEST_F(ServerModernEraTest, CacheableResultsCarryTheDefaultHints) {
    auto server = make_server();
    const std::set<std::string> cacheable = {"server/discover",          "tools/list",
                                             "resources/list",           "resources/read",
                                             "resources/templates/list", "prompts/list"};
    std::vector<json> requests = {
        make_discover_request(1),
        make_modern_request(2, "tools/list"),
        call_tool(3, "whoami"),
        make_modern_request(4, "resources/list"),
        make_modern_request(5, "resources/read", {{"uri", "test://doc"}}),
        make_modern_request(6, "resources/templates/list"),
        make_modern_request(7, "prompts/list"),
        make_modern_request(8, "prompts/get", {{"name", "greet"}}),
    };
    for (const auto& request : requests) {
        auto response = dispatch(*server, request);
        ASSERT_TRUE(response.contains("result")) << response;
        const auto& result = response["result"];
        const auto method = request["method"].get<std::string>();
        if (cacheable.contains(method)) {
            EXPECT_EQ(result.value("ttlMs", -1), 0) << method;
            EXPECT_EQ(result.value("cacheScope", ""), "private") << method;
        } else {
            EXPECT_FALSE(result.contains("ttlMs")) << method;
            EXPECT_FALSE(result.contains("cacheScope")) << method;
        }
    }
}

TEST_F(ServerModernEraTest, CacheHintsApplyPerTarget) {
    auto server = make_server();
    server->set_cache_hints(mcp::CacheTarget::eDiscover, {1, mcp::CacheScope::ePublic});
    server->set_cache_hints(mcp::CacheTarget::eToolsList, {2, mcp::CacheScope::ePublic});
    server->set_cache_hints(mcp::CacheTarget::ePromptsList, {3, mcp::CacheScope::ePrivate});
    server->set_cache_hints(mcp::CacheTarget::eResourcesList, {4, mcp::CacheScope::ePublic});
    server->set_cache_hints(mcp::CacheTarget::eResourceTemplatesList, {5, mcp::CacheScope::ePrivate});
    server->set_cache_hints(mcp::CacheTarget::eResourcesRead, {6, mcp::CacheScope::ePublic});

    const std::vector<std::tuple<json, int, std::string>> expected = {
        {make_discover_request(1), 1, "public"},
        {make_modern_request(2, "tools/list"), 2, "public"},
        {make_modern_request(3, "prompts/list"), 3, "private"},
        {make_modern_request(4, "resources/list"), 4, "public"},
        {make_modern_request(5, "resources/templates/list"), 5, "private"},
        {make_modern_request(6, "resources/read", {{"uri", "test://doc"}}), 6, "public"},
    };
    for (const auto& [request, ttl, scope] : expected) {
        auto response = dispatch(*server, request);
        ASSERT_TRUE(response.contains("result")) << response;
        EXPECT_EQ(response["result"]["ttlMs"], ttl) << request["method"];
        EXPECT_EQ(response["result"]["cacheScope"], scope) << request["method"];
    }
}

// "Servers MUST apply the same cacheScope to all response pages for a given list request."
TEST_F(ServerModernEraTest, EveryPageOfAListCarriesTheSameHints) {
    auto server = make_server();
    server->set_page_size(1);
    server->set_cache_hints(mcp::CacheTarget::eToolsList, {7, mcp::CacheScope::ePrivate});
    json params = json::object();
    int pages = 0;
    for (bool more = true; more;) {
        ASSERT_LT(pages, 10);
        auto response = dispatch(*server, make_modern_request(++pages, "tools/list", params));
        ASSERT_TRUE(response.contains("result")) << response;
        EXPECT_EQ(response["result"]["ttlMs"], 7) << response;
        EXPECT_EQ(response["result"]["cacheScope"], "private") << response;
        more = response["result"].contains("nextCursor");
        if (more) {
            params["cursor"] = response["result"]["nextCursor"];
        }
    }
    EXPECT_GT(pages, 1);
}

TEST_F(ServerModernEraTest, HandlerSuppliedResultMembersAreKept) {
    mcp::ServerCapabilities caps;
    caps.resources = mcp::ServerCapabilities::ResourcesCapability{};
    mcp::Server server({"modern-server", "1.0"}, caps);
    server.set_cache_hints(mcp::CacheTarget::eResourcesRead, {60000, mcp::CacheScope::ePublic});
    mcp::Resource resource;
    resource.uri = "test://own";
    resource.name = "own";
    server.add_resource<json, json>(resource, [](mcp::Context&, json) -> mcp::Task<json> {
        co_return json{{"contents", json::array({{{"uri", "test://own"}, {"text", "x"}}})},
                       {"ttlMs", 5},
                       {"cacheScope", "private"},
                       {"_meta", {{"example.com/trace", "abc"}}}};
    });

    auto response = dispatch(server, make_modern_request(1, "resources/read", {{"uri", "test://own"}}));
    ASSERT_TRUE(response.contains("result")) << response;
    const auto& result = response["result"];
    EXPECT_EQ(result["ttlMs"], 5);
    EXPECT_EQ(result["cacheScope"], "private");
    EXPECT_EQ(result["_meta"]["example.com/trace"], "abc");
    EXPECT_EQ(result["_meta"]["io.modelcontextprotocol/serverInfo"]["name"], "modern-server");
    EXPECT_EQ(result["resultType"], "complete");
}

TEST_F(ServerModernEraTest, ServerInfoCanBeLeftOut) {
    auto server = make_server();
    server->set_report_server_info(false);
    for (const auto& request :
         {make_discover_request(1), make_modern_request(2, "tools/list"), call_tool(3, "whoami")}) {
        auto response = dispatch(*server, request);
        ASSERT_TRUE(response.contains("result")) << response;
        EXPECT_FALSE(response["result"].contains("_meta")) << response;
        EXPECT_EQ(response["result"]["resultType"], "complete");
    }
}

TEST_F(ServerModernEraTest, LegacyResultsGainNoModernMembers) {
    auto server = make_server();
    server->set_cache_hints(mcp::CacheTarget::eToolsList, {60000, mcp::CacheScope::ePublic});
    server->set_cache_hints(mcp::CacheTarget::eResourcesRead, {60000, mcp::CacheScope::ePublic});
    auto legacy = [](const char* id, const char* method, json params = json::object()) {
        return json{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", std::move(params)}};
    };
    auto written = run_session(
        *server,
        {make_initialize_request("i"), make_initialized_notification(), legacy("t", "tools/list"),
         legacy_call_tool("c", "whoami"), legacy("r", "resources/read", {{"uri", "test://doc"}}),
         legacy("l", "resources/list"), legacy("tl", "resources/templates/list"),
         legacy("p", "prompts/list"), legacy("g", "prompts/get", {{"name", "greet"}}),
         legacy("x", "completion/complete",
                {{"ref", {{"type", "ref/prompt"}, {"name", "greet"}}},
                 {"argument", {{"name", "a"}, {"value", "d"}}}})},
        9);

    ASSERT_EQ(written.size(), 9u);
    EXPECT_FALSE(written[0]["result"].contains("resultType")) << written[0];
    for (std::size_t index = 1; index < written.size(); ++index) {
        const auto& response = written[index];
        ASSERT_TRUE(response.contains("result")) << response;
        for (const auto* member : {"resultType", "ttlMs", "cacheScope", "_meta"}) {
            EXPECT_FALSE(response["result"].contains(member)) << member << " in " << response;
        }
    }
    EXPECT_EQ(written[1]["result"], (json{{"tools", written[1]["result"]["tools"]}}));
}

}  // namespace

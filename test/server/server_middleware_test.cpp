/**
 * @file server_middleware_test.cpp
 * @brief Tests for Server middleware functionality
 */

#include "../support/hang_guard.hpp"
#include "../test_utils.hpp"

#include <gtest/gtest.h>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <chrono>
#include <mcp/protocol/protocol.hpp>
#include <mcp/server/server.hpp>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace mcp;
using json = nlohmann::json;

// Test 1: Middleware modifies params before handler
TEST(ServerMiddlewareTest, MiddlewareModifiesParams) {
    boost::asio::io_context io;

    ServerCapabilities caps;
    ServerCapabilities::ToolsCapability tools_cap;
    caps.tools = std::move(tools_cap);

    Implementation info;
    info.name = "test-server";
    info.version = "1.0";

    Server server(std::move(info), std::move(caps));

    // Register middleware that adds 100 to the "addend" param
    server.use([](Context& ctx, const json& params, TypeErasedHandler next) -> Task<json> {
        json modified = params;
        if (modified.contains("arguments") && modified["arguments"].contains("addend")) {
            modified["arguments"]["addend"] = modified["arguments"]["addend"].get<int>() + 100;
        }
        co_return co_await next(ctx, modified);
    });

    // Register a simple add tool
    server.add_tool<json, json>(
        "add", "Adds a number to 10",
        json{{"type", "object"}, {"properties", {{"addend", {{"type", "integer"}}}}}},
        [](const json& args) -> json {
            int addend = args.value("addend", 0);
            int result = 10 + addend;
            return json{{"sum", result}};
        });

    auto transport = std::make_shared<ScriptedTransport>(io.get_executor());
    auto* transport_ptr = transport.get();

    std::vector<json> responses;
    transport_ptr->set_on_write([&responses, transport_ptr](std::string_view msg) {
        responses.push_back(json::parse(msg));
        if (responses.size() >= 2) {
            transport_ptr->close();
        }
    });

    transport_ptr->enqueue_message(make_initialize_request("1").dump());
    transport_ptr->enqueue_message(make_initialized_notification().dump());

    json call_req{{"jsonrpc", "2.0"},
                  {"id", "2"},
                  {"method", "tools/call"},
                  {"params", {{"name", "add"}, {"arguments", {{"addend", 5}}}}}};
    transport_ptr->enqueue_message(call_req.dump());

    boost::asio::co_spawn(io, server.run(transport, io.get_executor()), boost::asio::detached);
    io.run();

    // Check that the result reflects the modified param (10 + 105 = 115)
    ASSERT_GE(responses.size(), 2);
    auto it = std::find_if(responses.begin(), responses.end(), [](const json& msg) {
        return msg.contains("id") && msg["id"] == "2" && msg.contains("result");
    });
    ASSERT_NE(it, responses.end());
    EXPECT_EQ((*it)["result"]["structuredContent"]["sum"], 115);
}

// Test 2: Middleware short-circuits (returns without calling next)
TEST(ServerMiddlewareTest, MiddlewareShortCircuits) {
    boost::asio::io_context io;

    ServerCapabilities caps;
    ServerCapabilities::ToolsCapability tools_cap;
    caps.tools = std::move(tools_cap);

    Implementation info;
    info.name = "test-server";
    info.version = "1.0";

    Server server(std::move(info), std::move(caps));

    // Middleware that blocks "forbidden" tool without calling next
    server.use([](Context& ctx, const json& params, TypeErasedHandler next) -> Task<json> {
        if (params.contains("name") && params["name"] == "forbidden") {
            throw std::runtime_error("Tool 'forbidden' is not allowed");
        }
        co_return co_await next(ctx, params);
    });

    // Register the forbidden tool (should never execute)
    bool tool_executed = false;
    server.add_tool<json, json>("forbidden", "A forbidden tool", json{{"type", "object"}},
                                [&tool_executed](const json&) -> json {
                                    tool_executed = true;
                                    return json{{"result", "should not see this"}};
                                });

    auto transport = std::make_shared<ScriptedTransport>(io.get_executor());
    auto* transport_ptr = transport.get();

    std::vector<json> responses;
    transport_ptr->set_on_write([&responses, transport_ptr](std::string_view msg) {
        responses.push_back(json::parse(msg));
        if (responses.size() >= 2) {
            transport_ptr->close();
        }
    });

    transport_ptr->enqueue_message(make_initialize_request("1").dump());
    transport_ptr->enqueue_message(make_initialized_notification().dump());

    json call_req{{"jsonrpc", "2.0"},
                  {"id", "2"},
                  {"method", "tools/call"},
                  {"params", {{"name", "forbidden"}, {"arguments", nlohmann::json::object()}}}};
    transport_ptr->enqueue_message(call_req.dump());

    boost::asio::co_spawn(io, server.run(transport, io.get_executor()), boost::asio::detached);
    io.run();

    // Verify the tool was never executed
    EXPECT_FALSE(tool_executed);

    // A middleware that throws is signalling that the call must not proceed, which belongs on the
    // protocol channel rather than in a successful tool result.
    auto it = std::find_if(responses.begin(), responses.end(), [](const json& msg) {
        return msg.contains("id") && msg["id"] == "2" && msg.contains("error");
    });
    ASSERT_NE(it, responses.end());
    EXPECT_EQ((*it)["error"]["code"], mcp::g_INTERNAL_ERROR);
    EXPECT_TRUE((*it)["error"]["message"].get<std::string>().find("forbidden") != std::string::npos);
}

// Test 3: Multiple middlewares execute in order (1→2→handler→2→1)
// Each of these reaches an application handler, so a middleware guarding the server guards it too.
TEST(ServerMiddlewareTest, MiddlewareGuardsSubscriptionsAndCompletion) {
    boost::asio::io_context io;

    ServerCapabilities caps;
    ServerCapabilities::ResourcesCapability resources_cap;
    resources_cap.subscribe = true;
    caps.resources = std::move(resources_cap);
    caps.completions = json::object();

    Implementation info;
    info.name = "test-server";
    info.version = "1.0";

    Server server(std::move(info), std::move(caps));
    server.use([](Context& ctx, const json& params, TypeErasedHandler next) -> Task<json> {
        if (params.contains("refuse")) {
            throw std::runtime_error("refused by middleware");
        }
        co_return co_await next(ctx, params);
    });

    int handler_calls = 0;
    server.on_subscribe([&handler_calls](const std::string&) { ++handler_calls; });
    server.on_unsubscribe([&handler_calls](const std::string&) { ++handler_calls; });
    server.set_completion_provider(
        [&handler_calls](const mcp::CompleteParams&) -> Task<mcp::CompleteResult> {
            ++handler_calls;
            co_return mcp::CompleteResult{};
        });

    auto transport = std::make_shared<ScriptedTransport>(io.get_executor());
    auto* transport_ptr = transport.get();
    std::vector<json> responses;
    transport_ptr->set_on_write([&responses, transport_ptr](std::string_view msg) {
        responses.push_back(json::parse(msg));
        if (responses.size() >= 7) {
            transport_ptr->close();
        }
    });
    transport_ptr->enqueue_message(make_initialize_request("0").dump());
    transport_ptr->enqueue_message(make_initialized_notification().dump());
    const json completion = {{"ref", {{"type", "ref/prompt"}, {"name", "p"}}},
                             {"argument", {{"name", "a"}, {"value", "b"}}}};
    const std::vector<std::pair<std::string, json>> requests = {
        {"resources/subscribe", {{"uri", "mem://a"}}},
        {"resources/unsubscribe", {{"uri", "mem://a"}}},
        {"completion/complete", completion},
    };
    int id = 1;
    for (const auto& [method, params] : requests) {
        for (const bool refuse : {true, false}) {
            json sent = params;
            if (refuse) {
                sent["refuse"] = true;
            }
            transport_ptr->enqueue_message(
                json{{"jsonrpc", "2.0"}, {"id", id++}, {"method", method}, {"params", sent}}.dump());
        }
    }

    boost::asio::co_spawn(io, server.run(transport, io.get_executor()), boost::asio::detached);
    io.run();

    ASSERT_EQ(responses.size(), 7U);
    EXPECT_EQ(handler_calls, 3);
    for (int request = 1; request <= 6; ++request) {
        const auto& response = responses[static_cast<std::size_t>(request)];
        EXPECT_EQ(response.contains("error"), request % 2 == 1) << response;
    }
}

TEST(ServerMiddlewareTest, MultipleMiddlewaresExecuteInOrder) {
    boost::asio::io_context io;

    ServerCapabilities caps;
    ServerCapabilities::ToolsCapability tools_cap;
    caps.tools = std::move(tools_cap);

    Implementation info;
    info.name = "test-server";
    info.version = "1.0";

    Server server(std::move(info), std::move(caps));

    // Track execution order
    auto order = std::make_shared<std::vector<int>>();

    // Middleware 1: logs 1 before, 3 after
    server.use([order](Context& ctx, const json& params, TypeErasedHandler next) -> Task<json> {
        order->push_back(1);
        auto result = co_await next(ctx, params);
        order->push_back(3);
        co_return result;
    });

    // Middleware 2: logs 2 before, 4 after
    server.use([order](Context& ctx, const json& params, TypeErasedHandler next) -> Task<json> {
        order->push_back(2);
        auto result = co_await next(ctx, params);
        order->push_back(4);
        co_return result;
    });

    // Tool handler
    server.add_tool<json, json>("track", "Tracks execution order", json{{"type", "object"}},
                                [order](const json&) -> json {
                                    order->push_back(99);
                                    return json{{"order", *order}};
                                });

    auto transport = std::make_shared<ScriptedTransport>(io.get_executor());
    auto* transport_ptr = transport.get();

    std::vector<json> responses;
    transport_ptr->set_on_write([&responses, transport_ptr](std::string_view msg) {
        responses.push_back(json::parse(msg));
        if (responses.size() >= 2) {
            transport_ptr->close();
        }
    });

    transport_ptr->enqueue_message(make_initialize_request("1").dump());
    transport_ptr->enqueue_message(make_initialized_notification().dump());

    json call_req{{"jsonrpc", "2.0"},
                  {"id", "2"},
                  {"method", "tools/call"},
                  {"params", {{"name", "track"}, {"arguments", nlohmann::json::object()}}}};
    transport_ptr->enqueue_message(call_req.dump());

    boost::asio::co_spawn(io, server.run(transport, io.get_executor()), boost::asio::detached);
    io.run();

    // Expected order: [1, 2, 99, 4, 3]
    ASSERT_EQ(order->size(), 5);
    EXPECT_EQ((*order)[0], 1);
    EXPECT_EQ((*order)[1], 2);
    EXPECT_EQ((*order)[2], 99);
    EXPECT_EQ((*order)[3], 4);
    EXPECT_EQ((*order)[4], 3);
}

// Test 4: Middleware post-processes result after handler
TEST(ServerMiddlewareTest, MiddlewarePostProcessesResult) {
    boost::asio::io_context io;

    ServerCapabilities caps;
    ServerCapabilities::ToolsCapability tools_cap;
    caps.tools = std::move(tools_cap);

    Implementation info;
    info.name = "test-server";
    info.version = "1.0";

    Server server(std::move(info), std::move(caps));

    // Middleware that doubles the result and adds metadata
    server.use([](Context& ctx, const json& params, TypeErasedHandler next) -> Task<json> {
        auto result = co_await next(ctx, params);

        // Post-process: double the sum if present
        if (result.is_object() && result.contains("sum")) {
            result["sum"] = result["sum"].get<int>() * 2;
            result["metadata"] = "processed by middleware";
        }

        co_return result;
    });

    // Tool that computes value * 2
    server.add_tool<json, json>(
        "compute", "Computes value * 2",
        json{{"type", "object"}, {"properties", {{"value", {{"type", "integer"}}}}}},
        [](const json& args) -> json {
            int value = args.value("value", 0);
            return json{{"sum", value * 2}};
        });

    auto transport = std::make_shared<ScriptedTransport>(io.get_executor());
    auto* transport_ptr = transport.get();

    std::vector<json> responses;
    transport_ptr->set_on_write([&responses, transport_ptr](std::string_view msg) {
        responses.push_back(json::parse(msg));
        if (responses.size() >= 2) {
            transport_ptr->close();
        }
    });

    transport_ptr->enqueue_message(make_initialize_request("1").dump());
    transport_ptr->enqueue_message(make_initialized_notification().dump());

    json call_req{{"jsonrpc", "2.0"},
                  {"id", "2"},
                  {"method", "tools/call"},
                  {"params", {{"name", "compute"}, {"arguments", {{"value", 10}}}}}};
    transport_ptr->enqueue_message(call_req.dump());

    boost::asio::co_spawn(io, server.run(transport, io.get_executor()), boost::asio::detached);
    io.run();

    // Check the result: original would be 20, middleware doubles to 40
    auto it = std::find_if(responses.begin(), responses.end(), [](const json& msg) {
        return msg.contains("id") && msg["id"] == "2" && msg.contains("result");
    });
    ASSERT_NE(it, responses.end());
    EXPECT_EQ((*it)["result"]["structuredContent"]["sum"], 40);
    EXPECT_EQ((*it)["result"]["structuredContent"]["metadata"], "processed by middleware");
}

namespace {

// Runs one tools/call for `tool` through `server` and returns its response.
json call_one_tool(Server& server, const std::string& tool) {
    boost::asio::io_context io;
    auto transport = std::make_shared<ScriptedTransport>(io.get_executor());
    auto* transport_ptr = transport.get();

    std::vector<json> responses;
    transport_ptr->set_on_write([&responses, transport_ptr](std::string_view msg) {
        responses.push_back(json::parse(msg));
        if (responses.size() >= 2) {
            transport_ptr->close();
        }
    });

    transport_ptr->enqueue_message(make_initialize_request("1").dump());
    transport_ptr->enqueue_message(make_initialized_notification().dump());
    transport_ptr->enqueue_message(json{{"jsonrpc", "2.0"},
                                        {"id", "2"},
                                        {"method", "tools/call"},
                                        {"params", {{"name", tool}, {"arguments", json::object()}}}}
                                       .dump());

    boost::asio::co_spawn(io, server.run(transport, io.get_executor()), boost::asio::detached);
    io.run();

    auto it = std::find_if(responses.begin(), responses.end(),
                           [](const json& msg) { return msg.contains("id") && msg["id"] == "2"; });
    return it == responses.end() ? json() : *it;
}

Server make_tool_server() {
    ServerCapabilities caps;
    caps.tools = ServerCapabilities::ToolsCapability{};
    Implementation info;
    info.name = "test-server";
    info.version = "1.0";
    return Server(std::move(info), std::move(caps));
}

}  // namespace

// A middleware that refuses a tool call with a tool error returns a complete CallToolResult, and
// that result reaches the client as it is rather than wrapped as structuredContent.
TEST(ServerMiddlewareTest, MiddlewareReturningAToolErrorResultIsSentUnchanged) {
    auto server = make_tool_server();
    bool handler_called = false;
    server.use([](Context&, const json&, TypeErasedHandler) -> Task<json> {
        co_return json(make_tool_error_result("blocked by policy"));
    });
    server.add_tool<json, json>("guarded", "Guarded tool", json{{"type", "object"}},
                                [&handler_called](const json&) -> json {
                                    handler_called = true;
                                    return json::object();
                                });

    const auto response = call_one_tool(server, "guarded");

    ASSERT_TRUE(response.contains("result")) << response.dump();
    EXPECT_FALSE(handler_called);
    EXPECT_TRUE(response["result"]["isError"].get<bool>());
    EXPECT_EQ(response["result"]["content"][0]["text"], "blocked by policy");
    EXPECT_FALSE(response["result"].contains("structuredContent")) << response.dump();
}

// A tool handler's exception is turned into an isError result inside the guard, before middleware
// sees anything, so a middleware wrapping next() in a try/catch receives a result, not a throw.
TEST(ServerMiddlewareTest, ToolExceptionReachesMiddlewareAsAnErrorResult) {
    auto server = make_tool_server();
    bool middleware_caught = false;
    bool middleware_saw_error_result = false;
    server.use([&](Context& ctx, const json& params, TypeErasedHandler next) -> Task<json> {
        json result;
        try {
            result = co_await next(ctx, params);
        } catch (...) {
            middleware_caught = true;
            throw;
        }
        middleware_saw_error_result = result.value("isError", false);
        co_return result;
    });
    server.add_tool<json, json>("failing", "Always throws", json{{"type", "object"}},
                                [](const json&) -> json { throw std::runtime_error("tool broke"); });

    const auto response = call_one_tool(server, "failing");

    ASSERT_TRUE(response.contains("result")) << response.dump();
    EXPECT_FALSE(middleware_caught);
    EXPECT_TRUE(middleware_saw_error_result);
    EXPECT_TRUE(response["result"]["isError"].get<bool>());
    EXPECT_NE(response["result"]["content"][0]["text"].get<std::string>().find("tool broke"),
              std::string::npos);
}

namespace {

struct ServedRequest {
    std::string method;
    json params;
};

const std::vector<ServedRequest>& handled_requests() {
    static const std::vector<ServedRequest> requests{
        {"resources/read", {{"uri", "test://failing"}}},
        {"prompts/get", {{"name", "failing"}}},
    };
    return requests;
}

const std::vector<ServedRequest>& middleware_requests() {
    static const std::vector<ServedRequest> requests{
        {"tools/call", {{"name", "failing"}, {"arguments", json::object()}}},
        {"resources/read", {{"uri", "test://failing"}}},
        {"prompts/get", {{"name", "failing"}}},
    };
    return requests;
}

Server make_full_server() {
    ServerCapabilities caps;
    caps.tools = ServerCapabilities::ToolsCapability{};
    caps.resources = ServerCapabilities::ResourcesCapability{};
    caps.prompts = ServerCapabilities::PromptsCapability{};
    Implementation info;
    info.name = "test-server";
    info.version = "1.0";
    return Server(std::move(info), std::move(caps));
}

void add_handlers(Server& server, bool handlers_throw) {
    server.add_tool<json, json>("failing", "Reached only by way of the middleware",
                                json{{"type", "object"}},
                                [](const json&) -> json { return json::object(); });
    Resource resource;
    resource.uri = "test://failing";
    resource.name = "failing";
    Prompt prompt;
    prompt.name = "failing";
    if (handlers_throw) {
        server.add_resource<json, json>(resource, [](const json&) -> json { throw 42; });
        server.add_prompt<json, json>(prompt, [](const json&) -> json { throw 42; });
    } else {
        server.add_resource<json, json>(resource, [](const json&) -> json { return json::object(); });
        server.add_prompt<json, json>(prompt, [](const json&) -> json { return json::object(); });
    }
}

// Feeds `messages` to `server` and returns the response carrying `id`, or null if none arrives.
json answer_to(Server& server, const std::vector<json>& messages, const json& id) {
    boost::asio::io_context io;
    auto transport = std::make_shared<ScriptedTransport>(io.get_executor());
    auto* transport_ptr = transport.get();

    // A request nobody answers keeps the server running even after its transport closes. The timer
    // runs out only then, so that the regression fails the test instead of hanging the suite.
    boost::asio::steady_timer watchdog(io);
    watchdog.expires_after(g_hang_guard);
    watchdog.async_wait([&io](const boost::system::error_code& error) {
        if (!error) {
            io.stop();
        }
    });

    json answer;
    transport_ptr->set_on_write([&](std::string_view msg) {
        auto parsed = json::parse(msg);
        if (parsed.contains("id") && parsed["id"] == id) {
            answer = std::move(parsed);
            watchdog.cancel();
            transport_ptr->close();
        }
    });

    for (const auto& message : messages) {
        transport_ptr->enqueue_message(message.dump());
    }

    boost::asio::co_spawn(io, server.run(transport, io.get_executor()), boost::asio::detached);
    io.run();
    return answer;
}

json answer_legacy(Server& server, const ServedRequest& request) {
    return answer_to(
        server,
        {make_initialize_request("1"), make_initialized_notification(),
         json{{"jsonrpc", "2.0"}, {"id", "2"}, {"method", request.method}, {"params", request.params}}},
        "2");
}

json answer_modern(Server& server, const ServedRequest& request) {
    return answer_to(server, {make_modern_request("2", request.method, request.params)}, "2");
}

void expect_internal_error(const json& response) {
    ASSERT_TRUE(response.contains("error")) << response.dump();
    EXPECT_EQ(response["error"]["code"], g_INTERNAL_ERROR);
    EXPECT_EQ(response["error"]["message"], "Internal error");
}

}  // namespace

TEST(ServerMiddlewareTest, NonStdExceptionFromMiddlewareIsAnsweredOnLegacyRequests) {
    for (const auto& request : middleware_requests()) {
        SCOPED_TRACE(request.method);
        auto server = make_full_server();
        add_handlers(server, false);
        server.use([](Context&, const json&, TypeErasedHandler) -> Task<json> { throw 42; });
        expect_internal_error(answer_legacy(server, request));
    }
}

TEST(ServerMiddlewareTest, NonStdExceptionFromMiddlewareIsAnsweredOnModernRequests) {
    for (const auto& request : middleware_requests()) {
        SCOPED_TRACE(request.method);
        auto server = make_full_server();
        add_handlers(server, false);
        server.use([](Context&, const json&, TypeErasedHandler) -> Task<json> { throw 42; });
        expect_internal_error(answer_modern(server, request));
    }
}

TEST(ServerMiddlewareTest, NonStdExceptionFromHandlerIsAnsweredOnLegacyRequests) {
    for (const auto& request : handled_requests()) {
        SCOPED_TRACE(request.method);
        auto server = make_full_server();
        add_handlers(server, true);
        expect_internal_error(answer_legacy(server, request));
    }
}

TEST(ServerMiddlewareTest, NonStdExceptionFromHandlerIsAnsweredOnModernRequests) {
    for (const auto& request : handled_requests()) {
        SCOPED_TRACE(request.method);
        auto server = make_full_server();
        add_handlers(server, true);
        expect_internal_error(answer_modern(server, request));
    }
}

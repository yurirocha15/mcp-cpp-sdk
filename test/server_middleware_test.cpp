/**
 * @file server_middleware_test.cpp
 * @brief Tests for Server middleware functionality
 */

#include "test_utils.hpp"

#include <gtest/gtest.h>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <mcp/protocol.hpp>
#include <mcp/server.hpp>
#include <nlohmann/json.hpp>
#include <string>
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
    EXPECT_EQ((*it)["result"]["sum"], 115);
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
            co_return json{{"error", "Tool 'forbidden' is not allowed"}};
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

    json call_req{{"jsonrpc", "2.0"},
                  {"id", "2"},
                  {"method", "tools/call"},
                  {"params", {{"name", "forbidden"}, {"arguments", {}}}}};
    transport_ptr->enqueue_message(call_req.dump());

    boost::asio::co_spawn(io, server.run(transport, io.get_executor()), boost::asio::detached);
    io.run();

    // Verify the tool was never executed
    EXPECT_FALSE(tool_executed);

    // Check that a result was sent with an error field
    auto it = std::find_if(responses.begin(), responses.end(), [](const json& msg) {
        return msg.contains("id") && msg["id"] == "2" && msg.contains("result");
    });
    ASSERT_NE(it, responses.end());
    EXPECT_TRUE((*it)["result"].contains("error"));
    EXPECT_TRUE((*it)["result"]["error"].get<std::string>().find("forbidden") != std::string::npos);
}

// Test 3: Multiple middlewares execute in order (1→2→handler→2→1)
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

    json call_req{{"jsonrpc", "2.0"},
                  {"id", "2"},
                  {"method", "tools/call"},
                  {"params", {{"name", "track"}, {"arguments", {}}}}};
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
    EXPECT_EQ((*it)["result"]["sum"], 40);
    EXPECT_EQ((*it)["result"]["metadata"], "processed by middleware");
}

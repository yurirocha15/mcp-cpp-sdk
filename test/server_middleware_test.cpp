/**
 * @file server_middleware_test.cpp
 * @brief Tests for Server middleware functionality
 */

#include <gtest/gtest.h>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <mcp/protocol.hpp>
#include <mcp/server.hpp>
#include <nlohmann/json.hpp>
#include <queue>
#include <string>
#include <vector>

using namespace mcp;
using json = nlohmann::json;

namespace {

/**
 * @brief A simple scripted transport for testing
 *
 * Pre-loads a sequence of messages to return on read, and captures writes.
 */
class ScriptedTransport : public ITransport {
   public:
    explicit ScriptedTransport(const boost::asio::any_io_executor& executor)
        : strand_(boost::asio::make_strand(executor)), timer_(strand_) {
        timer_.expires_at(std::chrono::steady_clock::time_point::max());
    }

    Task<std::string> read_message() override {
        for (;;) {
            if (closed_) {
                throw std::runtime_error("transport closed");
            }
            if (!incoming_.empty()) {
                auto msg = std::move(incoming_.front());
                incoming_.pop();
                co_return msg;
            }
            timer_.expires_at(std::chrono::steady_clock::time_point::max());
            try {
                co_await timer_.async_wait(boost::asio::use_awaitable);
            } catch (const boost::system::system_error& err) {
                if (err.code() != boost::asio::error::operation_aborted) {
                    throw;
                }
            }
        }
    }

    Task<void> write_message(std::string_view message) override {
        co_await boost::asio::post(strand_, boost::asio::use_awaitable);
        written_.emplace_back(message);
        if (on_write_) {
            on_write_(written_.back());
        }
        co_return;
    }

    void close() override {
        closed_ = true;
        timer_.cancel();
    }

    void enqueue_message(std::string msg) {
        boost::asio::post(strand_, [this, m = std::move(msg)]() mutable {
            incoming_.push(std::move(m));
            timer_.cancel();
        });
    }

    void set_on_write(std::function<void(std::string_view)> callback) {
        on_write_ = std::move(callback);
    }

    [[nodiscard]] const std::vector<std::string>& written() const { return written_; }

   private:
    boost::asio::strand<boost::asio::any_io_executor> strand_;
    boost::asio::steady_timer timer_;
    std::queue<std::string> incoming_;
    std::vector<std::string> written_;
    std::function<void(std::string_view)> on_write_;
    bool closed_ = false;
};

json make_initialize_request(std::string_view req_id) {
    return {{"jsonrpc", "2.0"},
            {"id", req_id},
            {"method", "initialize"},
            {"params",
             {{"protocolVersion", LATEST_PROTOCOL_VERSION},
              {"clientInfo", {{"name", "test-client"}, {"version", "0.1"}}},
              {"capabilities", json::object()}}}};
}

}  // namespace

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

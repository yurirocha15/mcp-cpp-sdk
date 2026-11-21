#include "../test_utils.hpp"

#include "../support/spawn.hpp"
#include "mcp/core/async.hpp"
#include "mcp/core/runtime.hpp"
#include "mcp/server/server.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <map>
#include <mcp/core/constants.hpp>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace server_handlers_test {

namespace protocol = mcp::constants::protocol;

struct AddParams {
    int augend = 0;
    int addend = 0;
};

inline void to_json(nlohmann::json& json_obj, const AddParams& params) {
    json_obj = nlohmann::json{{"augend", params.augend}, {"addend", params.addend}};
}

inline void from_json(const nlohmann::json& json_obj, AddParams& params) {
    json_obj.at("augend").get_to(params.augend);
    json_obj.at("addend").get_to(params.addend);
}

struct AddResult {
    int sum = 0;
};

inline void to_json(nlohmann::json& json_obj, const AddResult& result) {
    json_obj = nlohmann::json{{"sum", result.sum}};
}

inline void from_json(const nlohmann::json& json_obj, AddResult& result) {
    json_obj.at("sum").get_to(result.sum);
}

class ServerHandlersTest : public ::testing::Test {
   protected:
    boost::asio::io_context io_ctx_;

    struct ServerSetup {
        std::shared_ptr<mcp::ITransport> transport;
        ScriptedTransport* raw_transport;
        mcp::Server server;

        ServerSetup(boost::asio::io_context& io_ctx, mcp::ServerCapabilities caps)
            : transport(std::make_shared<ScriptedTransport>(io_ctx.get_executor())),
              raw_transport(static_cast<ScriptedTransport*>(transport.get())),
              server(
                  [] {
                      mcp::Implementation info;
                      info.name = "test-server";
                      info.version = "1.0";
                      return info;
                  }(),
                  std::move(caps)) {}
    };

    std::vector<nlohmann::json> run_request(ServerSetup& setup, nlohmann::json request) {
        std::vector<nlohmann::json> responses;
        setup.raw_transport->set_on_write([&responses, &setup](std::string_view message) {
            responses.push_back(nlohmann::json::parse(message));
            if (responses.size() == 2) {
                setup.raw_transport->close();
            }
        });
        setup.raw_transport->enqueue_message(make_initialize_request("1").dump());
        setup.raw_transport->enqueue_message(make_initialized_notification().dump());
        setup.raw_transport->enqueue_message(request.dump());

        mcp_test::spawn_detached(io_ctx_, [&]() -> mcp::Task<void> {
            co_await setup.server.run(setup.transport, io_ctx_.get_executor());
        });
        io_ctx_.run();
        return responses;
    }
};

TEST_F(ServerHandlersTest, SyncToolCallReturnsResult) {
    mcp::ServerCapabilities caps;
    mcp::ServerCapabilities::ToolsCapability tools_cap;
    caps.tools = std::move(tools_cap);

    ServerSetup setup(io_ctx_, std::move(caps));

    setup.server.add_tool<AddParams, AddResult>(
        "add", "Adds two numbers",
        nlohmann::json{
            {"type", "object"},
            {"properties", {{"augend", {{"type", "integer"}}}, {"addend", {{"type", "integer"}}}}}},
        [](AddParams params) -> AddResult { return AddResult{params.augend + params.addend}; });

    std::vector<nlohmann::json> responses;
    setup.raw_transport->set_on_write([&responses, &setup](std::string_view msg) {
        responses.push_back(nlohmann::json::parse(msg));
        if (responses.size() == 2) {
            setup.raw_transport->close();
        }
    });

    setup.raw_transport->enqueue_message(make_initialize_request("1").dump());
    setup.raw_transport->enqueue_message(make_initialized_notification().dump());

    nlohmann::json call_req;
    call_req["jsonrpc"] = "2.0";
    call_req["id"] = "2";
    call_req["method"] = "tools/call";
    nlohmann::json call_params;
    call_params["name"] = "add";
    call_params["arguments"] = nlohmann::json{{"augend", 3}, {"addend", 4}};
    call_req["params"] = std::move(call_params);
    setup.raw_transport->enqueue_message(call_req.dump());

    mcp_test::spawn_detached(io_ctx_, [&]() -> mcp::Task<void> {
        co_await setup.server.run(setup.transport, io_ctx_.get_executor());
    });

    io_ctx_.run();

    ASSERT_EQ(responses.size(), 2);
    EXPECT_EQ(responses[0]["id"], "1");

    auto& tool_response = responses[1];
    EXPECT_EQ(tool_response["id"], "2");
    ASSERT_TRUE(tool_response.contains("result"));
    EXPECT_EQ(tool_response["result"]["structuredContent"]["sum"], 7);
    EXPECT_EQ(tool_response["result"]["content"][0]["type"], "text");
}

TEST_F(ServerHandlersTest, SyncToolWithContextReceivesTheRequestContext) {
    mcp::ServerCapabilities caps;
    caps.tools = mcp::ServerCapabilities::ToolsCapability{};
    ServerSetup setup(io_ctx_, std::move(caps));

    bool saw_live_request = false;
    setup.server.add_tool<AddParams, AddResult>(
        "add", "Adds two numbers", nlohmann::json{{"type", "object"}},
        [&saw_live_request](mcp::Context& ctx, AddParams params) -> AddResult {
            saw_live_request = !ctx.is_cancelled();
            return AddResult{params.augend + params.addend};
        });

    auto responses = run_request(
        setup,
        nlohmann::json{{"jsonrpc", "2.0"},
                       {"id", "2"},
                       {"method", "tools/call"},
                       {"params", {{"name", "add"}, {"arguments", {{"augend", 2}, {"addend", 5}}}}}});

    ASSERT_EQ(responses.size(), 2);
    EXPECT_TRUE(saw_live_request);
    EXPECT_EQ(responses[1]["result"]["structuredContent"]["sum"], 7) << responses[1];
}

TEST_F(ServerHandlersTest, SyncToolRunsOnTheHandlerExecutor) {
    mcp::ServerCapabilities caps;
    caps.tools = mcp::ServerCapabilities::ToolsCapability{};
    ServerSetup setup(io_ctx_, std::move(caps));

    boost::asio::thread_pool pool(1);
    std::promise<std::thread::id> pool_thread;
    boost::asio::post(pool, [&pool_thread] { pool_thread.set_value(std::this_thread::get_id()); });
    const auto pool_thread_id = pool_thread.get_future().get();

    setup.server.set_handler_executor(pool.get_executor());
    std::thread::id handler_thread;
    setup.server.add_tool("where", "Reports its thread", nlohmann::json{{"type", "object"}},
                          [&handler_thread](const nlohmann::json&) -> nlohmann::json {
                              handler_thread = std::this_thread::get_id();
                              return nlohmann::json{{"ok", true}};
                          });

    auto responses = run_request(
        setup,
        nlohmann::json{{"jsonrpc", "2.0"},
                       {"id", "2"},
                       {"method", "tools/call"},
                       {"params", {{"name", "where"}, {"arguments", nlohmann::json::object()}}}});
    pool.join();

    ASSERT_EQ(responses.size(), 2);
    EXPECT_FALSE(responses[1]["result"].value("isError", false)) << responses[1];
    EXPECT_EQ(handler_thread, pool_thread_id);
}

// Destroying the event loop's context while a tool handler runs on the handler executor waits for
// the handler, whose answer then goes nowhere; the server is destroyed afterwards.
TEST(ServerHandlerPoolTest, DestroyingTheEventLoopWaitsForAHandlerOnThePool) {
    boost::asio::thread_pool pool(1);
    std::promise<void> started;
    std::promise<void> release;
    const std::shared_future<void> released = release.get_future().share();
    std::atomic<bool> returned{false};
    auto server = std::make_unique<mcp::Server>("pool-test", "1.0");
    server->set_handler_executor(pool.get_executor());
    server->add_tool("block", "Waits until released", nlohmann::json{{"type", "object"}},
                     [&started, released, &returned](const nlohmann::json&) -> nlohmann::json {
                         started.set_value();
                         released.wait();
                         returned = true;
                         return "done";
                     });

    auto io_ctx = std::make_unique<boost::asio::io_context>();
    auto transport = std::make_shared<ScriptedTransport>(io_ctx->get_executor());
    transport->enqueue_message(make_initialize_request("1").dump());
    transport->enqueue_message(make_initialized_notification().dump());
    transport->enqueue_message(
        nlohmann::json{{"jsonrpc", "2.0"},
                       {"id", "2"},
                       {"method", "tools/call"},
                       {"params", {{"name", "block"}, {"arguments", nlohmann::json::object()}}}}
            .dump());
    boost::asio::co_spawn(*io_ctx, server->run(transport, io_ctx->get_executor()),
                          boost::asio::detached);
    std::thread loop([&io_ctx] { io_ctx->run(); });
    started.get_future().wait();
    io_ctx->stop();
    loop.join();
    transport.reset();
    // Destroying the loop's context waits for the handler its coroutine waits for, which returns
    // meanwhile; released a little later, it returns while that destruction is under way.
    std::thread releaser([&release] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        release.set_value();
    });
    io_ctx.reset();
    EXPECT_TRUE(returned.load());
    releaser.join();

    pool.join();
    server.reset();
}

// The same with a Runtime as the handler executor, destroyed after the loop.
TEST(ServerHandlerPoolTest, DestroyingTheEventLoopWaitsForAHandlerOnARuntime) {
    auto pool = std::make_unique<mcp::Runtime>();
    pool->start(1);
    std::promise<void> started;
    std::promise<void> release;
    const std::shared_future<void> released = release.get_future().share();
    std::atomic<bool> returned{false};
    auto server = std::make_unique<mcp::Server>("pool-test", "1.0");
    server->set_handler_executor(pool->executor());
    server->add_tool("block", "Waits until released", nlohmann::json{{"type", "object"}},
                     [&started, released, &returned](const nlohmann::json&) -> nlohmann::json {
                         started.set_value();
                         released.wait();
                         returned = true;
                         return "done";
                     });

    auto io_ctx = std::make_unique<boost::asio::io_context>();
    auto transport = std::make_shared<ScriptedTransport>(io_ctx->get_executor());
    transport->enqueue_message(make_initialize_request("1").dump());
    transport->enqueue_message(make_initialized_notification().dump());
    transport->enqueue_message(
        nlohmann::json{{"jsonrpc", "2.0"},
                       {"id", "2"},
                       {"method", "tools/call"},
                       {"params", {{"name", "block"}, {"arguments", nlohmann::json::object()}}}}
            .dump());
    boost::asio::co_spawn(*io_ctx, server->run(transport, io_ctx->get_executor()),
                          boost::asio::detached);
    std::thread loop([&io_ctx] { io_ctx->run(); });
    started.get_future().wait();
    io_ctx->stop();
    loop.join();
    transport.reset();
    // Destroying the loop's context waits for the handler its coroutine waits for, which returns
    // meanwhile; released a little later, it returns while that destruction is under way.
    std::thread releaser([&release] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        release.set_value();
    });
    io_ctx.reset();
    EXPECT_TRUE(returned.load());
    releaser.join();

    pool->stop();
    pool.reset();
    server.reset();
}

// A coroutine handler on the handler executor that sends to the client once its event loop's
// context is being destroyed gets an error instead of reaching the loop's objects.
TEST(ServerHandlerPoolTest, AHandlerWhoseEventLoopIsDestroyedCannotReachTheClient) {
    boost::asio::thread_pool pool(1);
    std::promise<void> started;
    std::promise<void> release;
    std::promise<std::string> outcome;
    const std::shared_future<void> released = release.get_future().share();
    auto server = std::make_unique<mcp::Server>("pool-test", "1.0");
    server->set_handler_executor(pool.get_executor());
    server->add_tool("late", "Logs once released", nlohmann::json{{"type", "object"}},
                     [&started, released, &outcome](
                         mcp::Context& ctx, const nlohmann::json&) -> mcp::Task<nlohmann::json> {
                         started.set_value();
                         released.wait();
                         try {
                             co_await ctx.log_info("after the loop stopped");
                             outcome.set_value("sent");
                         } catch (const std::runtime_error& error) {
                             outcome.set_value(error.what());
                         }
                         co_return "done";
                     });

    auto io_ctx = std::make_unique<boost::asio::io_context>();
    auto transport = std::make_shared<ScriptedTransport>(io_ctx->get_executor());
    transport->enqueue_message(make_initialize_request("1").dump());
    transport->enqueue_message(make_initialized_notification().dump());
    transport->enqueue_message(
        nlohmann::json{{"jsonrpc", "2.0"},
                       {"id", "2"},
                       {"method", "tools/call"},
                       {"params", {{"name", "late"}, {"arguments", nlohmann::json::object()}}}}
            .dump());
    boost::asio::co_spawn(*io_ctx, server->run(transport, io_ctx->get_executor()),
                          boost::asio::detached);
    std::thread loop([&io_ctx] { io_ctx->run(); });
    started.get_future().wait();
    io_ctx->stop();
    loop.join();
    transport.reset();
    // Destroying the loop's context waits for the handler its coroutine waits for, which returns
    // meanwhile.
    std::thread releaser([&release] { release.set_value(); });
    io_ctx.reset();
    releaser.join();

    EXPECT_EQ(outcome.get_future().get(), "The server has stopped");
    pool.join();
    server.reset();
}

namespace {

/// An initialize request from a client that answers elicitation requests.
nlohmann::json make_eliciting_initialize_request() {
    auto request = make_initialize_request("1");
    request["params"]["capabilities"] = nlohmann::json{{"elicitation", nlohmann::json::object()}};
    return request;
}

/// Registers a tool "ask" that asks the client a question nobody answers and reports, through
/// @p outcome, what ended the wait: the error's text, or "unwound" when its frame is destroyed
/// instead.
void add_unanswered_question(mcp::Server& server, std::promise<std::string>& outcome) {
    server.add_tool("ask", "Asks the client", nlohmann::json{{"type", "object"}},
                    [&outcome](mcp::Context& ctx, const nlohmann::json&) -> mcp::Task<nlohmann::json> {
                        struct Report {
                            std::promise<std::string>* outcome;
                            bool done = false;
                            explicit Report(std::promise<std::string>* reported) : outcome(reported) {}
                            Report(const Report&) = delete;
                            Report& operator=(const Report&) = delete;
                            ~Report() {
                                if (!done) {
                                    outcome->set_value("unwound");
                                }
                            }
                        };
                        Report report{&outcome};
                        // On the heap: a std::string in the coroutine frame must not live across
                        // co_await.
                        const auto question = std::make_shared<const mcp::ElicitRequestParams>([] {
                            mcp::ElicitRequestFormParams form;
                            form.message = "Anyone there?";
                            form.requestedSchema = nlohmann::json{{"type", "object"}};
                            return mcp::ElicitRequestParams(std::move(form));
                        }());
                        try {
                            (void)co_await ctx.elicit(*question);
                            report.done = true;
                            outcome.set_value("answered");
                        } catch (const std::exception& error) {
                            report.done = true;
                            outcome.set_value(error.what());
                        }
                        co_return "done";
                    });
}

/// Serves "ask" on a loop of its own thread for @p server until the question reaches the client,
/// then stops that loop, calls @p before_destroying and destroys the loop's context.
void serve_until_asked(mcp::Server& server, const std::function<void()>& before_destroying = [] {}) {
    auto io_ctx = std::make_unique<boost::asio::io_context>();
    auto transport = std::make_shared<ScriptedTransport>(io_ctx->get_executor());
    std::promise<void> asked;
    bool signalled = false;
    transport->set_on_write([&asked, &signalled](std::string_view message) {
        if (!signalled && message.find("elicitation/create") != std::string_view::npos) {
            signalled = true;
            asked.set_value();
        }
    });
    transport->enqueue_message(make_eliciting_initialize_request().dump());
    transport->enqueue_message(make_initialized_notification().dump());
    transport->enqueue_message(
        nlohmann::json{{"jsonrpc", "2.0"},
                       {"id", "2"},
                       {"method", "tools/call"},
                       {"params", {{"name", "ask"}, {"arguments", nlohmann::json::object()}}}}
            .dump());
    boost::asio::co_spawn(*io_ctx, server.run(transport, io_ctx->get_executor()),
                          boost::asio::detached);
    std::thread loop([&io_ctx] { io_ctx->run(); });
    asked.get_future().wait();
    io_ctx->stop();
    loop.join();
    before_destroying();
    transport.reset();
    io_ctx.reset();
}

}  // namespace

// A Context call under way when its event loop's context is destroyed ends with an error in the
// handler, on the handler executor.
TEST(ServerHandlerPoolTest, AContextCallUnderWayWhenItsEventLoopIsDestroyedThrows) {
    boost::asio::thread_pool pool(1);
    std::promise<std::string> outcome;
    auto server = std::make_unique<mcp::Server>("pool-test", "1.0");
    server->set_handler_executor(pool.get_executor());
    add_unanswered_question(*server, outcome);

    serve_until_asked(*server);

    EXPECT_EQ(outcome.get_future().get(), "The server has stopped");
    pool.join();
    server.reset();
}

// The same, when the handler executor's context is destroyed before the event loop's: it waits for
// the Context call, and the handler goes with the loop's context.
TEST(ServerHandlerPoolTest, AContextCallUnderWayWhenItsHandlerExecutorIsDestroyedFirstUnwindsIt) {
    auto pool = std::make_unique<mcp::Runtime>();
    pool->start(1);
    std::promise<std::string> outcome;
    auto ended = outcome.get_future();
    auto server = std::make_unique<mcp::Server>("pool-test", "1.0");
    server->set_handler_executor(pool->executor());
    add_unanswered_question(*server, outcome);

    // Destroying the handler executor's context waits for the Context call under way on the loop,
    // which ends as the loop's context is destroyed, and the handler is destroyed with it. Stopped
    // first, the handler executor runs nothing more.
    std::thread destroyer;
    serve_until_asked(*server, [&destroyer, &pool] {
        pool->stop();
        destroyer = std::thread([&pool] { pool.reset(); });
    });
    destroyer.join();
    server.reset();

    EXPECT_EQ(ended.get(), "unwound");
}

// A coroutine handler on the handler executor reaches the client through the server's loop.
TEST_F(ServerHandlersTest, AToolOnTheHandlerExecutorLogsToTheClient) {
    mcp::ServerCapabilities caps;
    caps.tools = mcp::ServerCapabilities::ToolsCapability{};
    ServerSetup setup(io_ctx_, std::move(caps));
    boost::asio::thread_pool pool(1);
    setup.server.set_handler_executor(pool.get_executor());
    setup.server.add_tool("log_then_answer", "Logs, then answers", nlohmann::json{{"type", "object"}},
                          [](mcp::Context& ctx, const nlohmann::json&) -> mcp::Task<nlohmann::json> {
                              co_await ctx.log_info("from the pool");
                              co_return "answered";
                          });

    std::vector<nlohmann::json> responses;
    setup.raw_transport->set_on_write([&responses, &setup](std::string_view msg) {
        responses.push_back(nlohmann::json::parse(msg));
        if (responses.size() == 3) {
            setup.raw_transport->close();
        }
    });
    setup.raw_transport->enqueue_message(make_initialize_request("1").dump());
    setup.raw_transport->enqueue_message(make_initialized_notification().dump());
    setup.raw_transport->enqueue_message(nlohmann::json{
        {"jsonrpc", "2.0"},
        {"id", "2"},
        {"method", "tools/call"},
        {"params", {{"name", "log_then_answer"}, {"arguments", nlohmann::json::object()}}}}
                                             .dump());
    mcp_test::spawn_detached(io_ctx_, [&]() -> mcp::Task<void> {
        co_await setup.server.run(setup.transport, io_ctx_.get_executor());
    });
    io_ctx_.run();
    pool.join();

    ASSERT_EQ(responses.size(), 3);
    EXPECT_EQ(responses[1]["method"], "notifications/message");
    EXPECT_EQ(responses[1]["params"]["data"], "from the pool");
    EXPECT_FALSE(responses[2]["result"].value("isError", false)) << responses[2];
}

// A middleware that calls the next handler more than once, with the handlers on the handler
// executor, reaches the client through the Context from the handlers and after the calls.
TEST_F(ServerHandlersTest, AMiddlewareThatCallsNextAgainReachesTheClientWithAHandlerExecutor) {
    mcp::ServerCapabilities caps;
    caps.tools = mcp::ServerCapabilities::ToolsCapability{};
    ServerSetup setup(io_ctx_, std::move(caps));
    boost::asio::thread_pool pool(2);
    setup.server.set_handler_executor(pool.get_executor());
    setup.server.use([](mcp::Context& ctx, const nlohmann::json& params,
                        mcp::TypeErasedHandler next) -> mcp::Task<nlohmann::json> {
        nlohmann::json result;
        for (int call = 0; call < 3; ++call) {
            result = co_await next(ctx, params);
        }
        co_await ctx.log_info("after the calls");
        co_return result;
    });
    setup.server.add_tool("log_then_answer", "Logs, then answers", nlohmann::json{{"type", "object"}},
                          [](mcp::Context& ctx, const nlohmann::json&) -> mcp::Task<nlohmann::json> {
                              co_await ctx.log_info("from the pool");
                              co_return "answered";
                          });
    setup.server.add_text_resource("test://text", "text", [] { return std::string("hello"); });
    setup.server.add_text_prompt("greet", "Greets", {},
                                 [](const mcp::Server::PromptArguments&) { return std::string("hi"); });

    std::vector<nlohmann::json> messages;
    int answers = 0;
    setup.raw_transport->set_on_write([&messages, &answers, &setup](std::string_view msg) {
        messages.push_back(nlohmann::json::parse(msg));
        if (messages.back().contains("id") && ++answers == 4) {
            setup.raw_transport->close();
        }
    });
    setup.raw_transport->enqueue_message(make_initialize_request("1").dump());
    setup.raw_transport->enqueue_message(make_initialized_notification().dump());
    setup.raw_transport->enqueue_message(nlohmann::json{
        {"jsonrpc", "2.0"},
        {"id", "2"},
        {"method", "tools/call"},
        {"params", {{"name", "log_then_answer"}, {"arguments", nlohmann::json::object()}}}}
                                             .dump());
    setup.raw_transport->enqueue_message(nlohmann::json{
        {"jsonrpc", "2.0"},
        {"id", "3"},
        {"method", "resources/read"},
        {"params", {{"uri", "test://text"}}}}.dump());
    setup.raw_transport->enqueue_message(nlohmann::json{
        {"jsonrpc", "2.0"},
        {"id", "4"},
        {"method", "prompts/get"},
        {"params", {{"name", "greet"}}}}.dump());
    mcp_test::spawn_detached(io_ctx_, [&]() -> mcp::Task<void> {
        co_await setup.server.run(setup.transport, io_ctx_.get_executor());
    });
    io_ctx_.run();
    pool.join();

    std::map<std::string, int> logged;
    std::map<std::string, nlohmann::json> answered;
    for (const auto& message : messages) {
        if (message.value("method", "") == "notifications/message") {
            ++logged[message["params"]["data"].get<std::string>()];
        } else if (message.contains("id")) {
            answered[message["id"].get<std::string>()] = message;
        }
    }
    EXPECT_EQ(logged["from the pool"], 3);
    EXPECT_EQ(logged["after the calls"], 3);
    ASSERT_EQ(answered.size(), 4);
    EXPECT_FALSE(answered["2"]["result"].value("isError", false)) << answered["2"];
    EXPECT_EQ(answered["3"]["result"]["contents"][0]["text"], "hello") << answered["3"];
    EXPECT_TRUE(answered["4"].contains("result")) << answered["4"];
}

// A request a handler makes through its Context after its session ended says so.
TEST_F(ServerHandlersTest, AContextRequestAfterTheSessionEndedSaysTheSessionEnded) {
    mcp::ServerCapabilities caps;
    caps.tools = mcp::ServerCapabilities::ToolsCapability{};
    ServerSetup setup(io_ctx_, std::move(caps));
    std::string outcome;
    setup.server.add_tool(
        "late", "Asks for the roots once the client has gone", nlohmann::json{{"type", "object"}},
        [&setup, &outcome](mcp::Context& ctx, const nlohmann::json&) -> mcp::Task<nlohmann::json> {
            setup.raw_transport->close();
            while (!ctx.is_cancelled()) {
                boost::asio::steady_timer timer(co_await boost::asio::this_coro::executor,
                                                std::chrono::milliseconds(1));
                co_await timer.async_wait(boost::asio::use_awaitable);
            }
            try {
                (void)co_await ctx.request_roots();
                outcome = "answered";
            } catch (const std::exception& error) {
                outcome = error.what();
            }
            co_return "done";
        });
    auto initialize = make_initialize_request("1");
    initialize["params"]["capabilities"] = nlohmann::json{{"roots", nlohmann::json::object()}};
    setup.raw_transport->enqueue_message(initialize.dump());
    setup.raw_transport->enqueue_message(make_initialized_notification().dump());
    setup.raw_transport->enqueue_message(
        nlohmann::json{{"jsonrpc", "2.0"},
                       {"id", "2"},
                       {"method", "tools/call"},
                       {"params", {{"name", "late"}, {"arguments", nlohmann::json::object()}}}}
            .dump());
    mcp_test::spawn_detached(io_ctx_, [&]() -> mcp::Task<void> {
        co_await setup.server.run(setup.transport, io_ctx_.get_executor());
    });
    io_ctx_.run();

    EXPECT_EQ(outcome, "reverse RPC is unavailable: the client's session has ended");
}

// A middleware may return the next handler's coroutine without awaiting it.
TEST_F(ServerHandlersTest, AMiddlewareMayReturnTheNextHandlerUnawaited) {
    mcp::ServerCapabilities caps;
    caps.tools = mcp::ServerCapabilities::ToolsCapability{};
    ServerSetup setup(io_ctx_, std::move(caps));
    const auto pass_on = [](mcp::Context& ctx, const nlohmann::json& params,
                            mcp::TypeErasedHandler next) -> mcp::Task<nlohmann::json> {
        return next(ctx, params);
    };
    setup.server.use(pass_on);
    setup.server.use(pass_on);
    const auto greeting = std::make_shared<const std::string>("hello from the tool");
    setup.server.add_tool<nlohmann::json, nlohmann::json>(
        "greet", "Greets", nlohmann::json{{"type", "object"}},
        [greeting](mcp::Context&, nlohmann::json) -> mcp::Task<nlohmann::json> {
            co_return nlohmann::json(*greeting);
        });

    auto responses = run_request(
        setup,
        nlohmann::json{{"jsonrpc", "2.0"},
                       {"id", "2"},
                       {"method", "tools/call"},
                       {"params", {{"name", "greet"}, {"arguments", nlohmann::json::object()}}}});

    ASSERT_EQ(responses.size(), 2);
    EXPECT_NE(responses[1].dump().find("hello from the tool"), std::string::npos) << responses[1];
}

// When the handler executor ends while a tool handler waits on it and the server's loop goes on,
// the call ends with a tool error on that loop.
TEST_F(ServerHandlersTest, AToolCallFailsWhenTheHandlerExecutorEndsFirst) {
    mcp::ServerCapabilities caps;
    caps.tools = mcp::ServerCapabilities::ToolsCapability{};
    ServerSetup setup(io_ctx_, std::move(caps));
    auto pool = std::make_unique<mcp::Runtime>();
    pool->start(1);
    setup.server.set_handler_executor(pool->executor());
    const auto loop = io_ctx_.get_executor();
    setup.server.add_tool("wait", "Waits on the handler executor", nlohmann::json{{"type", "object"}},
                          [&pool, loop](const nlohmann::json&) -> mcp::Task<nlohmann::json> {
                              const auto executor = co_await boost::asio::this_coro::executor;
                              boost::asio::steady_timer never(executor, std::chrono::hours(1));
                              // The loop destroys the handler executor, whose Runtime waits for this
                              // thread to leave the handler, so the wait below is under way when its
                              // context goes.
                              boost::asio::post(loop, [&pool] { pool.reset(); });
                              co_await never.async_wait(boost::asio::use_awaitable);
                              co_return "done";
                          });

    auto responses = run_request(
        setup, nlohmann::json{{"jsonrpc", "2.0"},
                              {"id", "2"},
                              {"method", "tools/call"},
                              {"params", {{"name", "wait"}, {"arguments", nlohmann::json::object()}}}});

    ASSERT_EQ(responses.size(), 2);
    EXPECT_TRUE(responses[1]["result"].value("isError", false)) << responses[1];
    EXPECT_NE(responses[1].dump().find("ended before the handler returned"), std::string::npos)
        << responses[1];
}

TEST_F(ServerHandlersTest, SyncHandlersRunThroughMiddleware) {
    mcp::ServerCapabilities caps;
    caps.tools = mcp::ServerCapabilities::ToolsCapability{};
    caps.resources = mcp::ServerCapabilities::ResourcesCapability{};
    ServerSetup setup(io_ctx_, std::move(caps));

    int middleware_calls = 0;
    setup.server.use([&middleware_calls](mcp::Context& ctx, const nlohmann::json& params,
                                         mcp::TypeErasedHandler next) -> mcp::Task<nlohmann::json> {
        ++middleware_calls;
        co_return co_await next(ctx, params);
    });
    setup.server.add_tool<AddParams, AddResult>(
        "add", "Adds two numbers", nlohmann::json{{"type", "object"}},
        [](AddParams params) -> AddResult { return AddResult{params.augend + params.addend}; });

    auto responses = run_request(
        setup,
        nlohmann::json{{"jsonrpc", "2.0"},
                       {"id", "2"},
                       {"method", "tools/call"},
                       {"params", {{"name", "add"}, {"arguments", {{"augend", 1}, {"addend", 1}}}}}});

    ASSERT_EQ(responses.size(), 2);
    EXPECT_EQ(responses[1]["result"]["structuredContent"]["sum"], 2) << responses[1];
    EXPECT_EQ(middleware_calls, 1);
}

TEST_F(ServerHandlersTest, AsyncToolCallReturnsResult) {
    mcp::ServerCapabilities caps;
    mcp::ServerCapabilities::ToolsCapability tools_cap;
    caps.tools = std::move(tools_cap);

    ServerSetup setup(io_ctx_, std::move(caps));

    setup.server.add_tool<AddParams, AddResult>("add_async", "Adds two numbers asynchronously",
                                                nlohmann::json{{"type", "object"}},
                                                [](AddParams params) -> mcp::Task<AddResult> {
                                                    co_return AddResult{params.augend + params.addend};
                                                });

    std::vector<nlohmann::json> responses;
    setup.raw_transport->set_on_write([&responses, &setup](std::string_view msg) {
        responses.push_back(nlohmann::json::parse(msg));
        if (responses.size() == 2) {
            setup.raw_transport->close();
        }
    });

    setup.raw_transport->enqueue_message(make_initialize_request("1").dump());
    setup.raw_transport->enqueue_message(make_initialized_notification().dump());

    nlohmann::json call_req;
    call_req["jsonrpc"] = "2.0";
    call_req["id"] = "2";
    call_req["method"] = "tools/call";
    nlohmann::json call_params;
    call_params["name"] = "add_async";
    call_params["arguments"] = nlohmann::json{{"augend", 10}, {"addend", 20}};
    call_req["params"] = std::move(call_params);
    setup.raw_transport->enqueue_message(call_req.dump());

    mcp_test::spawn_detached(io_ctx_, [&]() -> mcp::Task<void> {
        co_await setup.server.run(setup.transport, io_ctx_.get_executor());
    });

    io_ctx_.run();

    ASSERT_EQ(responses.size(), 2);
    auto& tool_response = responses[1];
    EXPECT_EQ(tool_response["id"], "2");
    ASSERT_TRUE(tool_response.contains("result"));
    EXPECT_EQ(tool_response["result"]["structuredContent"]["sum"], 30);
}

TEST_F(ServerHandlersTest, DomainObjectWithContentFieldIsStillNormalized) {
    mcp::ServerCapabilities caps;
    caps.tools = mcp::ServerCapabilities::ToolsCapability{};
    ServerSetup setup(io_ctx_, std::move(caps));

    setup.server.add_tool<nlohmann::json, nlohmann::json>(
        "domain-content", "Returns a domain object", nlohmann::json{{"type", "object"}},
        [](nlohmann::json) {
            return nlohmann::json{{"content", "not an MCP content array"}, {"answer", 42}};
        });

    auto responses = run_request(
        setup, nlohmann::json{
                   {"jsonrpc", "2.0"},
                   {"id", "2"},
                   {"method", "tools/call"},
                   {"params", {{"name", "domain-content"}, {"arguments", nlohmann::json::object()}}}});

    ASSERT_EQ(responses.size(), 2);
    const auto& result = responses[1]["result"];
    EXPECT_EQ(result["structuredContent"]["content"], "not an MCP content array");
    EXPECT_EQ(result["structuredContent"]["answer"], 42);
    ASSERT_TRUE(result["content"].is_array());
    EXPECT_EQ(result["content"][0]["type"], "text");
}

// A typed handler follows the same rule as a JSON one: a value that is already a CallToolResult is
// sent unchanged.
TEST_F(ServerHandlersTest, TypedHandlerReturningCallToolResultJsonIsNotRewrapped) {
    mcp::ServerCapabilities caps;
    caps.tools = mcp::ServerCapabilities::ToolsCapability{};
    ServerSetup setup(io_ctx_, std::move(caps));

    setup.server.add_tool<nlohmann::json, nlohmann::json>(
        "typed-result", "Returns a ready result", nlohmann::json{{"type", "object"}},
        [](nlohmann::json) { return nlohmann::json(mcp::make_tool_error_result("not permitted")); });

    auto responses = run_request(
        setup, nlohmann::json{
                   {"jsonrpc", "2.0"},
                   {"id", "2"},
                   {"method", "tools/call"},
                   {"params", {{"name", "typed-result"}, {"arguments", nlohmann::json::object()}}}});

    ASSERT_EQ(responses.size(), 2);
    const auto& result = responses[1]["result"];
    EXPECT_TRUE(result["isError"].get<bool>());
    ASSERT_EQ(result["content"].size(), 1);
    EXPECT_EQ(result["content"][0]["text"], "not permitted");
    EXPECT_FALSE(result.contains("structuredContent"));
}

TEST_F(ServerHandlersTest, TypedHandlerExceptionBecomesProtocolToolError) {
    mcp::ServerCapabilities caps;
    caps.tools = mcp::ServerCapabilities::ToolsCapability{};
    ServerSetup setup(io_ctx_, std::move(caps));

    setup.server.add_tool<nlohmann::json, nlohmann::json>(
        "typed-failure", "Always fails", nlohmann::json{{"type", "object"}},
        [](nlohmann::json) -> nlohmann::json { throw std::runtime_error("typed failure"); });

    auto responses = run_request(
        setup, nlohmann::json{
                   {"jsonrpc", "2.0"},
                   {"id", "2"},
                   {"method", "tools/call"},
                   {"params", {{"name", "typed-failure"}, {"arguments", nlohmann::json::object()}}}});

    ASSERT_EQ(responses.size(), 2);
    const auto& result = responses[1]["result"];
    EXPECT_TRUE(result["isError"].get<bool>());
    ASSERT_TRUE(result["content"].is_array());
    EXPECT_EQ(result["content"][0]["type"], "text");
    EXPECT_NE(result["content"][0]["text"].get<std::string>().find("typed failure"), std::string::npos);
    EXPECT_FALSE(result.contains("structuredContent"));
}

TEST_F(ServerHandlersTest, ToolWithContextLogsMessage) {
    mcp::ServerCapabilities caps;
    mcp::ServerCapabilities::ToolsCapability tools_cap;
    caps.tools = std::move(tools_cap);

    ServerSetup setup(io_ctx_, std::move(caps));

    setup.server.add_tool<AddParams, AddResult>(
        "add_with_log", "Adds and logs", nlohmann::json{{"type", "object"}},
        [](mcp::Context& ctx, AddParams params) -> mcp::Task<AddResult> {
            co_await ctx.log_info("computing sum");
            co_return AddResult{params.augend + params.addend};
        });

    std::vector<nlohmann::json> responses;
    setup.raw_transport->set_on_write([&responses, &setup](std::string_view msg) {
        responses.push_back(nlohmann::json::parse(msg));
        // init response + log notification + tool result = 3
        if (responses.size() == 3) {
            setup.raw_transport->close();
        }
    });

    setup.raw_transport->enqueue_message(make_initialize_request("1").dump());
    setup.raw_transport->enqueue_message(make_initialized_notification().dump());

    nlohmann::json call_req;
    call_req["jsonrpc"] = "2.0";
    call_req["id"] = "2";
    call_req["method"] = "tools/call";
    nlohmann::json call_params;
    call_params["name"] = "add_with_log";
    call_params["arguments"] = nlohmann::json{{"augend", 5}, {"addend", 6}};
    call_req["params"] = std::move(call_params);
    setup.raw_transport->enqueue_message(call_req.dump());

    mcp_test::spawn_detached(io_ctx_, [&]() -> mcp::Task<void> {
        co_await setup.server.run(setup.transport, io_ctx_.get_executor());
    });

    io_ctx_.run();

    ASSERT_EQ(responses.size(), 3);

    // Second message is the log notification
    auto& log_notification = responses[1];
    EXPECT_EQ(log_notification["method"], "notifications/message");
    EXPECT_EQ(log_notification["params"]["data"], "computing sum");
    EXPECT_FALSE(log_notification.contains("id"));

    // Third message is the tool result
    auto& tool_response = responses[2];
    EXPECT_EQ(tool_response["id"], "2");
    ASSERT_TRUE(tool_response.contains("result"));
    EXPECT_EQ(tool_response["result"]["structuredContent"]["sum"], 11);
}

TEST_F(ServerHandlersTest, ToolsListReturnsRegisteredTools) {
    mcp::ServerCapabilities caps;
    mcp::ServerCapabilities::ToolsCapability tools_cap;
    caps.tools = std::move(tools_cap);

    ServerSetup setup(io_ctx_, std::move(caps));

    setup.server.add_tool<AddParams, AddResult>(
        "add", "Adds two numbers",
        nlohmann::json{
            {"type", "object"},
            {"properties", {{"augend", {{"type", "integer"}}}, {"addend", {{"type", "integer"}}}}}},
        [](AddParams params) -> AddResult { return AddResult{params.augend + params.addend}; });

    std::vector<nlohmann::json> responses;
    setup.raw_transport->set_on_write([&responses, &setup](std::string_view msg) {
        responses.push_back(nlohmann::json::parse(msg));
        if (responses.size() == 2) {
            setup.raw_transport->close();
        }
    });

    setup.raw_transport->enqueue_message(make_initialize_request("1").dump());
    setup.raw_transport->enqueue_message(make_initialized_notification().dump());

    nlohmann::json list_req;
    list_req["jsonrpc"] = "2.0";
    list_req["id"] = "2";
    list_req["method"] = "tools/list";
    setup.raw_transport->enqueue_message(list_req.dump());

    mcp_test::spawn_detached(io_ctx_, [&]() -> mcp::Task<void> {
        co_await setup.server.run(setup.transport, io_ctx_.get_executor());
    });

    io_ctx_.run();

    ASSERT_EQ(responses.size(), 2);
    auto& list_response = responses[1];
    EXPECT_EQ(list_response["id"], "2");
    ASSERT_TRUE(list_response.contains("result"));
    ASSERT_TRUE(list_response["result"].contains("tools"));
    auto& tools = list_response["result"]["tools"];
    ASSERT_EQ(tools.size(), 1);
    EXPECT_EQ(tools[0]["name"], "add");
    EXPECT_EQ(tools[0]["description"], "Adds two numbers");
}

TEST_F(ServerHandlersTest, ToolsListReturnsToolsInRegistrationOrder) {
    mcp::ServerCapabilities caps;
    mcp::ServerCapabilities::ToolsCapability tools_cap;
    caps.tools = std::move(tools_cap);

    ServerSetup setup(io_ctx_, std::move(caps));

    mcp::Tool schema;
    schema.inputSchema = nlohmann::json{{"type", "object"}};

    for (const auto& name : {"charlie", "alpha", "bravo"}) {
        mcp::Tool tool = schema;
        tool.name = name;
        setup.server.add_raw_tool(tool, [](const nlohmann::json&) -> nlohmann::json {
            return mcp::make_tool_text_result("");
        });
    }

    auto responses =
        run_request(setup, nlohmann::json{{"jsonrpc", "2.0"}, {"id", "2"}, {"method", "tools/list"}});

    ASSERT_EQ(responses.size(), 2);
    auto& tools = responses[1]["result"]["tools"];
    ASSERT_EQ(tools.size(), 3);
    EXPECT_EQ(tools[0]["name"], "charlie");
    EXPECT_EQ(tools[1]["name"], "alpha");
    EXPECT_EQ(tools[2]["name"], "bravo");
}

TEST_F(ServerHandlersTest, ResourcesReadReturnsContent) {
    mcp::ServerCapabilities caps;
    mcp::ServerCapabilities::ResourcesCapability resources_cap;
    caps.resources = std::move(resources_cap);

    ServerSetup setup(io_ctx_, std::move(caps));

    mcp::Resource resource;
    resource.uri = "file:///hello.txt";
    resource.name = "hello.txt";
    resource.description = "A greeting file";

    setup.server.add_resource<mcp::ReadResourceRequestParams, mcp::ReadResourceResult>(
        std::move(resource), [](mcp::ReadResourceRequestParams params) -> mcp::ReadResourceResult {
            mcp::TextResourceContents text_contents;
            text_contents.uri = params.uri;
            text_contents.text = "Hello, World!";
            text_contents.mimeType = "text/plain";

            mcp::ReadResourceResult result;
            result.contents.emplace_back(std::move(text_contents));
            return result;
        });

    std::vector<nlohmann::json> responses;
    setup.raw_transport->set_on_write([&responses, &setup](std::string_view msg) {
        responses.push_back(nlohmann::json::parse(msg));
        if (responses.size() == 2) {
            setup.raw_transport->close();
        }
    });

    setup.raw_transport->enqueue_message(make_initialize_request("1").dump());
    setup.raw_transport->enqueue_message(make_initialized_notification().dump());

    nlohmann::json read_req;
    read_req["jsonrpc"] = "2.0";
    read_req["id"] = "2";
    read_req["method"] = "resources/read";
    read_req["params"] = nlohmann::json{{"uri", "file:///hello.txt"}};
    setup.raw_transport->enqueue_message(read_req.dump());

    mcp_test::spawn_detached(io_ctx_, [&]() -> mcp::Task<void> {
        co_await setup.server.run(setup.transport, io_ctx_.get_executor());
    });

    io_ctx_.run();

    ASSERT_EQ(responses.size(), 2);
    auto& read_response = responses[1];
    EXPECT_EQ(read_response["id"], "2");
    ASSERT_TRUE(read_response.contains("result"));
    ASSERT_TRUE(read_response["result"].contains("contents"));
    auto& contents = read_response["result"]["contents"];
    ASSERT_EQ(contents.size(), 1);
    EXPECT_EQ(contents[0]["uri"], "file:///hello.txt");
    EXPECT_EQ(contents[0]["text"], "Hello, World!");
    EXPECT_EQ(contents[0]["mimeType"], "text/plain");
}

TEST_F(ServerHandlersTest, ResourcesListReturnsRegisteredResources) {
    mcp::ServerCapabilities caps;
    mcp::ServerCapabilities::ResourcesCapability resources_cap;
    caps.resources = std::move(resources_cap);

    ServerSetup setup(io_ctx_, std::move(caps));

    mcp::Resource resource;
    resource.uri = "file:///test.txt";
    resource.name = "test.txt";
    resource.description = "A test file";

    setup.server.add_resource<mcp::ReadResourceRequestParams, mcp::ReadResourceResult>(
        std::move(resource), [](mcp::ReadResourceRequestParams /*params*/) -> mcp::ReadResourceResult {
            return mcp::ReadResourceResult{};
        });

    std::vector<nlohmann::json> responses;
    setup.raw_transport->set_on_write([&responses, &setup](std::string_view msg) {
        responses.push_back(nlohmann::json::parse(msg));
        if (responses.size() == 2) {
            setup.raw_transport->close();
        }
    });

    setup.raw_transport->enqueue_message(make_initialize_request("1").dump());
    setup.raw_transport->enqueue_message(make_initialized_notification().dump());

    nlohmann::json list_req;
    list_req["jsonrpc"] = "2.0";
    list_req["id"] = "2";
    list_req["method"] = "resources/list";
    setup.raw_transport->enqueue_message(list_req.dump());

    mcp_test::spawn_detached(io_ctx_, [&]() -> mcp::Task<void> {
        co_await setup.server.run(setup.transport, io_ctx_.get_executor());
    });

    io_ctx_.run();

    ASSERT_EQ(responses.size(), 2);
    auto& list_response = responses[1];
    ASSERT_TRUE(list_response.contains("result"));
    auto& resources = list_response["result"]["resources"];
    ASSERT_EQ(resources.size(), 1);
    EXPECT_EQ(resources[0]["uri"], "file:///test.txt");
    EXPECT_EQ(resources[0]["name"], "test.txt");
}

TEST_F(ServerHandlersTest, ResourceTemplatesListReturnsRegisteredTemplates) {
    mcp::ServerCapabilities caps;
    mcp::ServerCapabilities::ResourcesCapability resources_cap;
    caps.resources = std::move(resources_cap);

    ServerSetup setup(io_ctx_, std::move(caps));

    mcp::ResourceTemplate tmpl;
    tmpl.uriTemplate = "file:///{path}";
    tmpl.name = "file-access";
    tmpl.description = "Access files by path";

    setup.server.add_resource_template(std::move(tmpl));

    std::vector<nlohmann::json> responses;
    setup.raw_transport->set_on_write([&responses, &setup](std::string_view msg) {
        responses.push_back(nlohmann::json::parse(msg));
        if (responses.size() == 2) {
            setup.raw_transport->close();
        }
    });

    setup.raw_transport->enqueue_message(make_initialize_request("1").dump());
    setup.raw_transport->enqueue_message(make_initialized_notification().dump());

    nlohmann::json list_req;
    list_req["jsonrpc"] = "2.0";
    list_req["id"] = "2";
    list_req["method"] = "resources/templates/list";
    setup.raw_transport->enqueue_message(list_req.dump());

    mcp_test::spawn_detached(io_ctx_, [&]() -> mcp::Task<void> {
        co_await setup.server.run(setup.transport, io_ctx_.get_executor());
    });

    io_ctx_.run();

    ASSERT_EQ(responses.size(), 2);
    auto& list_response = responses[1];
    ASSERT_TRUE(list_response.contains("result"));
    auto& templates = list_response["result"]["resourceTemplates"];
    ASSERT_EQ(templates.size(), 1);
    EXPECT_EQ(templates[0]["uriTemplate"], "file:///{path}");
    EXPECT_EQ(templates[0]["name"], "file-access");
}

TEST_F(ServerHandlersTest, ResourceTemplateHandlerServesMatchingUri) {
    mcp::ServerCapabilities caps;
    caps.resources = mcp::ServerCapabilities::ResourcesCapability{};
    ServerSetup setup(io_ctx_, std::move(caps));

    mcp::ResourceTemplate tmpl;
    tmpl.uriTemplate = "file:///{name}";
    tmpl.name = "files";
    setup.server.add_resource_template<mcp::ReadResourceRequestParams, mcp::ReadResourceResult>(
        tmpl, [](mcp::ReadResourceRequestParams params) -> mcp::ReadResourceResult {
            mcp::TextResourceContents content;
            content.uri = params.uri;
            content.text = "template content";
            mcp::ReadResourceResult result;
            result.contents.emplace_back(std::move(content));
            return result;
        });

    auto responses = run_request(setup, nlohmann::json{{"jsonrpc", "2.0"},
                                                       {"id", "2"},
                                                       {"method", "resources/read"},
                                                       {"params", {{"uri", "file:///notes.txt"}}}});

    ASSERT_EQ(responses.size(), 2);
    EXPECT_EQ(responses[1]["result"]["contents"][0]["uri"], "file:///notes.txt");
    EXPECT_EQ(responses[1]["result"]["contents"][0]["text"], "template content");
}

TEST_F(ServerHandlersTest, ExactResourceTakesPrecedenceOverTemplate) {
    mcp::ServerCapabilities caps;
    caps.resources = mcp::ServerCapabilities::ResourcesCapability{};
    ServerSetup setup(io_ctx_, std::move(caps));

    mcp::ResourceTemplate tmpl;
    tmpl.uriTemplate = "file:///{name}";
    tmpl.name = "files";
    setup.server.add_resource_template<mcp::ReadResourceRequestParams, mcp::ReadResourceResult>(
        tmpl, [](mcp::ReadResourceRequestParams params) -> mcp::ReadResourceResult {
            mcp::TextResourceContents content;
            content.uri = params.uri;
            content.text = "template";
            mcp::ReadResourceResult result;
            result.contents.emplace_back(std::move(content));
            return result;
        });

    mcp::Resource exact;
    exact.uri = "file:///exact.txt";
    exact.name = "exact";
    setup.server.add_resource<mcp::ReadResourceRequestParams, mcp::ReadResourceResult>(
        exact, [](mcp::ReadResourceRequestParams params) -> mcp::ReadResourceResult {
            mcp::TextResourceContents content;
            content.uri = params.uri;
            content.text = "exact";
            mcp::ReadResourceResult result;
            result.contents.emplace_back(std::move(content));
            return result;
        });

    auto responses = run_request(setup, nlohmann::json{{"jsonrpc", "2.0"},
                                                       {"id", "2"},
                                                       {"method", "resources/read"},
                                                       {"params", {{"uri", "file:///exact.txt"}}}});

    ASSERT_EQ(responses.size(), 2);
    EXPECT_EQ(responses[1]["result"]["contents"][0]["text"], "exact");
}

TEST_F(ServerHandlersTest, ResourceTemplateRegistrationRejectsDuplicateAndEquivalentPatterns) {
    mcp::ServerCapabilities caps;
    caps.resources = mcp::ServerCapabilities::ResourcesCapability{};
    ServerSetup setup(io_ctx_, std::move(caps));

    mcp::ResourceTemplate first;
    first.uriTemplate = "file:///{path}";
    first.name = "first";
    setup.server.add_resource_template(first);

    auto duplicate = first;
    duplicate.name = "duplicate";
    EXPECT_THROW(setup.server.add_resource_template(duplicate), std::invalid_argument);

    mcp::ResourceTemplate equivalent;
    equivalent.uriTemplate = "file:///{name}";
    equivalent.name = "equivalent";
    EXPECT_THROW(setup.server.add_resource_template(equivalent), std::invalid_argument);

    mcp::ResourceTemplate malformed;
    malformed.uriTemplate = "file:///{path";
    malformed.name = "malformed";
    EXPECT_THROW(setup.server.add_resource_template(malformed), std::invalid_argument);
}

TEST_F(ServerHandlersTest, AmbiguousResourceTemplateMatchReturnsInvalidParams) {
    mcp::ServerCapabilities caps;
    caps.resources = mcp::ServerCapabilities::ResourcesCapability{};
    ServerSetup setup(io_ctx_, std::move(caps));

    auto register_template = [&setup](std::string uri_template, std::string name) {
        mcp::ResourceTemplate tmpl;
        tmpl.uriTemplate = std::move(uri_template);
        tmpl.name = std::move(name);
        setup.server.add_resource_template<mcp::ReadResourceRequestParams, mcp::ReadResourceResult>(
            tmpl, [](mcp::ReadResourceRequestParams) { return mcp::ReadResourceResult{}; });
    };
    register_template("file:///{+path}", "all-files");
    register_template("file:///fixed/{name}", "fixed-files");

    auto responses =
        run_request(setup, nlohmann::json{{"jsonrpc", "2.0"},
                                          {"id", "2"},
                                          {"method", "resources/read"},
                                          {"params", {{"uri", "file:///fixed/item.txt"}}}});

    ASSERT_EQ(responses.size(), 2);
    EXPECT_EQ(responses[1]["error"]["code"], mcp::constants::jsonrpc::g_INVALID_PARAMS);
    EXPECT_NE(responses[1]["error"]["message"].get<std::string>().find("Ambiguous"), std::string::npos);
}

TEST_F(ServerHandlersTest, MetadataOnlyTemplateDoesNotBlockHandledTemplate) {
    mcp::ServerCapabilities caps;
    caps.resources = mcp::ServerCapabilities::ResourcesCapability{};
    ServerSetup setup(io_ctx_, std::move(caps));

    mcp::ResourceTemplate metadata_only;
    metadata_only.uriTemplate = "file:///{+path}";
    metadata_only.name = "all-files";
    setup.server.add_resource_template(metadata_only);

    mcp::ResourceTemplate handled;
    handled.uriTemplate = "file:///fixed/{name}";
    handled.name = "fixed-files";
    setup.server.add_resource_template<mcp::ReadResourceRequestParams, mcp::ReadResourceResult>(
        handled, [](mcp::ReadResourceRequestParams params) {
            mcp::TextResourceContents content;
            content.uri = params.uri;
            content.text = "handled";
            mcp::ReadResourceResult result;
            result.contents.emplace_back(std::move(content));
            return result;
        });

    auto responses =
        run_request(setup, nlohmann::json{{"jsonrpc", "2.0"},
                                          {"id", "2"},
                                          {"method", "resources/read"},
                                          {"params", {{"uri", "file:///fixed/item.txt"}}}});

    ASSERT_EQ(responses.size(), 2);
    EXPECT_EQ(responses[1]["result"]["contents"][0]["text"], "handled");
}

// 2025-11-25 and earlier name a resource that does not exist with -32002, and its URI in data.
TEST_F(ServerHandlersTest, UnknownResourceReturnsResourceNotFound) {
    mcp::ServerCapabilities caps;
    caps.resources = mcp::ServerCapabilities::ResourcesCapability{};
    ServerSetup setup(io_ctx_, std::move(caps));

    auto responses =
        run_request(setup, nlohmann::json{{"jsonrpc", "2.0"},
                                          {"id", "2"},
                                          {"method", "resources/read"},
                                          {"params", {{"uri", "file:///does-not-exist.txt"}}}});

    ASSERT_EQ(responses.size(), 2);
    EXPECT_EQ(responses[1]["error"]["code"], mcp::constants::jsonrpc::g_RESOURCE_NOT_FOUND);
    EXPECT_EQ(responses[1]["error"]["data"], nlohmann::json({{"uri", "file:///does-not-exist.txt"}}));
}

TEST_F(ServerHandlersTest, UnknownResourceTemplateReturnsResourceNotFound) {
    mcp::ServerCapabilities caps;
    caps.resources = mcp::ServerCapabilities::ResourcesCapability{};
    ServerSetup setup(io_ctx_, std::move(caps));

    mcp::ResourceTemplate tmpl;
    tmpl.uriTemplate = "file:///docs/{name}";
    tmpl.name = "docs";
    setup.server.add_resource_template<mcp::ReadResourceRequestParams, mcp::ReadResourceResult>(
        tmpl, [](mcp::ReadResourceRequestParams) { return mcp::ReadResourceResult{}; });

    auto responses =
        run_request(setup, nlohmann::json{{"jsonrpc", "2.0"},
                                          {"id", "2"},
                                          {"method", "resources/read"},
                                          {"params", {{"uri", "file:///other/item.txt"}}}});

    ASSERT_EQ(responses.size(), 2);
    EXPECT_EQ(responses[1]["error"]["code"], mcp::constants::jsonrpc::g_RESOURCE_NOT_FOUND);
    EXPECT_EQ(responses[1]["error"]["data"], nlohmann::json({{"uri", "file:///other/item.txt"}}));
}

TEST_F(ServerHandlersTest, OverlongUriIsRejectedWithoutReachingTemplateMatching) {
    mcp::ServerCapabilities caps;
    caps.resources = mcp::ServerCapabilities::ResourcesCapability{};
    ServerSetup setup(io_ctx_, std::move(caps));

    mcp::ResourceTemplate tmpl;
    tmpl.uriTemplate = "file:///{+path}";
    tmpl.name = "files";
    setup.server.add_resource_template<mcp::ReadResourceRequestParams, mcp::ReadResourceResult>(
        tmpl, [](mcp::ReadResourceRequestParams) { return mcp::ReadResourceResult{}; });

    const std::string uri = "file:///" + std::string(64 * 1024, 'a');
    auto responses = run_request(
        setup,
        nlohmann::json{
            {"jsonrpc", "2.0"}, {"id", "2"}, {"method", "resources/read"}, {"params", {{"uri", uri}}}});

    ASSERT_EQ(responses.size(), 2);
    ASSERT_TRUE(responses[1].contains("error"));
    EXPECT_EQ(responses[1]["error"]["code"], mcp::constants::jsonrpc::g_INVALID_PARAMS);
    // Says the URI was too long, rather than that no such resource exists, and does not echo it.
    const auto message = responses[1]["error"]["message"].get<std::string>();
    EXPECT_NE(message.find("limit"), std::string::npos);
    EXPECT_EQ(message.find("Unknown resource"), std::string::npos);
    EXPECT_LT(message.size(), uri.size());
}

// A URI at the accepted limit is served, so the bound admits what it claims to admit. The match is
// run on a std::thread, which carries the platform's default stack rather than gtest's main thread's.
TEST_F(ServerHandlersTest, UriAtTheLengthLimitIsStillMatchedByTemplate) {
    mcp::ServerCapabilities caps;
    caps.resources = mcp::ServerCapabilities::ResourcesCapability{};
    ServerSetup setup(io_ctx_, std::move(caps));

    mcp::ResourceTemplate tmpl;
    tmpl.uriTemplate = "file:///{+path}";
    tmpl.name = "files";
    setup.server.add_resource_template<mcp::ReadResourceRequestParams, mcp::ReadResourceResult>(
        tmpl, [](mcp::ReadResourceRequestParams params) {
            mcp::TextResourceContents content;
            content.uri = params.uri;
            content.text = "matched";
            mcp::ReadResourceResult result;
            result.contents.emplace_back(std::move(content));
            return result;
        });

    constexpr std::size_t limit = 512;
    const std::string prefix = "file:///";
    const std::string uri = prefix + std::string(limit - prefix.size(), 'a');
    ASSERT_EQ(uri.size(), limit);

    std::vector<nlohmann::json> responses;
    setup.raw_transport->set_on_write([&responses, &setup](std::string_view message) {
        responses.push_back(nlohmann::json::parse(message));
        if (responses.size() == 2) {
            setup.raw_transport->close();
        }
    });
    setup.raw_transport->enqueue_message(make_initialize_request("1").dump());
    setup.raw_transport->enqueue_message(make_initialized_notification().dump());
    setup.raw_transport->enqueue_message(nlohmann::json{
        {"jsonrpc", "2.0"},
        {"id", "2"},
        {"method", "resources/read"},
        {"params", {{"uri", uri}}}}.dump());

    mcp_test::spawn_detached(io_ctx_, [&]() -> mcp::Task<void> {
        co_await setup.server.run(setup.transport, io_ctx_.get_executor());
    });

    std::thread pump([this]() { io_ctx_.run(); });
    pump.join();

    ASSERT_EQ(responses.size(), 2);
    ASSERT_TRUE(responses[1].contains("result"));
    EXPECT_EQ(responses[1]["result"]["contents"][0]["text"], "matched");
}

// Each expression operator matches exactly the URIs its expansion can produce: the cases below
// pin the matcher to the regular expressions the templates are documented to compile to.
TEST_F(ServerHandlersTest, TemplateExpressionsMatchWhatTheirOperatorExpandsTo) {
    mcp::ServerCapabilities caps;
    caps.resources = mcp::ServerCapabilities::ResourcesCapability{};
    ServerSetup setup(io_ctx_, std::move(caps));

    struct Case {
        const char* uri;
        bool matches;
    };
    const std::vector<std::pair<std::string, std::vector<Case>>> templates = {
        {"a://{x}", {{"a://abc", true}, {"a://", false}, {"a://ab/c", false}, {"a://a?b", false}}},
        {"b://{+path}", {{"b://x/y/z", true}, {"b://x?y", false}, {"b://", false}}},
        {"c://item{#frag}",
         {{"c://item", true}, {"c://item#top", true}, {"c://item#a#b", false}, {"c://itemx", false}}},
        {"d://file{.ext}",
         {{"d://file", true},
          {"d://file.tar.gz", true},
          {"d://file..a", true},
          {"d://file.", false},
          {"d://file.a/b", false}}},
        {"e://root{/seg}",
         {{"e://root", true}, {"e://root/a/b", true}, {"e://root/", false}, {"e://root/a?x", false}}},
        {"f://matrix{;params}",
         {{"f://matrix", true},
          {"f://matrix;", true},
          {"f://matrix;a=1;b", true},
          {"f://matrix;a?x", false}}},
        {"g://search{?q}",
         {{"g://search", true},
          {"g://search?", true},
          {"g://search?q=a&b=c", true},
          {"g://search?q#x", false}}},
        {"h://list?a=1{&b}",
         {{"h://list?a=1", true}, {"h://list?a=1&b=2", true}, {"h://list?a=1#x", false}}},
        {"i://a.b{x}", {{"i://a.bz", true}, {"i://aXbz", false}}},
    };

    std::vector<std::pair<std::string, const Case*>> requests;
    for (const auto& [uri_template, cases] : templates) {
        mcp::ResourceTemplate tmpl;
        tmpl.uriTemplate = uri_template;
        tmpl.name = uri_template;
        setup.server.add_resource_template<mcp::ReadResourceRequestParams, mcp::ReadResourceResult>(
            tmpl, [name = uri_template](mcp::ReadResourceRequestParams params) {
                mcp::TextResourceContents content;
                content.uri = params.uri;
                content.text = name;
                mcp::ReadResourceResult result;
                result.contents.emplace_back(std::move(content));
                return result;
            });
        for (const auto& test_case : cases) {
            requests.emplace_back(uri_template, &test_case);
        }
    }

    std::vector<nlohmann::json> responses;
    setup.raw_transport->set_on_write([&](std::string_view message) {
        responses.push_back(nlohmann::json::parse(message));
        if (responses.size() == requests.size() + 1) {
            setup.raw_transport->close();
        }
    });
    setup.raw_transport->enqueue_message(make_initialize_request("init").dump());
    setup.raw_transport->enqueue_message(make_initialized_notification().dump());
    for (std::size_t index = 0; index < requests.size(); ++index) {
        setup.raw_transport->enqueue_message(
            nlohmann::json{{"jsonrpc", "2.0"},
                           {"id", static_cast<std::int64_t>(index)},
                           {"method", "resources/read"},
                           {"params", {{"uri", requests[index].second->uri}}}}
                .dump());
    }
    mcp_test::spawn_detached(io_ctx_, [&]() -> mcp::Task<void> {
        co_await setup.server.run(setup.transport, io_ctx_.get_executor());
    });
    io_ctx_.run();

    ASSERT_EQ(responses.size(), requests.size() + 1);
    for (const auto& response : responses) {
        if (!response["id"].is_number_integer()) {
            continue;
        }
        const auto& [uri_template, test_case] = requests.at(response["id"].get<std::size_t>());
        SCOPED_TRACE(uri_template + " against " + test_case->uri);
        if (test_case->matches) {
            ASSERT_TRUE(response.contains("result")) << response.dump();
            EXPECT_EQ(response["result"]["contents"][0]["text"], uri_template);
        } else {
            ASSERT_TRUE(response.contains("error")) << response.dump();
            EXPECT_EQ(response["error"]["code"], mcp::constants::jsonrpc::g_RESOURCE_NOT_FOUND);
        }
    }
}

// Expressions separated by a literal their values may also contain, here '-', leave the matcher
// many ways to split a URI. One that almost matches must still be answered at once rather than
// after trying every split.
TEST_F(ServerHandlersTest, ANearlyMatchingUriIsAnsweredInBoundedTime) {
    mcp::ServerCapabilities caps;
    caps.resources = mcp::ServerCapabilities::ResourcesCapability{};
    ServerSetup setup(io_ctx_, std::move(caps));

    mcp::ResourceTemplate tmpl;
    tmpl.uriTemplate = "metrics://{year}-{month}-{day}-{hour}";
    tmpl.name = "metrics";
    setup.server.add_resource_template<mcp::ReadResourceRequestParams, mcp::ReadResourceResult>(
        tmpl, [](mcp::ReadResourceRequestParams) { return mcp::ReadResourceResult{}; });

    std::string uri = "metrics://";
    uri += std::string(512 - uri.size() - 1, '-');
    uri += '/';
    ASSERT_EQ(uri.size(), 512U);

    const auto started = std::chrono::steady_clock::now();
    auto responses = run_request(
        setup,
        nlohmann::json{
            {"jsonrpc", "2.0"}, {"id", "2"}, {"method", "resources/read"}, {"params", {{"uri", uri}}}});
    const auto elapsed = std::chrono::steady_clock::now() - started;

    ASSERT_EQ(responses.size(), 2);
    EXPECT_EQ(responses[1]["error"]["code"], mcp::constants::jsonrpc::g_RESOURCE_NOT_FOUND);
    EXPECT_LT(elapsed, std::chrono::seconds(1));
}

TEST_F(ServerHandlersTest, ExactResourceIsServedRegardlessOfUriLength) {
    mcp::ServerCapabilities caps;
    caps.resources = mcp::ServerCapabilities::ResourcesCapability{};
    ServerSetup setup(io_ctx_, std::move(caps));

    mcp::Resource exact;
    exact.uri = "file:///" + std::string(64 * 1024, 'a');
    exact.name = "long";
    setup.server.add_resource<mcp::ReadResourceRequestParams, mcp::ReadResourceResult>(
        exact, [](mcp::ReadResourceRequestParams params) {
            mcp::TextResourceContents content;
            content.uri = params.uri;
            content.text = "exact";
            mcp::ReadResourceResult result;
            result.contents.emplace_back(std::move(content));
            return result;
        });

    auto responses = run_request(setup, nlohmann::json{{"jsonrpc", "2.0"},
                                                       {"id", "2"},
                                                       {"method", "resources/read"},
                                                       {"params", {{"uri", exact.uri}}}});

    ASSERT_EQ(responses.size(), 2);
    EXPECT_EQ(responses[1]["result"]["contents"][0]["text"], "exact");
}

TEST_F(ServerHandlersTest, AdjacentTemplateExpressionsAreRejectedAtRegistration) {
    mcp::ServerCapabilities caps;
    caps.resources = mcp::ServerCapabilities::ResourcesCapability{};
    ServerSetup setup(io_ctx_, std::move(caps));

    mcp::ResourceTemplate adjacent;
    adjacent.uriTemplate = "file:///{dir}{name}";
    adjacent.name = "adjacent";
    EXPECT_THROW(setup.server.add_resource_template(adjacent), std::invalid_argument);

    mcp::ResourceTemplate separated;
    separated.uriTemplate = "file:///{dir}/{name}";
    separated.name = "separated";
    EXPECT_NO_THROW(setup.server.add_resource_template(separated));

    // Ambiguous: the second expression has no leading literal, or its literal can occur inside
    // the first expression's value.
    for (const char* uri_template : {"file:///{dir}{+name}", "file:///{name}{.ext}",
                                     "file:///{+path}{/segment}", "file:///{?q}{&r}"}) {
        SCOPED_TRACE(uri_template);
        mcp::ResourceTemplate ambiguous;
        ambiguous.uriTemplate = uri_template;
        ambiguous.name = uri_template;
        EXPECT_THROW(setup.server.add_resource_template(ambiguous), std::invalid_argument);
    }
}

TEST_F(ServerHandlersTest, AdjacentExpressionsWithADecidableBoundaryAreAccepted) {
    mcp::ServerCapabilities caps;
    caps.resources = mcp::ServerCapabilities::ResourcesCapability{};
    ServerSetup setup(io_ctx_, std::move(caps));

    // The second expression starts with a character the first can never contain.
    for (const char* uri_template : {"file:///{+path}{?format}", "users://{id}{#section}",
                                     "users://{id}{/detail}", "file:///{+path}{#fragment}"}) {
        SCOPED_TRACE(uri_template);
        mcp::ResourceTemplate decidable;
        decidable.uriTemplate = uri_template;
        decidable.name = uri_template;
        EXPECT_NO_THROW(setup.server.add_resource_template(decidable));
    }
}

TEST_F(ServerHandlersTest, DuplicateNamedRegistrationsAreRejected) {
    ServerSetup setup(io_ctx_, mcp::ServerCapabilities{});

    auto tool_handler = [](nlohmann::json) { return nlohmann::json::object(); };
    setup.server.add_tool<nlohmann::json, nlohmann::json>(
        "duplicate", "first", nlohmann::json{{"type", "object"}}, tool_handler);
    EXPECT_THROW((setup.server.add_tool<nlohmann::json, nlohmann::json>(
                     "duplicate", "second", nlohmann::json{{"type", "object"}}, tool_handler)),
                 std::invalid_argument);

    mcp::Resource resource;
    resource.uri = "file:///duplicate";
    resource.name = "first";
    auto resource_handler = [](mcp::ReadResourceRequestParams) { return mcp::ReadResourceResult{}; };
    setup.server.add_resource<mcp::ReadResourceRequestParams, mcp::ReadResourceResult>(
        resource, resource_handler);
    resource.name = "second";
    EXPECT_THROW((setup.server.add_resource<mcp::ReadResourceRequestParams, mcp::ReadResourceResult>(
                     resource, resource_handler)),
                 std::invalid_argument);

    mcp::Prompt prompt;
    prompt.name = "duplicate";
    auto prompt_handler = [](mcp::GetPromptRequestParams) { return mcp::GetPromptResult{}; };
    setup.server.add_prompt<mcp::GetPromptRequestParams, mcp::GetPromptResult>(prompt, prompt_handler);
    prompt.description = "second";
    EXPECT_THROW((setup.server.add_prompt<mcp::GetPromptRequestParams, mcp::GetPromptResult>(
                     prompt, prompt_handler)),
                 std::invalid_argument);
}

// The prompts spec lists "Invalid prompt name" under -32602 (Invalid params): the method exists,
// the name it was given does not.
TEST_F(ServerHandlersTest, UnknownPromptReturnsInvalidParams) {
    mcp::ServerCapabilities caps;
    caps.prompts = mcp::ServerCapabilities::PromptsCapability{};
    ServerSetup setup(io_ctx_, std::move(caps));

    nlohmann::json get_req;
    get_req["jsonrpc"] = "2.0";
    get_req["id"] = "2";
    get_req["method"] = "prompts/get";
    get_req["params"] = nlohmann::json{{"name", "nonexistent"}};

    auto responses = run_request(setup, std::move(get_req));

    ASSERT_EQ(responses.size(), 2);
    auto& error_response = responses[1];
    EXPECT_EQ(error_response["id"], "2");
    ASSERT_TRUE(error_response.contains("error")) << "response: " << error_response.dump();
    EXPECT_EQ(error_response["error"]["code"], mcp::constants::jsonrpc::g_INVALID_PARAMS);
    EXPECT_NE(error_response["error"]["message"].get<std::string>().find("nonexistent"),
              std::string::npos);
}

TEST_F(ServerHandlersTest, PromptsGetReturnsPromptMessages) {
    mcp::ServerCapabilities caps;
    mcp::ServerCapabilities::PromptsCapability prompts_cap;
    caps.prompts = std::move(prompts_cap);

    ServerSetup setup(io_ctx_, std::move(caps));

    mcp::Prompt prompt;
    prompt.name = "greeting";
    prompt.description = "A greeting prompt";
    mcp::PromptArgument arg;
    arg.name = "name";
    arg.description = "The name to greet";
    prompt.arguments = std::vector<mcp::PromptArgument>{std::move(arg)};

    setup.server.add_prompt<mcp::GetPromptRequestParams, mcp::GetPromptResult>(
        std::move(prompt), [](mcp::GetPromptRequestParams params) -> mcp::GetPromptResult {
            std::string name_val = "World";
            if (params.arguments) {
                auto iter = params.arguments->find("name");
                if (iter != params.arguments->end()) {
                    name_val = iter->second;
                }
            }

            mcp::TextContent text;
            text.text = "Hello, " + name_val + "!";

            mcp::PromptMessage msg;
            msg.role = mcp::Role::eUser;
            msg.content = std::move(text);

            mcp::GetPromptResult result;
            result.description = "A personalized greeting";
            result.messages.push_back(std::move(msg));
            return result;
        });

    std::vector<nlohmann::json> responses;
    setup.raw_transport->set_on_write([&responses, &setup](std::string_view msg) {
        responses.push_back(nlohmann::json::parse(msg));
        if (responses.size() == 2) {
            setup.raw_transport->close();
        }
    });

    setup.raw_transport->enqueue_message(make_initialize_request("1").dump());
    setup.raw_transport->enqueue_message(make_initialized_notification().dump());

    nlohmann::json get_req;
    get_req["jsonrpc"] = "2.0";
    get_req["id"] = "2";
    get_req["method"] = "prompts/get";
    get_req["params"] = nlohmann::json{{"name", "greeting"}, {"arguments", {{"name", "Alice"}}}};
    setup.raw_transport->enqueue_message(get_req.dump());

    mcp_test::spawn_detached(io_ctx_, [&]() -> mcp::Task<void> {
        co_await setup.server.run(setup.transport, io_ctx_.get_executor());
    });

    io_ctx_.run();

    ASSERT_EQ(responses.size(), 2);
    auto& get_response = responses[1];
    EXPECT_EQ(get_response["id"], "2");
    ASSERT_TRUE(get_response.contains("result"));
    EXPECT_EQ(get_response["result"]["description"], "A personalized greeting");
    auto& messages = get_response["result"]["messages"];
    ASSERT_EQ(messages.size(), 1);
    EXPECT_EQ(messages[0]["role"], "user");
    EXPECT_EQ(messages[0]["content"]["text"], "Hello, Alice!");
}

TEST_F(ServerHandlersTest, PromptsListReturnsRegisteredPrompts) {
    mcp::ServerCapabilities caps;
    mcp::ServerCapabilities::PromptsCapability prompts_cap;
    caps.prompts = std::move(prompts_cap);

    ServerSetup setup(io_ctx_, std::move(caps));

    mcp::Prompt prompt;
    prompt.name = "test-prompt";
    prompt.description = "A test prompt";

    setup.server.add_prompt<mcp::GetPromptRequestParams, mcp::GetPromptResult>(
        std::move(prompt), [](mcp::GetPromptRequestParams /*params*/) -> mcp::GetPromptResult {
            return mcp::GetPromptResult{};
        });

    std::vector<nlohmann::json> responses;
    setup.raw_transport->set_on_write([&responses, &setup](std::string_view msg) {
        responses.push_back(nlohmann::json::parse(msg));
        if (responses.size() == 2) {
            setup.raw_transport->close();
        }
    });

    setup.raw_transport->enqueue_message(make_initialize_request("1").dump());
    setup.raw_transport->enqueue_message(make_initialized_notification().dump());

    nlohmann::json list_req;
    list_req["jsonrpc"] = "2.0";
    list_req["id"] = "2";
    list_req["method"] = "prompts/list";
    setup.raw_transport->enqueue_message(list_req.dump());

    mcp_test::spawn_detached(io_ctx_, [&]() -> mcp::Task<void> {
        co_await setup.server.run(setup.transport, io_ctx_.get_executor());
    });

    io_ctx_.run();

    ASSERT_EQ(responses.size(), 2);
    auto& list_response = responses[1];
    ASSERT_TRUE(list_response.contains("result"));
    auto& prompts = list_response["result"]["prompts"];
    ASSERT_EQ(prompts.size(), 1);
    EXPECT_EQ(prompts[0]["name"], "test-prompt");
    EXPECT_EQ(prompts[0]["description"], "A test prompt");
}

TEST_F(ServerHandlersTest, UnknownToolReturnsError) {
    mcp::ServerCapabilities caps;
    mcp::ServerCapabilities::ToolsCapability tools_cap;
    caps.tools = std::move(tools_cap);

    ServerSetup setup(io_ctx_, std::move(caps));

    std::vector<nlohmann::json> responses;
    setup.raw_transport->set_on_write([&responses, &setup](std::string_view msg) {
        responses.push_back(nlohmann::json::parse(msg));
        if (responses.size() == 2) {
            setup.raw_transport->close();
        }
    });

    setup.raw_transport->enqueue_message(make_initialize_request("1").dump());
    setup.raw_transport->enqueue_message(make_initialized_notification().dump());

    nlohmann::json call_req;
    call_req["jsonrpc"] = "2.0";
    call_req["id"] = "2";
    call_req["method"] = "tools/call";
    nlohmann::json call_params;
    call_params["name"] = "nonexistent";
    call_params["arguments"] = nlohmann::json::object();
    call_req["params"] = std::move(call_params);
    setup.raw_transport->enqueue_message(call_req.dump());

    mcp_test::spawn_detached(io_ctx_, [&]() -> mcp::Task<void> {
        co_await setup.server.run(setup.transport, io_ctx_.get_executor());
    });

    io_ctx_.run();

    ASSERT_EQ(responses.size(), 2);
    auto& error_response = responses[1];
    EXPECT_EQ(error_response["id"], "2");
    ASSERT_TRUE(error_response.contains("error"));
    EXPECT_EQ(error_response["error"]["code"], mcp::constants::jsonrpc::g_INVALID_PARAMS);
    EXPECT_TRUE(error_response["error"]["message"].get<std::string>().find("nonexistent") !=
                std::string::npos);
}

// --- Pagination Tests ---

TEST_F(ServerHandlersTest, ToolsListPagesThroughTheToolsByCursor) {
    mcp::ServerCapabilities caps;
    mcp::ServerCapabilities::ToolsCapability tools_cap;
    caps.tools = std::move(tools_cap);

    ServerSetup setup(io_ctx_, std::move(caps));

    setup.server.set_page_size(2);

    for (int i = 0; i < 5; ++i) {
        auto name = "tool_" + std::to_string(i);
        setup.server.add_tool<AddParams, AddResult>(
            name, "Tool " + std::to_string(i), nlohmann::json{{"type", "object"}},
            [](AddParams p) -> AddResult { return AddResult{p.augend + p.addend}; });
    }

    std::map<std::string, nlohmann::json> responses;
    setup.raw_transport->set_on_write([&responses, &setup](std::string_view msg) {
        auto response = nlohmann::json::parse(msg);
        const auto id = response.at("id").get<std::string>();
        responses[id] = std::move(response);
        if (responses.size() == 5) {
            setup.raw_transport->close();
        }
    });

    setup.raw_transport->enqueue_message(make_initialize_request("1").dump());
    setup.raw_transport->enqueue_message(make_initialized_notification().dump());
    const auto list = [&setup](const std::string& id, std::optional<nlohmann::json> params) {
        nlohmann::json list_req;
        list_req["jsonrpc"] = "2.0";
        list_req["id"] = id;
        list_req["method"] = "tools/list";
        if (params) {
            list_req["params"] = std::move(*params);
        }
        setup.raw_transport->enqueue_message(list_req.dump());
    };
    list("first", std::nullopt);
    list("middle", nlohmann::json{{"cursor", "2"}});
    list("last", nlohmann::json{{"cursor", "4"}});
    // Only reachable with a page size set and a non-empty collection: paginate() returns the
    // whole slice before it ever looks at the cursor when either is absent.
    list("null", nlohmann::json{{"cursor", nullptr}});

    mcp_test::spawn_detached(io_ctx_, [&]() -> mcp::Task<void> {
        co_await setup.server.run(setup.transport, io_ctx_.get_executor());
    });

    io_ctx_.run();

    ASSERT_EQ(responses.size(), 5);
    const auto& first = responses["first"]["result"];
    ASSERT_EQ(first["tools"].size(), 2);
    EXPECT_EQ(first["tools"][0]["name"], "tool_0");
    EXPECT_EQ(first["tools"][1]["name"], "tool_1");
    ASSERT_TRUE(first.contains("nextCursor"));
    EXPECT_EQ(first["nextCursor"], "2");

    const auto& middle = responses["middle"]["result"];
    ASSERT_EQ(middle["tools"].size(), 2);
    EXPECT_EQ(middle["tools"][0]["name"], "tool_2");
    EXPECT_EQ(middle["tools"][1]["name"], "tool_3");
    EXPECT_EQ(middle["nextCursor"], "4");

    const auto& last = responses["last"]["result"];
    ASSERT_EQ(last["tools"].size(), 1);
    EXPECT_EQ(last["tools"][0]["name"], "tool_4");
    EXPECT_FALSE(last.contains("nextCursor"));

    const auto& null_cursor = responses["null"];
    ASSERT_FALSE(null_cursor.contains("error")) << "actual error: " << null_cursor["error"].dump();
    ASSERT_EQ(null_cursor["result"]["tools"].size(), 2);
    EXPECT_EQ(null_cursor["result"]["tools"][0]["name"], "tool_0");
    ASSERT_TRUE(null_cursor["result"].contains("nextCursor"));
    EXPECT_EQ(null_cursor["result"]["nextCursor"], "2");
}

TEST_F(ServerHandlersTest, ToolsListResultIsTheListToolsResultOfTheRegisteredTools) {
    mcp::ServerCapabilities caps;
    caps.tools = mcp::ServerCapabilities::ToolsCapability{};
    ServerSetup setup(io_ctx_, std::move(caps));
    setup.server.set_page_size(2);

    mcp::Tool typed;
    typed.name = "add";
    typed.description = "Adds two numbers";
    typed.inputSchema = nlohmann::json{{"type", "object"}, {"required", {"augend", "addend"}}};
    typed.outputSchema =
        nlohmann::json{{"type", "object"}, {"properties", {{"sum", {{"type", "integer"}}}}}};
    setup.server.add_tool<AddParams, AddResult>(
        typed.name, *typed.description, typed.inputSchema, *typed.outputSchema,
        [](AddParams p) -> AddResult { return AddResult{p.augend + p.addend}; });

    mcp::Tool raw;
    raw.name = "raw";
    raw.title = "Raw tool";
    raw.inputSchema = nlohmann::json{{"type", "object"}};
    raw.annotations = mcp::ToolAnnotations{};
    raw.annotations->readOnlyHint = true;
    setup.server.add_raw_tool(
        raw, [](const nlohmann::json&) -> nlohmann::json { return mcp::make_tool_text_result("raw"); });

    setup.server.add_tool<AddParams, AddResult>(
        "third", "Not on the first page", nlohmann::json{{"type", "object"}},
        [](AddParams p) -> AddResult { return AddResult{p.augend + p.addend}; });

    std::vector<nlohmann::json> responses;
    setup.raw_transport->set_on_write([&responses, &setup](std::string_view msg) {
        responses.push_back(nlohmann::json::parse(msg));
        if (responses.size() == 2) {
            setup.raw_transport->close();
        }
    });

    setup.raw_transport->enqueue_message(make_initialize_request("1").dump());
    setup.raw_transport->enqueue_message(make_initialized_notification().dump());
    setup.raw_transport->enqueue_message(
        nlohmann::json{{"jsonrpc", "2.0"}, {"id", "2"}, {"method", "tools/list"}}.dump());

    mcp_test::spawn_detached(io_ctx_, [&]() -> mcp::Task<void> {
        co_await setup.server.run(setup.transport, io_ctx_.get_executor());
    });

    io_ctx_.run();

    mcp::ListToolsResult expected;
    expected.tools = {typed, raw};
    expected.nextCursor = "2";
    ASSERT_EQ(responses.size(), 2);
    EXPECT_EQ(responses[1],
              (nlohmann::json{{"jsonrpc", "2.0"}, {"id", "2"}, {"result", nlohmann::json(expected)}}));
}

TEST_F(ServerHandlersTest, PaginationDisabledByDefault) {
    mcp::ServerCapabilities caps;
    mcp::ServerCapabilities::ToolsCapability tools_cap;
    caps.tools = std::move(tools_cap);

    ServerSetup setup(io_ctx_, std::move(caps));

    for (int i = 0; i < 5; ++i) {
        auto name = "tool_" + std::to_string(i);
        setup.server.add_tool<AddParams, AddResult>(
            name, "Tool " + std::to_string(i), nlohmann::json{{"type", "object"}},
            [](AddParams p) -> AddResult { return AddResult{p.augend + p.addend}; });
    }

    std::vector<nlohmann::json> responses;
    setup.raw_transport->set_on_write([&responses, &setup](std::string_view msg) {
        responses.push_back(nlohmann::json::parse(msg));
        if (responses.size() == 2) {
            setup.raw_transport->close();
        }
    });

    setup.raw_transport->enqueue_message(make_initialize_request("1").dump());
    setup.raw_transport->enqueue_message(make_initialized_notification().dump());

    nlohmann::json list_req;
    list_req["jsonrpc"] = "2.0";
    list_req["id"] = "2";
    list_req["method"] = "tools/list";
    setup.raw_transport->enqueue_message(list_req.dump());

    mcp_test::spawn_detached(io_ctx_, [&]() -> mcp::Task<void> {
        co_await setup.server.run(setup.transport, io_ctx_.get_executor());
    });

    io_ctx_.run();

    ASSERT_EQ(responses.size(), 2);
    auto& result = responses[1]["result"];
    EXPECT_EQ(result["tools"].size(), 5);
    EXPECT_FALSE(result.contains("nextCursor"));
}

TEST_F(ServerHandlersTest, ResourcesListPagination) {
    mcp::ServerCapabilities caps;
    mcp::ServerCapabilities::ResourcesCapability resources_cap;
    caps.resources = std::move(resources_cap);

    ServerSetup setup(io_ctx_, std::move(caps));

    setup.server.set_page_size(1);

    for (int i = 0; i < 3; ++i) {
        mcp::Resource resource;
        resource.uri = "file:///r" + std::to_string(i) + ".txt";
        resource.name = "r" + std::to_string(i);

        setup.server.add_resource<mcp::ReadResourceRequestParams, mcp::ReadResourceResult>(
            std::move(resource),
            [](mcp::ReadResourceRequestParams /*params*/) -> mcp::ReadResourceResult {
                return mcp::ReadResourceResult{};
            });
    }

    std::vector<nlohmann::json> responses;
    setup.raw_transport->set_on_write([&responses, &setup](std::string_view msg) {
        responses.push_back(nlohmann::json::parse(msg));
        if (responses.size() == 2) {
            setup.raw_transport->close();
        }
    });

    setup.raw_transport->enqueue_message(make_initialize_request("1").dump());
    setup.raw_transport->enqueue_message(make_initialized_notification().dump());

    nlohmann::json list_req;
    list_req["jsonrpc"] = "2.0";
    list_req["id"] = "2";
    list_req["method"] = "resources/list";
    setup.raw_transport->enqueue_message(list_req.dump());

    mcp_test::spawn_detached(io_ctx_, [&]() -> mcp::Task<void> {
        co_await setup.server.run(setup.transport, io_ctx_.get_executor());
    });

    io_ctx_.run();

    ASSERT_EQ(responses.size(), 2);
    auto& result = responses[1]["result"];
    ASSERT_EQ(result["resources"].size(), 1);
    EXPECT_EQ(result["resources"][0]["name"], "r0");
    EXPECT_EQ(result["nextCursor"], "1");
}

// --- OutputSchema Tests ---

TEST_F(ServerHandlersTest, ToolWithOutputSchemaIncludesStructuredContent) {
    mcp::ServerCapabilities caps;
    mcp::ServerCapabilities::ToolsCapability tools_cap;
    caps.tools = std::move(tools_cap);

    ServerSetup setup(io_ctx_, std::move(caps));

    nlohmann::json output_schema = {{"type", "object"},
                                    {"properties", {{"sum", {{"type", "integer"}}}}}};

    setup.server.add_tool<AddParams, AddResult>(
        "add_structured", "Adds with output schema",
        nlohmann::json{
            {"type", "object"},
            {"properties", {{"augend", {{"type", "integer"}}}, {"addend", {{"type", "integer"}}}}}},
        output_schema,
        [](AddParams params) -> AddResult { return AddResult{params.augend + params.addend}; });

    std::vector<nlohmann::json> responses;
    setup.raw_transport->set_on_write([&responses, &setup](std::string_view msg) {
        responses.push_back(nlohmann::json::parse(msg));
        if (responses.size() == 2) {
            setup.raw_transport->close();
        }
    });

    setup.raw_transport->enqueue_message(make_initialize_request("1").dump());
    setup.raw_transport->enqueue_message(make_initialized_notification().dump());

    nlohmann::json call_req;
    call_req["jsonrpc"] = "2.0";
    call_req["id"] = "2";
    call_req["method"] = "tools/call";
    call_req["params"] =
        nlohmann::json{{"name", "add_structured"}, {"arguments", {{"augend", 10}, {"addend", 20}}}};
    setup.raw_transport->enqueue_message(call_req.dump());

    mcp_test::spawn_detached(io_ctx_, [&]() -> mcp::Task<void> {
        co_await setup.server.run(setup.transport, io_ctx_.get_executor());
    });

    io_ctx_.run();

    ASSERT_EQ(responses.size(), 2);
    auto& result = responses[1]["result"];
    ASSERT_TRUE(result.contains("structuredContent"));
    EXPECT_EQ(result["structuredContent"]["sum"], 30);
    EXPECT_EQ(result["content"][0]["text"], R"({"sum":30})");
}

TEST_F(ServerHandlersTest, ToolWithOutputSchemaCanReturnAnErrorWithoutStructuredContent) {
    mcp::ServerCapabilities caps;
    caps.tools = mcp::ServerCapabilities::ToolsCapability{};
    ServerSetup setup(io_ctx_, std::move(caps));

    setup.server.add_tool<nlohmann::json, nlohmann::json>(
        "failing_structured", "Fails before producing structured output",
        nlohmann::json{{"type", "object"}}, nlohmann::json{{"type", "object"}},
        [](const nlohmann::json&) -> nlohmann::json {
            throw std::runtime_error("structured tool failed");
        });

    auto responses = run_request(
        setup,
        nlohmann::json{
            {"jsonrpc", "2.0"},
            {"id", "2"},
            {"method", "tools/call"},
            {"params", {{"name", "failing_structured"}, {"arguments", nlohmann::json::object()}}}});

    ASSERT_EQ(responses.size(), 2);
    const auto& result = responses[1]["result"];
    EXPECT_TRUE(result["isError"].get<bool>());
    EXPECT_FALSE(result.contains("structuredContent"));
    EXPECT_EQ(result["content"][0]["text"], "structured tool failed");
}

TEST_F(ServerHandlersTest, ToolWithoutOutputSchemaStillReturnsStructuredContent) {
    mcp::ServerCapabilities caps;
    mcp::ServerCapabilities::ToolsCapability tools_cap;
    caps.tools = std::move(tools_cap);

    ServerSetup setup(io_ctx_, std::move(caps));

    setup.server.add_tool<AddParams, AddResult>(
        "add_plain", "Adds without output schema", nlohmann::json{{"type", "object"}},
        [](AddParams params) -> AddResult { return AddResult{params.augend + params.addend}; });

    std::vector<nlohmann::json> responses;
    setup.raw_transport->set_on_write([&responses, &setup](std::string_view msg) {
        responses.push_back(nlohmann::json::parse(msg));
        if (responses.size() == 2) {
            setup.raw_transport->close();
        }
    });

    setup.raw_transport->enqueue_message(make_initialize_request("1").dump());
    setup.raw_transport->enqueue_message(make_initialized_notification().dump());

    nlohmann::json call_req;
    call_req["jsonrpc"] = "2.0";
    call_req["id"] = "2";
    call_req["method"] = "tools/call";
    call_req["params"] =
        nlohmann::json{{"name", "add_plain"}, {"arguments", {{"augend", 5}, {"addend", 3}}}};
    setup.raw_transport->enqueue_message(call_req.dump());

    mcp_test::spawn_detached(io_ctx_, [&]() -> mcp::Task<void> {
        co_await setup.server.run(setup.transport, io_ctx_.get_executor());
    });

    io_ctx_.run();

    ASSERT_EQ(responses.size(), 2);
    auto& result = responses[1]["result"];
    EXPECT_EQ(result["structuredContent"]["sum"], 8);
    EXPECT_EQ(result["content"][0]["type"], "text");
}

// --- Completion Tests ---

TEST_F(ServerHandlersTest, CompletionReturnsResults) {
    mcp::ServerCapabilities caps;
    caps.completions = nlohmann::json::object();

    ServerSetup setup(io_ctx_, std::move(caps));

    setup.server.set_completion_provider(
        [](const mcp::CompleteParams& params) -> mcp::Task<mcp::CompleteResult> {
            mcp::CompleteResult result;
            result.completion.values = {"hello", "help", "hero"};
            result.completion.total = 3;
            result.completion.hasMore = false;
            co_return result;
        });

    std::vector<nlohmann::json> responses;
    setup.raw_transport->set_on_write([&responses, &setup](std::string_view msg) {
        responses.push_back(nlohmann::json::parse(msg));
        if (responses.size() == 2) {
            setup.raw_transport->close();
        }
    });

    setup.raw_transport->enqueue_message(make_initialize_request("1").dump());
    setup.raw_transport->enqueue_message(make_initialized_notification().dump());

    nlohmann::json complete_req;
    complete_req["jsonrpc"] = "2.0";
    complete_req["id"] = "2";
    complete_req["method"] = "completion/complete";
    complete_req["params"] = nlohmann::json{{"ref", {{"type", "ref/prompt"}, {"name", "my-prompt"}}},
                                            {"argument", {{"name", "arg1"}, {"value", "hel"}}}};
    setup.raw_transport->enqueue_message(complete_req.dump());

    mcp_test::spawn_detached(io_ctx_, [&]() -> mcp::Task<void> {
        co_await setup.server.run(setup.transport, io_ctx_.get_executor());
    });

    io_ctx_.run();

    ASSERT_EQ(responses.size(), 2);
    auto& result = responses[1]["result"];
    ASSERT_TRUE(result.contains("completion"));
    auto& completion = result["completion"];
    ASSERT_EQ(completion["values"].size(), 3);
    EXPECT_EQ(completion["values"][0], "hello");
    EXPECT_EQ(completion["values"][1], "help");
    EXPECT_EQ(completion["values"][2], "hero");
    EXPECT_EQ(completion["total"], 3);
    EXPECT_FALSE(completion["hasMore"].get<bool>());
}

TEST_F(ServerHandlersTest, CompletionWithoutHandlerReturnsError) {
    mcp::ServerCapabilities caps;

    ServerSetup setup(io_ctx_, std::move(caps));

    std::vector<nlohmann::json> responses;
    setup.raw_transport->set_on_write([&responses, &setup](std::string_view msg) {
        responses.push_back(nlohmann::json::parse(msg));
        if (responses.size() == 2) {
            setup.raw_transport->close();
        }
    });

    setup.raw_transport->enqueue_message(make_initialize_request("1").dump());
    setup.raw_transport->enqueue_message(make_initialized_notification().dump());

    nlohmann::json complete_req;
    complete_req["jsonrpc"] = "2.0";
    complete_req["id"] = "2";
    complete_req["method"] = "completion/complete";
    complete_req["params"] = nlohmann::json{{"ref", {{"type", "ref/prompt"}, {"name", "test"}}},
                                            {"argument", {{"name", "arg"}, {"value", "val"}}}};
    setup.raw_transport->enqueue_message(complete_req.dump());

    mcp_test::spawn_detached(io_ctx_, [&]() -> mcp::Task<void> {
        co_await setup.server.run(setup.transport, io_ctx_.get_executor());
    });

    io_ctx_.run();

    ASSERT_EQ(responses.size(), 2);
    auto& error_response = responses[1];
    ASSERT_TRUE(error_response.contains("error"));
    EXPECT_EQ(error_response["error"]["code"], mcp::constants::jsonrpc::g_METHOD_NOT_FOUND);
}

// A tool name is chosen entirely by the client, and an unknown one is the ordinary error path of
// any `tools/call`. The name reaches a diagnostic that the server operator reads, so a name
// carrying CR/LF forges a line in whatever that diagnostic lands in, and a bidi override reorders
// the rest of it. Same defect as the peer-controlled text in the auth diagnostics, opposite
// direction: here the untrusted side is the client.
TEST_F(ServerHandlersTest, UnknownToolDiagnosticFlattensClientChosenName) {
    mcp::ServerCapabilities caps;
    caps.tools = mcp::ServerCapabilities::ToolsCapability{};
    ServerSetup setup(io_ctx_, std::move(caps));

    // "zqtripwire" is a token no other code path produces; see the tripwire assertions below.
    // \xe2\x80\xae is U+202E RIGHT-TO-LEFT OVERRIDE, written escaped so this source file does not
    // itself contain a bidi override.
    const std::string forged_name =
        "zqtripwire\r\n2026-09-20 ERROR forged line from the client\xe2\x80\xae reordered tail";

    nlohmann::json call_req;
    call_req["jsonrpc"] = "2.0";
    call_req["id"] = "2";
    call_req["method"] = "tools/call";
    call_req["params"] = nlohmann::json{{"name", forged_name}, {"arguments", nlohmann::json::object()}};

    auto responses = run_request(setup, std::move(call_req));

    ASSERT_EQ(responses.size(), 2);
    auto& error_response = responses[1];
    ASSERT_TRUE(error_response.contains("error")) << "response: " << error_response.dump();
    const auto message = error_response["error"]["message"].get<std::string>();

    // Tripwire. The payload must actually have reached the "Unknown tool" site rather than being
    // rejected earlier by params validation or routed to some other diagnostic that flattens its
    // own message; either would make the assertions below pass for a reason unrelated to the site
    // under test. The code pins WHICH of the two "Unknown tool" sites this is: the RPC path refuses
    // in handle_tools_call_wire with mcp::constants::jsonrpc::g_INVALID_PARAMS, whereas the invoke_tool
    // path throws and surfaces as mcp::constants::jsonrpc::g_INTERNAL_ERROR through the dispatcher.
    // Dump the message on failure so a vacuous pass cannot hide.
    ASSERT_EQ(error_response["error"]["code"], mcp::constants::jsonrpc::g_INVALID_PARAMS)
        << "response: " << error_response.dump();
    ASSERT_EQ(message.rfind("Unknown tool: ", 0), 0U) << "actual message: " << message;
    ASSERT_NE(message.find("zqtripwire"), std::string::npos) << "actual message: " << message;

    EXPECT_EQ(message.find('\r'), std::string::npos) << "actual message: " << message;
    EXPECT_EQ(message.find('\n'), std::string::npos) << "actual message: " << message;
    EXPECT_EQ(message.find("\xe2\x80\xae"), std::string::npos) << "actual message: " << message;
}

// The same site must also bound the name, so a client cannot flood the operator's log through a
// megabyte-long tool name.
TEST_F(ServerHandlersTest, UnknownToolDiagnosticBoundsClientChosenName) {
    mcp::ServerCapabilities caps;
    caps.tools = mcp::ServerCapabilities::ToolsCapability{};
    ServerSetup setup(io_ctx_, std::move(caps));

    const std::string forged_name = "zqtripwire" + std::string(64 * 1024, 'A');

    nlohmann::json call_req;
    call_req["jsonrpc"] = "2.0";
    call_req["id"] = "2";
    call_req["method"] = "tools/call";
    call_req["params"] = nlohmann::json{{"name", forged_name}, {"arguments", nlohmann::json::object()}};

    auto responses = run_request(setup, std::move(call_req));

    ASSERT_EQ(responses.size(), 2);
    auto& error_response = responses[1];
    ASSERT_TRUE(error_response.contains("error")) << "response: " << error_response.dump();
    const auto message = error_response["error"]["message"].get<std::string>();

    ASSERT_EQ(error_response["error"]["code"], mcp::constants::jsonrpc::g_INVALID_PARAMS)
        << "actual message prefix: " << message.substr(0, 64);
    ASSERT_EQ(message.rfind("Unknown tool: ", 0), 0U)
        << "actual message prefix: " << message.substr(0, 64);
    ASSERT_NE(message.find("zqtripwire"), std::string::npos)
        << "actual message prefix: " << message.substr(0, 64);
    EXPECT_LT(message.size(), forged_name.size()) << "message size: " << message.size();
    EXPECT_LE(message.size(), std::size_t{512}) << "message size: " << message.size();
}

// The second "Unknown tool" site. `invoke_tool` bypasses the JSON-RPC loop for json_only
// deployments, so the name it is given is whatever the embedding passes in -- peer-chosen text in
// exactly the deployments that use this entry point. It is a distinct site from the one the RPC
// path takes, and it throws rather than building an error frame, so it needs its own test.
TEST_F(ServerHandlersTest, InvokeToolUnknownNameDiagnosticIsFlattened) {
    mcp::ServerCapabilities caps;
    caps.tools = mcp::ServerCapabilities::ToolsCapability{};
    ServerSetup setup(io_ctx_, std::move(caps));

    const std::string forged_name =
        "zqtripwire\r\n2026-09-20 ERROR forged line from the caller\xe2\x80\xae reordered tail";

    std::string message;
    bool threw = false;
    mcp_test::spawn_detached(io_ctx_, [&]() -> mcp::Task<void> {
        try {
            static_cast<void>(co_await setup.server.invoke_tool(forged_name, nlohmann::json::object()));
        } catch (const std::exception& error) {
            threw = true;
            message = error.what();
        }
    });
    io_ctx_.run();

    // Tripwire: the name must have reached the invoke_tool site itself, not some earlier refusal.
    ASSERT_TRUE(threw) << "invoke_tool accepted an unknown tool name";
    ASSERT_EQ(message.rfind("Unknown tool: ", 0), 0U) << "actual message: " << message;
    ASSERT_NE(message.find("zqtripwire"), std::string::npos) << "actual message: " << message;

    EXPECT_EQ(message.find('\r'), std::string::npos) << "actual message: " << message;
    EXPECT_EQ(message.find('\n'), std::string::npos) << "actual message: " << message;
    EXPECT_EQ(message.find("\xe2\x80\xae"), std::string::npos) << "actual message: " << message;
}

// --- Explicit-null request members ---
//
// An absent JSON member and one serialized as explicit `null` are the same statement on the
// wire. Both requests below are messages a conforming peer may send; neither may be rejected.

TEST_F(ServerHandlersTest, PromptsGetAcceptsExplicitNullArguments) {
    mcp::ServerCapabilities caps;
    mcp::ServerCapabilities::PromptsCapability prompts_cap;
    caps.prompts = std::move(prompts_cap);

    ServerSetup setup(io_ctx_, std::move(caps));

    mcp::Prompt prompt;
    prompt.name = "greeting";
    setup.server.add_prompt<mcp::GetPromptRequestParams, mcp::GetPromptResult>(
        std::move(prompt), [](mcp::GetPromptRequestParams params) -> mcp::GetPromptResult {
            EXPECT_FALSE(params.arguments.has_value());

            mcp::TextContent text;
            text.text = "Hello, World!";

            mcp::PromptMessage msg;
            msg.role = mcp::Role::eUser;
            msg.content = std::move(text);

            mcp::GetPromptResult result;
            result.messages.push_back(std::move(msg));
            return result;
        });

    std::vector<nlohmann::json> responses;
    setup.raw_transport->set_on_write([&responses, &setup](std::string_view msg) {
        responses.push_back(nlohmann::json::parse(msg));
        if (responses.size() == 2) {
            setup.raw_transport->close();
        }
    });

    setup.raw_transport->enqueue_message(make_initialize_request("1").dump());
    setup.raw_transport->enqueue_message(make_initialized_notification().dump());

    nlohmann::json get_req;
    get_req["jsonrpc"] = "2.0";
    get_req["id"] = "2";
    get_req["method"] = "prompts/get";
    get_req["params"] = nlohmann::json{{"name", "greeting"}, {"arguments", nullptr}};
    setup.raw_transport->enqueue_message(get_req.dump());

    mcp_test::spawn_detached(io_ctx_, [&]() -> mcp::Task<void> {
        co_await setup.server.run(setup.transport, io_ctx_.get_executor());
    });

    io_ctx_.run();

    ASSERT_EQ(responses.size(), 2);
    auto& get_response = responses[1];
    EXPECT_EQ(get_response["id"], "2");
    ASSERT_FALSE(get_response.contains("error")) << "actual error: " << get_response["error"].dump();
    ASSERT_TRUE(get_response.contains("result"));
    auto& messages = get_response["result"]["messages"];
    ASSERT_EQ(messages.size(), 1);
    EXPECT_EQ(messages[0]["content"]["text"], "Hello, World!");
}

TEST_F(ServerHandlersTest, InitializeAcceptsExplicitNullClientInfoOptionals) {
    // clientInfo is an Implementation, so this breaks the handshake on a peer's first message.
    ServerSetup setup(io_ctx_, mcp::ServerCapabilities{});

    std::vector<nlohmann::json> responses;
    setup.raw_transport->set_on_write([&responses, &setup](std::string_view msg) {
        responses.push_back(nlohmann::json::parse(msg));
        setup.raw_transport->close();
    });

    auto init_req = make_initialize_request("1");
    init_req["params"]["clientInfo"]["title"] = nullptr;
    init_req["params"]["clientInfo"]["description"] = nullptr;
    init_req["params"]["clientInfo"]["websiteUrl"] = nullptr;
    init_req["params"]["clientInfo"]["icons"] = nullptr;
    setup.raw_transport->enqueue_message(init_req.dump());

    mcp_test::spawn_detached(io_ctx_, [&]() -> mcp::Task<void> {
        co_await setup.server.run(setup.transport, io_ctx_.get_executor());
    });

    io_ctx_.run();

    ASSERT_EQ(responses.size(), 1);
    auto& init_response = responses[0];
    ASSERT_FALSE(init_response.contains("error")) << "actual error: " << init_response["error"].dump();
    ASSERT_TRUE(init_response.contains("result"));
    EXPECT_EQ(init_response["result"]["protocolVersion"],
              std::string(protocol::g_LATEST_LEGACY_PROTOCOL_VERSION));
}

TEST_F(ServerHandlersTest, InitializeAcceptsExplicitNullCapabilityFlags) {
    ServerSetup setup(io_ctx_, mcp::ServerCapabilities{});

    std::vector<nlohmann::json> responses;
    setup.raw_transport->set_on_write([&responses, &setup](std::string_view msg) {
        responses.push_back(nlohmann::json::parse(msg));
        setup.raw_transport->close();
    });

    auto init_req = make_initialize_request("1");
    init_req["params"]["capabilities"]["roots"] = {{"listChanged", nullptr}};
    init_req["params"]["capabilities"]["extensions"] = nullptr;
    setup.raw_transport->enqueue_message(init_req.dump());

    mcp_test::spawn_detached(io_ctx_, [&]() -> mcp::Task<void> {
        co_await setup.server.run(setup.transport, io_ctx_.get_executor());
    });

    io_ctx_.run();

    ASSERT_EQ(responses.size(), 1);
    auto& init_response = responses[0];
    ASSERT_FALSE(init_response.contains("error")) << "actual error: " << init_response["error"].dump();
    ASSERT_TRUE(init_response.contains("result"));
}

TEST_F(ServerHandlersTest, MessageNestedBeyondTheDepthLimitIsAParseError) {
    ServerSetup setup(io_ctx_, mcp::ServerCapabilities{});

    std::vector<nlohmann::json> responses;
    setup.raw_transport->set_on_write([&responses, &setup](std::string_view msg) {
        responses.push_back(nlohmann::json::parse(msg));
        if (responses.size() == 2) {
            setup.raw_transport->close();
        }
    });

    // A ping whose whole document nests `depth` levels: the envelope and params are two of them.
    const auto ping_nested_to = [](const std::string& id, std::size_t depth) {
        const std::size_t arrays = depth - 2;
        return R"({"jsonrpc":"2.0","id":")" + id + R"(","method":"ping","params":{"data":)" +
               std::string(arrays, '[') + std::string(arrays, ']') + "}}";
    };
    setup.raw_transport->enqueue_message(ping_nested_to("at-limit", 128));
    setup.raw_transport->enqueue_message(ping_nested_to("over-limit", 129));

    mcp_test::spawn_detached(io_ctx_, [&]() -> mcp::Task<void> {
        co_await setup.server.run(setup.transport, io_ctx_.get_executor());
    });

    io_ctx_.run();

    ASSERT_EQ(responses.size(), 2U);
    const auto with_id = [&responses](const nlohmann::json& id) {
        const auto found =
            std::find_if(responses.begin(), responses.end(), [&id](const nlohmann::json& r) {
                return r.contains("id") ? r.at("id") == id : id.is_null();
            });
        return found == responses.end() ? nlohmann::json() : *found;
    };
    const auto answered = with_id("at-limit");
    ASSERT_TRUE(answered.is_object()) << "the ping at the limit got no response";
    EXPECT_TRUE(answered.contains("result")) << answered.dump();
    // The id of a message that was never decoded is unknown, so the error carries none.
    const auto refused = with_id(nullptr);
    ASSERT_TRUE(refused.is_object()) << "the ping over the limit got no parse error";
    EXPECT_EQ(refused.at("error").at("code"), mcp::constants::jsonrpc::g_PARSE_ERROR);
}

}  // namespace server_handlers_test

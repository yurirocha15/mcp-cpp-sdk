#include "../support/hang_guard.hpp"
#include "../test_utils.hpp"

#include "../support/spawn.hpp"
#include "mcp/client/client.hpp"
#include "mcp/core/async.hpp"
#include "mcp/server/server.hpp"
#include "mcp/transport/memory.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <chrono>
#include <functional>
#include <mcp/core/constants.hpp>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace client_tasks_test {

namespace {

namespace asio = boost::asio;
using nlohmann::json;

mcp::Task<bool> wait_until(std::function<bool()> condition) {
    auto executor = co_await asio::this_coro::executor;
    const auto give_up = std::chrono::steady_clock::now() + g_hang_guard;
    while (!condition() && std::chrono::steady_clock::now() < give_up) {
        asio::steady_timer timer(executor, std::chrono::milliseconds(1));
        co_await timer.async_wait(asio::use_awaitable);
    }
    co_return condition();
}

json form_elicitation(const std::string& message) {
    mcp::ElicitRequestFormParams params;
    params.message = message;
    params.requestedSchema = {{"type", "object"}, {"properties", {{"name", {{"type", "string"}}}}}};
    return params;
}

/// A real server on a memory transport, and a client of it.
class ClientTasks : public ::testing::Test {
   protected:
    asio::io_context io_;
    std::unique_ptr<mcp::Server> server_;
    std::unique_ptr<mcp::Client> client_;
    std::shared_ptr<mcp::InMemoryTaskStore> store_ = std::make_shared<mcp::InMemoryTaskStore>();
    std::atomic<bool> release_{false};
    std::atomic<bool> started_{false};
    std::atomic<int> elicitations_{0};
    std::vector<json> asked_;

    void SetUp() override {
        mcp::ServerCapabilities caps;
        caps.tools = mcp::ServerCapabilities::ToolsCapability{};
        server_ = std::make_unique<mcp::Server>(mcp::Implementation{"tasks", "1.0"}, caps);
        server_->add_tool<json, json>(
            "echo", "Echoes", json{{"type", "object"}},
            [](json args) -> mcp::Task<json> { co_return json{{"echo", std::move(args)}}; });
        server_->add_tool<json, json>("gated", "Runs until released", json{{"type", "object"}},
                                      [this](json /*args*/) -> mcp::Task<json> {
                                          started_ = true;
                                          co_await wait_until([this] { return release_.load(); });
                                          co_return json{{"released", true}};
                                      });
        server_->add_tool<json, json>("cancellable", "Runs until cancelled", json{{"type", "object"}},
                                      [](mcp::Context& ctx, json /*args*/) -> mcp::Task<json> {
                                          co_await wait_until([&ctx] { return ctx.is_cancelled(); });
                                          co_return json{{"cancelled", true}};
                                      });
        server_->add_tool<json, json>(
            "broken_output", "Returns what its output schema forbids", json{{"type", "object"}},
            json{{"type", "object"},
                 {"properties", {{"x", {{"type", "integer"}}}}},
                 {"required", {"x"}}},
            [](json /*args*/) -> mcp::Task<json> { co_return json{{"x", "text"}}; });
        server_->add_tool<json, json>(
            "ask", "Asks for a name", json{{"type", "object"}},
            [](mcp::Context& ctx, json /*args*/) -> mcp::Task<json> {
                const auto answer = co_await ctx.elicit(form_elicitation("Your name?"));
                co_return json{{"name", answer.content.value_or(json::object()).value("name", "")}};
            });
        server_->add_tool<json, json>(
            "never", "Never a task", json{{"type", "object"}},
            [](json /*args*/) -> mcp::Task<json> { co_return json{{"inline", true}}; });
        mcp::TaskOptions options;
        options.store = store_;
        options.poll_interval = std::chrono::milliseconds(5);
        options.warning_handler = [](const std::string&) {};
        server_->enable_tasks(options);
        server_->set_handler_executor(io_.get_executor());
        for (const auto* name : {"echo", "gated", "cancellable", "broken_output", "ask"}) {
            server_->set_tool_task_mode(name, mcp::ToolTaskMode::eAuto);
        }
    }

    void connect(mcp::ClientOptions options, bool modern = true) {
        auto [client_transport, server_transport] =
            mcp::create_memory_transport_pair(io_.get_executor());
        if (modern) {
            options.protocol = mcp::ClientProtocol::pinned("2026-07-28");
        } else {
            options.protocol = mcp::ClientProtocol::legacy();
        }
        client_ = std::make_unique<mcp::Client>(client_transport, io_.get_executor(), options);
        client_->on_elicitation(
            [this](const mcp::ElicitRequestParams&) -> mcp::Task<mcp::ElicitResult> {
                ++elicitations_;
                mcp::ElicitResult result;
                result.action = mcp::ElicitAction::eAccept;
                result.content = json{{"name", "Luca"}};
                co_return result;
            });
        mcp_test::spawn_detached(io_, [this, server_transport]() -> mcp::Task<void> {
            try {
                co_await server_->run(server_transport, io_.get_executor());
            } catch (const std::exception&) {
            }
        });
    }

    template <typename Body>
    void run(Body body) {
        asio::steady_timer guard(io_, g_hang_guard);
        bool fired = false;
        guard.async_wait([this, &fired](const boost::system::error_code& error) {
            if (!error) {
                fired = true;
                io_.stop();
            }
        });
        mcp_test::spawn_detached(io_, [this, &body, &guard]() -> mcp::Task<void> {
            try {
                co_await client_->connect("tasks-client", "1.0");
                co_await body();
            } catch (const std::exception& error) {
                ADD_FAILURE() << "unexpected exception: " << error.what();
            }
            client_->close();
            guard.cancel();
            io_.stop();
        });
        io_.run();
        EXPECT_FALSE(fired) << "the test did not finish";
    }
};

// ---- following ---------------------------------------------------------------------------------

TEST_F(ClientTasks, CallToolFollowsATaskToItsResult) {
    mcp::ClientOptions options;
    options.tasks = mcp::ClientTaskMode::eAuto;
    connect(options);
    std::optional<mcp::CallToolResult> result;
    run([&]() -> mcp::Task<void> {
        const json arguments = {{"a", 1}};
        result = co_await client_->call_tool("echo", arguments);
    });
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->structuredContent->at("echo").at("a"), 1);
    // The server ran it as a task: one is held, and the client got its outcome.
    EXPECT_EQ(store_->size(), 1u);
}

TEST_F(ClientTasks, TheClientDoesNotDeclareTheExtensionByDefault) {
    connect(mcp::ClientOptions{});
    std::optional<mcp::CallToolResult> result;
    run([&]() -> mcp::Task<void> {
        const json arguments = json::object();
        result = co_await client_->call_tool("echo", arguments);
    });
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(store_->size(), 0u);
}

TEST_F(ClientTasks, AToolThatIsNotATaskReturnsItsResultWhileFollowing) {
    mcp::ClientOptions options;
    options.tasks = mcp::ClientTaskMode::eAuto;
    connect(options);
    std::optional<mcp::CallToolResult> result;
    run([&]() -> mcp::Task<void> {
        const json arguments = json::object();
        result = co_await client_->call_tool("never", arguments);
    });
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->structuredContent->at("inline"), true);
    EXPECT_EQ(store_->size(), 0u);
}

TEST_F(ClientTasks, AFailedTaskThrowsTheErrorItFailedWith) {
    mcp::ClientOptions options;
    options.tasks = mcp::ClientTaskMode::eAuto;
    connect(options);
    int code = 0;
    run([&]() -> mcp::Task<void> {
        try {
            const json arguments = json::object();
            co_await client_->call_tool("broken_output", arguments);
        } catch (const mcp::McpError& error) {
            code = error.code();
        }
    });
    EXPECT_EQ(code, mcp::constants::jsonrpc::g_INTERNAL_ERROR);
}

TEST_F(ClientTasks, ATaskThatAsksForInputIsAnsweredByTheRegisteredHandler) {
    mcp::ClientOptions options;
    options.tasks = mcp::ClientTaskMode::eAuto;
    options.task_poll_interval = std::chrono::milliseconds(2);
    connect(options);
    std::optional<mcp::CallToolResult> result;
    run([&]() -> mcp::Task<void> {
        const json arguments = json::object();
        result = co_await client_->call_tool("ask", arguments);
    });
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->structuredContent->at("name"), "Luca");
    EXPECT_EQ(elicitations_.load(), 1);
}

TEST_F(ClientTasks, ALegacySessionIgnoresTheOption) {
    mcp::ClientOptions options;
    options.tasks = mcp::ClientTaskMode::eAuto;
    connect(options, false);
    std::optional<mcp::CallToolResult> result;
    run([&]() -> mcp::Task<void> {
        const json arguments = json::object();
        result = co_await client_->call_tool("echo", arguments);
    });
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(store_->size(), 0u);
}

// ---- handles -----------------------------------------------------------------------------------

TEST_F(ClientTasks, StartToolReturnsAHandleTheApplicationDrives) {
    connect(mcp::ClientOptions{});
    mcp::DetailedTask working;
    mcp::DetailedTask finished;
    std::optional<mcp::CallToolResult> result;
    std::string id;
    run([&]() -> mcp::Task<void> {
        const json arguments = json::object();
        auto started = co_await client_->start_tool("gated", arguments);
        EXPECT_TRUE(std::holds_alternative<mcp::TaskHandle>(started));
        if (!std::holds_alternative<mcp::TaskHandle>(started)) {
            co_return;
        }
        const auto handle = std::get<mcp::TaskHandle>(started);
        id = handle.id();
        EXPECT_EQ(handle.created().status, mcp::TaskStatus::eWorking);
        working = co_await handle.get();
        release_ = true;
        finished = co_await handle.wait();
        result = co_await handle.result();
    });
    EXPECT_EQ(id.size(), 32u);
    EXPECT_EQ(working.task.status, mcp::TaskStatus::eWorking);
    EXPECT_EQ(finished.task.status, mcp::TaskStatus::eCompleted);
    ASSERT_TRUE(finished.result.has_value());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->structuredContent->at("released"), true);
}

TEST_F(ClientTasks, StartToolReturnsTheResultWhenTheServerRunsItInThePlace) {
    connect(mcp::ClientOptions{});
    std::optional<mcp::CallToolResult> result;
    run([&]() -> mcp::Task<void> {
        const json arguments = json::object();
        auto started = co_await client_->start_tool("never", arguments);
        if (auto* inline_result = std::get_if<mcp::CallToolResult>(&started)) {
            result = *inline_result;
        }
    });
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->structuredContent->at("inline"), true);
}

TEST_F(ClientTasks, TheApplicationAnswersAnInputRequestWithUpdate) {
    connect(mcp::ClientOptions{});
    mcp::DetailedTask waiting;
    mcp::DetailedTask finished;
    run([&]() -> mcp::Task<void> {
        const json arguments = json::object();
        auto started = co_await client_->start_tool("ask", arguments);
        const auto handle = std::get<mcp::TaskHandle>(started);
        for (int attempt = 0; attempt < 5000; ++attempt) {
            waiting = co_await handle.get();
            if (waiting.task.status == mcp::TaskStatus::eInputRequired) {
                break;
            }
            auto executor = co_await asio::this_coro::executor;
            asio::steady_timer pause(executor, std::chrono::milliseconds(1));
            co_await pause.async_wait(asio::use_awaitable);
        }
        const auto key = waiting.inputRequests->begin().key();
        const json responses = {{key, {{"action", "accept"}, {"content", {{"name", "Ada"}}}}}};
        co_await handle.update(responses);
        finished = co_await handle.wait();
    });
    EXPECT_EQ(waiting.task.status, mcp::TaskStatus::eInputRequired);
    ASSERT_TRUE(finished.result.has_value());
    EXPECT_EQ(finished.result->at("structuredContent").at("name"), "Ada");
    EXPECT_EQ(elicitations_.load(), 0);
}

TEST_F(ClientTasks, CancellingATaskEndsIt) {
    connect(mcp::ClientOptions{});
    mcp::DetailedTask finished;
    bool threw = false;
    run([&]() -> mcp::Task<void> {
        const json arguments = json::object();
        auto started = co_await client_->start_tool("cancellable", arguments);
        const auto handle = std::get<mcp::TaskHandle>(started);
        co_await handle.cancel();
        finished = co_await handle.wait();
        try {
            co_await handle.result();
        } catch (const mcp::McpError&) {
            threw = true;
        }
    });
    EXPECT_EQ(finished.task.status, mcp::TaskStatus::eCancelled);
    EXPECT_TRUE(threw);
}

TEST_F(ClientTasks, ATaskIsResumedFromItsIdAlone) {
    connect(mcp::ClientOptions{});
    mcp::DetailedTask finished;
    run([&]() -> mcp::Task<void> {
        const json arguments = json::object();
        auto started = co_await client_->start_tool("gated", arguments);
        const auto id = std::get<mcp::TaskHandle>(started).id();
        const auto resumed = client_->task_handle(id);
        release_ = true;
        finished = co_await resumed.wait();
    });
    EXPECT_EQ(finished.task.status, mcp::TaskStatus::eCompleted);
}

TEST_F(ClientTasks, AnUnknownTaskIsInvalidParams) {
    connect(mcp::ClientOptions{});
    int code = 0;
    run([&]() -> mcp::Task<void> {
        const auto handle = client_->task_handle("0123456789abcdef0123456789abcdef");
        try {
            co_await handle.get();
        } catch (const mcp::McpError& error) {
            code = error.code();
        }
    });
    EXPECT_EQ(code, mcp::constants::jsonrpc::g_INVALID_PARAMS);
}

TEST_F(ClientTasks, ADetachedHandleRefusesEveryCall) {
    connect(mcp::ClientOptions{});
    int code = 0;
    run([&]() -> mcp::Task<void> {
        const mcp::TaskHandle detached;
        try {
            co_await detached.get();
        } catch (const mcp::McpError& error) {
            code = error.code();
        }
    });
    EXPECT_EQ(code, mcp::constants::jsonrpc::g_INVALID_REQUEST);
}

TEST_F(ClientTasks, ATaskStatusNotificationArrivesOnAStreamThatAsksForIt) {
    connect(mcp::ClientOptions{});
    std::optional<mcp::SubscriptionEvent> event;
    mcp::SubscriptionFilter acknowledged;
    run([&]() -> mcp::Task<void> {
        const json arguments = json::object();
        auto started = co_await client_->start_tool("gated", arguments);
        const auto id = std::get<mcp::TaskHandle>(started).id();
        mcp::SubscriptionFilter filter;
        filter.taskIds = {id};
        auto subscription = co_await client_->listen(filter);
        acknowledged = subscription.acknowledged();
        release_ = true;
        // The state at the acknowledgment comes first, then the change.
        for (int received = 0; received < 2; ++received) {
            event = co_await subscription.next();
            if (!event || event->params.at("status") == "completed") {
                break;
            }
        }
        co_await subscription.close();
    });
    ASSERT_EQ(acknowledged.taskIds.size(), 1u);
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->method, "notifications/tasks");
    EXPECT_EQ(event->params.at("status"), "completed");
    EXPECT_EQ(event->params.at("taskId"), acknowledged.taskIds[0]);
}

TEST(ClientTasksOptions, PollAndTimeoutMustBePositive) {
    asio::io_context io;
    auto [client_transport, server_transport] = mcp::create_memory_transport_pair(io.get_executor());
    mcp::ClientOptions poll;
    poll.task_poll_interval = std::chrono::milliseconds(0);
    EXPECT_THROW(mcp::Client(client_transport, io.get_executor(), poll), std::invalid_argument);
    mcp::ClientOptions timeout;
    timeout.task_timeout = std::chrono::milliseconds(-1);
    EXPECT_THROW(mcp::Client(client_transport, io.get_executor(), timeout), std::invalid_argument);
}

// ---- against a scripted server -----------------------------------------------------------------

json response_to(const json& request, json result) {
    return {{"jsonrpc", "2.0"}, {"id", request.at("id")}, {"result", std::move(result)}};
}

json task_fields(const std::string& status, json extra = json::object()) {
    json task = {{"taskId", "task-1"},
                 {"status", status},
                 {"createdAt", "2026-01-01T00:00:00.000Z"},
                 {"lastUpdatedAt", "2026-01-01T00:00:00.000Z"},
                 {"ttlMs", 60000}};
    task.update(extra);
    return task;
}

class ScriptedServer {
   public:
    using Reply = std::function<json(const json&, std::size_t)>;

    explicit ScriptedServer(mcp::ClientOptions options = {})
        : transport(std::make_shared<ScriptedTransport>(io.get_executor())), guard(io) {
        options.protocol = mcp::ClientProtocol::pinned("2026-07-28");
        client = std::make_unique<mcp::Client>(transport, io.get_executor(), std::move(options));
        transport->set_on_write([this](std::string_view text) {
            auto message = json::parse(text);
            seen.push_back(message);
            if (!message.contains("id") || !message.contains("method")) {
                return;
            }
            const auto method = message.at("method").get<std::string>();
            if (method == "server/discover") {
                transport->enqueue_message(
                    response_to(message, {{"resultType", "complete"},
                                          {"supportedVersions", json::array({"2026-07-28"})},
                                          {"capabilities", {{"tools", json::object()}}},
                                          {"_meta",
                                           {{"io.modelcontextprotocol/serverInfo",
                                             {{"name", "peer"}, {"version", "1"}}}}}})
                        .dump());
                return;
            }
            const auto index = ++calls[method];
            auto answer = reply(message, index);
            if (!answer.is_null()) {
                transport->enqueue_message(response_to(message, std::move(answer)).dump());
            }
        });
    }

    void run(std::function<mcp::Task<void>()> body) {
        guard.expires_after(g_hang_guard);
        guard.async_wait([this](const boost::system::error_code& error) {
            if (!error) {
                guard_fired = true;
                io.stop();
            }
        });
        mcp_test::spawn_detached(io, [this, body = std::move(body)]() -> mcp::Task<void> {
            try {
                co_await client->connect("tasks-client", "1.0");
                co_await body();
            } catch (const std::exception& error) {
                ADD_FAILURE() << "unexpected exception: " << error.what();
            }
            client->close();
            guard.cancel();
        });
        io.run();
        EXPECT_FALSE(guard_fired) << "the test did not finish";
    }

    [[nodiscard]] std::vector<json> requests(std::string_view method) const {
        std::vector<json> matching;
        for (const auto& message : seen) {
            if (message.contains("id") && message.value("method", "") == method) {
                matching.push_back(message);
            }
        }
        return matching;
    }

    asio::io_context io;
    std::shared_ptr<ScriptedTransport> transport;
    std::unique_ptr<mcp::Client> client;
    Reply reply;
    std::vector<json> seen;
    std::map<std::string, std::size_t> calls;
    asio::steady_timer guard;
    bool guard_fired{false};
};

json declared_extension(const json& request) {
    return request.at("params")
        .at("_meta")
        .at("io.modelcontextprotocol/clientCapabilities")
        .value("extensions", json::object());
}

TEST(ClientTasksScripted, ATaskAnswerIsAnErrorWhenTheClientDeclaredNothing) {
    ScriptedServer server;
    server.reply = [](const json&, std::size_t) -> json {
        json task = task_fields("working");
        task["resultType"] = "task";
        return task;
    };
    int code = 0;
    server.run([&]() -> mcp::Task<void> {
        try {
            const json arguments = json::object();
            co_await server.client->call_tool("t", arguments);
        } catch (const mcp::McpError& error) {
            code = error.code();
        }
    });
    EXPECT_EQ(code, mcp::constants::jsonrpc::g_INVALID_REQUEST);
    const auto calls = server.requests("tools/call");
    ASSERT_EQ(calls.size(), 1u);
    EXPECT_TRUE(declared_extension(calls[0]).empty());
}

TEST(ClientTasksScripted, EveryRequestDeclaresTheExtensionWhenTheOptionIsOn) {
    mcp::ClientOptions options;
    options.tasks = mcp::ClientTaskMode::eAuto;
    options.task_poll_interval = std::chrono::milliseconds(1);
    ScriptedServer server(options);
    server.reply = [](const json& request, std::size_t) -> json {
        if (request.at("method") == "tools/call") {
            json task = task_fields("working");
            task["resultType"] = "task";
            return task;
        }
        if (request.at("method") == "tools/list") {
            return {{"resultType", "complete"}, {"tools", json::array()}};
        }
        return task_fields("completed",
                           {{"resultType", "complete"}, {"result", {{"content", json::array()}}}});
    };
    server.run([&]() -> mcp::Task<void> {
        const json arguments = json::object();
        co_await server.client->call_tool("t", arguments);
        co_await server.client->list_tools();
    });
    for (const auto* method : {"tools/call", "tasks/get", "tools/list"}) {
        const auto sent = server.requests(method);
        ASSERT_FALSE(sent.empty()) << method;
        EXPECT_TRUE(declared_extension(sent[0]).contains("io.modelcontextprotocol/tasks")) << method;
    }
}

TEST(ClientTasksScripted, TheTaskMethodsDeclareTheExtensionWhateverTheOption) {
    ScriptedServer server;
    server.reply = [](const json&, std::size_t) -> json {
        return task_fields("completed",
                           {{"resultType", "complete"}, {"result", {{"content", json::array()}}}});
    };
    server.run([&]() -> mcp::Task<void> {
        const auto handle = server.client->task_handle("task-1");
        co_await handle.get();
        const json responses = json::object();
        co_await handle.update(responses);
        co_await handle.cancel();
    });
    for (const auto* method : {"tasks/get", "tasks/update", "tasks/cancel"}) {
        const auto sent = server.requests(method);
        ASSERT_EQ(sent.size(), 1u) << method;
        EXPECT_EQ(sent[0].at("params").at("taskId"), "task-1");
        EXPECT_TRUE(declared_extension(sent[0]).contains("io.modelcontextprotocol/tasks")) << method;
    }
}

// Closing the client ends a wait for a task at once, rather than at the next poll.
TEST(ClientTasksScripted, ClosingTheClientEndsAWaitForATask) {
    mcp::ClientOptions options;
    options.task_poll_interval = std::chrono::hours(1);
    ScriptedServer server(options);
    server.reply = [](const json&, std::size_t) -> json {
        return task_fields("working", {{"resultType", "complete"}});
    };
    std::string ended;
    server.run([&]() -> mcp::Task<void> {
        const auto handle = server.client->task_handle("task-1");
        // Closes the client once the first poll has been answered and the wait pauses.
        mcp_test::spawn_detached(server.io, [&server]() -> mcp::Task<void> {
            while (server.requests("tasks/get").empty()) {
                asio::steady_timer timer(server.io, std::chrono::milliseconds(1));
                co_await timer.async_wait(asio::use_awaitable);
            }
            for (int tick = 0; tick < 10; ++tick) {
                asio::steady_timer timer(server.io, std::chrono::milliseconds(1));
                co_await timer.async_wait(asio::use_awaitable);
            }
            server.client->close();
        });
        try {
            co_await handle.wait();
            ended = "finished";
        } catch (const std::exception& error) {
            ended = error.what();
        }
    });
    EXPECT_NE(ended.find("Client transport"), std::string::npos) << ended;
}

TEST(ClientTasksScripted, PollsAsOftenAsTheServerSuggests) {
    mcp::ClientOptions options;
    options.task_poll_interval = std::chrono::hours(1);
    ScriptedServer server(options);
    server.reply = [](const json&, std::size_t index) -> json {
        if (index < 3) {
            return task_fields("working", {{"resultType", "complete"}, {"pollIntervalMs", 40}});
        }
        return task_fields("completed",
                           {{"resultType", "complete"}, {"result", {{"content", json::array()}}}});
    };
    std::chrono::milliseconds elapsed{0};
    server.run([&]() -> mcp::Task<void> {
        const auto handle = server.client->task_handle("task-1");
        const auto begin = std::chrono::steady_clock::now();
        co_await handle.wait();
        elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - begin);
    });
    EXPECT_EQ(server.requests("tasks/get").size(), 3u);
    // Two pauses of the suggested interval separate three polls.
    EXPECT_GE(elapsed.count(), 80);
}

TEST(ClientTasksScripted, UsesItsOwnIntervalWhenTheServerSuggestsNone) {
    mcp::ClientOptions options;
    options.task_poll_interval = std::chrono::milliseconds(30);
    ScriptedServer server(options);
    server.reply = [](const json&, std::size_t index) -> json {
        if (index < 3) {
            return task_fields("working", {{"resultType", "complete"}});
        }
        return task_fields("completed",
                           {{"resultType", "complete"}, {"result", {{"content", json::array()}}}});
    };
    std::chrono::milliseconds elapsed{0};
    server.run([&]() -> mcp::Task<void> {
        const auto handle = server.client->task_handle("task-1");
        const auto begin = std::chrono::steady_clock::now();
        co_await handle.wait();
        elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - begin);
    });
    EXPECT_GE(elapsed.count(), 60);
}

TEST(ClientTasksScripted, GivesUpAfterTheTaskTimeout) {
    mcp::ClientOptions options;
    options.task_poll_interval = std::chrono::milliseconds(5);
    options.task_timeout = std::chrono::milliseconds(60);
    ScriptedServer server(options);
    server.reply = [](const json&, std::size_t) -> json {
        return task_fields("working", {{"resultType", "complete"}});
    };
    int code = 0;
    server.run([&]() -> mcp::Task<void> {
        const auto handle = server.client->task_handle("task-1");
        try {
            co_await handle.wait();
        } catch (const mcp::McpError& error) {
            code = error.code();
        }
    });
    EXPECT_EQ(code, mcp::constants::jsonrpc::g_REQUEST_TIMEOUT);
}

TEST(ClientTasksScripted, CallToolCancelsTheTaskItGivesUpOn) {
    mcp::ClientOptions options;
    options.tasks = mcp::ClientTaskMode::eAuto;
    options.task_poll_interval = std::chrono::milliseconds(5);
    options.task_timeout = std::chrono::milliseconds(60);
    ScriptedServer server(options);
    server.reply = [](const json& request, std::size_t) -> json {
        if (request.at("method") == "tools/call") {
            json task = task_fields("working");
            task["resultType"] = "task";
            return task;
        }
        if (request.at("method") == "tasks/cancel") {
            return json::object();
        }
        return task_fields("working", {{"resultType", "complete"}});
    };
    int code = 0;
    server.run([&]() -> mcp::Task<void> {
        try {
            const json arguments = json::object();
            co_await server.client->call_tool("t", arguments);
        } catch (const mcp::McpError& error) {
            code = error.code();
        }
    });
    EXPECT_EQ(code, mcp::constants::jsonrpc::g_REQUEST_TIMEOUT);
    const auto cancels = server.requests("tasks/cancel");
    ASSERT_EQ(cancels.size(), 1u);
    EXPECT_EQ(cancels[0].at("params").at("taskId"), "task-1");
}

// The progress a followed task reports with the call's token reaches the call's callback until the
// task's result arrives; the polls made for the call carry no token of their own.
TEST(ClientTasksScripted, AFollowedTaskKeepsTheCallsProgressToken) {
    mcp::ClientOptions options;
    options.tasks = mcp::ClientTaskMode::eAuto;
    options.task_poll_interval = std::chrono::milliseconds(5);
    ScriptedServer server(options);
    json token;
    server.reply = [&](const json& request, std::size_t index) -> json {
        if (request.at("method") == "tools/call") {
            token = request.at("params").at("_meta").value("progressToken", json());
            json task = task_fields("working");
            task["resultType"] = "task";
            return task;
        }
        if (index == 1) {
            server.transport->enqueue_message(
                json{{"jsonrpc", "2.0"},
                     {"method", "notifications/progress"},
                     {"params", {{"progressToken", token}, {"progress", 0.5}}}}
                    .dump());
            return task_fields("working", {{"resultType", "complete"}});
        }
        return task_fields("completed",
                           {{"resultType", "complete"}, {"result", {{"content", json::array()}}}});
    };
    std::vector<double> seen;
    int unrouted = 0;
    server.client->on_progress([&](const mcp::ProgressNotificationParams&) { ++unrouted; });
    server.run([&]() -> mcp::Task<void> {
        mcp::RequestOptions request;
        request.on_progress = [&](const mcp::ProgressNotificationParams& progress) {
            seen.push_back(progress.progress);
        };
        const json arguments = json::object();
        co_await server.client->call_tool("t", arguments, request);
    });
    EXPECT_FALSE(token.is_null()) << "the call carried no progress token";
    EXPECT_EQ(seen, std::vector<double>{0.5});
    EXPECT_EQ(unrouted, 0);
    const auto polls = server.requests("tasks/get");
    ASSERT_EQ(polls.size(), 2u);
    for (const auto& poll : polls) {
        EXPECT_FALSE(poll.at("params").at("_meta").contains("progressToken"));
    }
}

// A cancelled call_tool() stops following its task at once, even in the middle of a long poll
// interval, and asks the server to cancel the task.
TEST(ClientTasksScripted, CancelledCallToolCancelsItsTask) {
    mcp::ClientOptions options;
    options.tasks = mcp::ClientTaskMode::eAuto;
    ScriptedServer server(options);
    server.reply = [](const json& request, std::size_t) -> json {
        if (request.at("method") == "tools/call") {
            json task = task_fields("working");
            task["resultType"] = "task";
            return task;
        }
        if (request.at("method") == "tasks/cancel") {
            return json::object();
        }
        // An interval far past the test's hang guard: only the token can end the pause.
        return task_fields("working", {{"resultType", "complete"}, {"pollIntervalMs", 3600000}});
    };
    int code = 0;
    server.run([&]() -> mcp::Task<void> {
        mcp::RequestOptions request_options;
        request_options.cancellation = mcp::CancellationToken();
        mcp_test::spawn_detached(server.io,
                                 [&server, token = *request_options.cancellation]() -> mcp::Task<void> {
                                     asio::steady_timer tick(server.io);
                                     while (server.requests("tasks/get").empty()) {
                                         tick.expires_after(std::chrono::milliseconds(1));
                                         co_await tick.async_wait(asio::use_awaitable);
                                     }
                                     // Past the first poll's answer, so the call is in its pause.
                                     tick.expires_after(std::chrono::milliseconds(20));
                                     co_await tick.async_wait(asio::use_awaitable);
                                     token.cancel("user stopped it");
                                 });
        try {
            const json arguments = json::object();
            co_await server.client->call_tool("t", arguments, request_options);
        } catch (const mcp::McpError& error) {
            code = error.code();
        }
    });
    EXPECT_EQ(code, mcp::constants::jsonrpc::g_REQUEST_CANCELLED);
    EXPECT_EQ(server.requests("tasks/get").size(), 1u);
    const auto cancels = server.requests("tasks/cancel");
    ASSERT_EQ(cancels.size(), 1u);
    EXPECT_EQ(cancels[0].at("params").at("taskId"), "task-1");
}

// A cancelled TaskHandle::wait() stops polling at once and leaves the task to its handle: nothing
// asks the server to cancel it.
TEST(ClientTasksScripted, CancelledWaitLeavesTheTask) {
    ScriptedServer server;
    server.reply = [](const json&, std::size_t) -> json {
        // An interval far past the test's hang guard: only the token can end the pause.
        return task_fields("working", {{"resultType", "complete"}, {"pollIntervalMs", 3600000}});
    };
    int code = 0;
    server.run([&]() -> mcp::Task<void> {
        mcp::RequestOptions request_options;
        request_options.cancellation = mcp::CancellationToken();
        mcp_test::spawn_detached(server.io,
                                 [&server, token = *request_options.cancellation]() -> mcp::Task<void> {
                                     asio::steady_timer tick(server.io);
                                     while (server.requests("tasks/get").empty()) {
                                         tick.expires_after(std::chrono::milliseconds(1));
                                         co_await tick.async_wait(asio::use_awaitable);
                                     }
                                     // Past the first poll's answer, so the wait is in its pause.
                                     tick.expires_after(std::chrono::milliseconds(20));
                                     co_await tick.async_wait(asio::use_awaitable);
                                     token.cancel();
                                 });
        const auto handle = server.client->task_handle("task-1");
        try {
            co_await handle.wait(request_options);
        } catch (const mcp::McpError& error) {
            code = error.code();
        }
    });
    EXPECT_EQ(code, mcp::constants::jsonrpc::g_REQUEST_CANCELLED);
    EXPECT_EQ(server.requests("tasks/get").size(), 1u);
    EXPECT_TRUE(server.requests("tasks/cancel").empty());
}

TEST(ClientTasksScripted, CallToolCancelsTheTaskWhenItCannotAnswerItsQuestions) {
    mcp::ClientOptions options;
    options.tasks = mcp::ClientTaskMode::eAuto;
    options.task_poll_interval = std::chrono::milliseconds(1);
    ScriptedServer server(options);
    const json question = {
        {"method", "elicitation/create"},
        {"params",
         {{"mode", "form"}, {"message", "Name?"}, {"requestedSchema", {{"type", "object"}}}}}};
    server.reply = [&question](const json& request, std::size_t) -> json {
        if (request.at("method") == "tools/call") {
            json task = task_fields("working");
            task["resultType"] = "task";
            return task;
        }
        if (request.at("method") == "tasks/cancel") {
            return json::object();
        }
        // No elicitation handler is registered, so the question cannot be answered.
        return task_fields("input_required",
                           {{"resultType", "complete"}, {"inputRequests", {{"name", question}}}});
    };
    int code = 0;
    server.run([&]() -> mcp::Task<void> {
        try {
            const json arguments = json::object();
            co_await server.client->call_tool("t", arguments);
        } catch (const mcp::McpError& error) {
            code = error.code();
        }
    });
    EXPECT_EQ(code, mcp::constants::jsonrpc::g_INVALID_REQUEST);
    EXPECT_EQ(server.requests("tasks/cancel").size(), 1u);
}

TEST(ClientTasksScripted, AHandleThatGivesUpLeavesTheTaskRunning) {
    mcp::ClientOptions options;
    options.task_poll_interval = std::chrono::milliseconds(5);
    options.task_timeout = std::chrono::milliseconds(40);
    ScriptedServer server(options);
    server.reply = [](const json&, std::size_t) -> json {
        return task_fields("working", {{"resultType", "complete"}});
    };
    int code = 0;
    server.run([&]() -> mcp::Task<void> {
        const auto handle = server.client->task_handle("task-1");
        try {
            co_await handle.wait();
        } catch (const mcp::McpError& error) {
            code = error.code();
        }
    });
    EXPECT_EQ(code, mcp::constants::jsonrpc::g_REQUEST_TIMEOUT);
    EXPECT_TRUE(server.requests("tasks/cancel").empty());
}

// What a server that sends a task the decoders refuse is answered with: an McpError, from the
// call that follows tasks and from the one that returns a handle alike.
TEST(ClientTasksScripted, AMalformedTaskIsAnMcpErrorOnBothPaths) {
    const std::vector<json> malformed = {
        json{{"resultType", "task"}},
        json{{"resultType", "task"}, {"taskId", 7}},
        [] {
            json task = task_fields("bogus");
            task["resultType"] = "task";
            return task;
        }(),
        [] {
            json task = task_fields("working");
            task["resultType"] = "task";
            task["status"] = nullptr;
            return task;
        }(),
    };
    for (const auto& reply : malformed) {
        SCOPED_TRACE(reply.dump());
        mcp::ClientOptions options;
        options.tasks = mcp::ClientTaskMode::eAuto;
        ScriptedServer server(options);
        server.reply = [&reply](const json&, std::size_t) -> json { return reply; };
        int following = 0;
        int starting = 0;
        server.run([&]() -> mcp::Task<void> {
            const json arguments = json::object();
            try {
                co_await server.client->call_tool("t", arguments);
            } catch (const mcp::McpError& error) {
                following = error.code();
            }
            try {
                co_await server.client->start_tool("t", arguments);
            } catch (const mcp::McpError& error) {
                starting = error.code();
            }
        });
        EXPECT_EQ(following, mcp::constants::jsonrpc::g_INVALID_REQUEST);
        EXPECT_EQ(starting, mcp::constants::jsonrpc::g_INVALID_REQUEST);
    }
}

TEST(ClientTasksScripted, AFailedTaskWithoutAnErrorCodeIsAnMcpError) {
    mcp::ClientOptions options;
    options.tasks = mcp::ClientTaskMode::eAuto;
    ScriptedServer server(options);
    server.reply = [](const json& request, std::size_t) -> json {
        if (request.at("method") == "tools/call") {
            json task = task_fields("working");
            task["resultType"] = "task";
            return task;
        }
        return task_fields("failed", {{"resultType", "complete"}, {"error", json::object()}});
    };
    int code = 0;
    server.run([&]() -> mcp::Task<void> {
        try {
            const json arguments = json::object();
            co_await server.client->call_tool("t", arguments);
        } catch (const mcp::McpError& error) {
            code = error.code();
        }
    });
    EXPECT_EQ(code, mcp::constants::jsonrpc::g_INVALID_REQUEST);
}

TEST(ClientTasksScripted, AnUnknownStatusIsNotATaskState) {
    ScriptedServer server;
    server.reply = [](const json&, std::size_t) -> json {
        return task_fields("paused", {{"resultType", "complete"}});
    };
    int code = 0;
    server.run([&]() -> mcp::Task<void> {
        const auto handle = server.client->task_handle("task-1");
        try {
            co_await handle.get();
        } catch (const mcp::McpError& error) {
            code = error.code();
        }
    });
    EXPECT_EQ(code, mcp::constants::jsonrpc::g_INVALID_REQUEST);
}

TEST(ClientTasksScripted, EachInputRequestIsAnsweredOnce) {
    mcp::ClientOptions options;
    options.task_poll_interval = std::chrono::milliseconds(1);
    ScriptedServer server(options);
    int answered = 0;
    server.client->on_request("elicitation/create", [&answered](const json&) -> mcp::Task<json> {
        ++answered;
        co_return json{{"action", "accept"}, {"content", {{"name", "Luca"}}}};
    });
    const json question = {
        {"method", "elicitation/create"},
        {"params",
         {{"mode", "form"}, {"message", "Name?"}, {"requestedSchema", {{"type", "object"}}}}}};
    server.reply = [&question](const json&, std::size_t index) -> json {
        if (index <= 3) {
            // The same question shows on each poll until the update takes effect.
            return task_fields("input_required",
                               {{"resultType", "complete"}, {"inputRequests", {{"name", question}}}});
        }
        return task_fields("completed",
                           {{"resultType", "complete"}, {"result", {{"content", json::array()}}}});
    };
    server.run([&]() -> mcp::Task<void> {
        const auto handle = server.client->task_handle("task-1");
        co_await handle.wait();
    });
    EXPECT_EQ(answered, 1);
    const auto updates = server.requests("tasks/update");
    ASSERT_EQ(updates.size(), 1u);
    EXPECT_EQ(updates[0].at("params").at("inputResponses").at("name").at("action"), "accept");
}

TEST(ClientTasksScripted, AListenRequestForTasksDeclaresTheExtension) {
    ScriptedServer server;
    server.reply = [](const json&, std::size_t) -> json { return json::object(); };
    std::optional<json> listen_request;
    server.transport->set_on_write([&server, &listen_request](std::string_view text) {
        auto message = json::parse(text);
        server.seen.push_back(message);
        if (!message.contains("id")) {
            return;
        }
        const auto method = message.at("method").get<std::string>();
        if (method == "server/discover") {
            server.transport->enqueue_message(
                response_to(message, {{"resultType", "complete"},
                                      {"supportedVersions", json::array({"2026-07-28"})},
                                      {"capabilities", json::object()}})
                    .dump());
        } else if (method == "subscriptions/listen") {
            listen_request = message;
            server.transport->enqueue_message(json{
                {"jsonrpc", "2.0"},
                {"method", "notifications/subscriptions/acknowledged"},
                {"params",
                 {{"_meta", {{"io.modelcontextprotocol/subscriptionId", message.at("id")}}},
                  {"notifications",
                   {{"taskIds", {"task-1"}}}}}}}.dump());
        }
    });
    mcp::SubscriptionFilter acknowledged;
    server.run([&]() -> mcp::Task<void> {
        mcp::SubscriptionFilter filter;
        filter.taskIds = {"task-1", "task-2"};
        auto subscription = co_await server.client->listen(filter);
        acknowledged = subscription.acknowledged();
        EXPECT_EQ(subscription.unhonored().taskIds, std::vector<std::string>{"task-2"});
    });
    ASSERT_TRUE(listen_request.has_value());
    EXPECT_EQ(listen_request->at("params").at("notifications").at("taskIds"),
              json::array({"task-1", "task-2"}));
    EXPECT_TRUE(declared_extension(*listen_request).contains("io.modelcontextprotocol/tasks"));
    EXPECT_EQ(acknowledged.taskIds, std::vector<std::string>{"task-1"});
}

}  // namespace

}  // namespace client_tasks_test

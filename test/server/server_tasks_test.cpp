#include "../support/hang_guard.hpp"
#include "../test_utils.hpp"

#include "mcp/auth/oauth.hpp"
#include "mcp/protocol/extensions.hpp"
#include "mcp/server/server.hpp"
#include "mcp/transport/memory.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <chrono>
#include <functional>
#include <mcp/core/constants.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace server_tasks_test {

namespace {

namespace asio = boost::asio;
using nlohmann::json;

const std::string g_tasks = "io.modelcontextprotocol/tasks";

/// Waits, a millisecond at a time, until @p condition holds. Gives up after g_hang_guard so a broken
/// hand-off fails the test instead of hanging the run.
mcp::Task<bool> wait_until(std::function<bool()> condition) {
    auto executor = co_await asio::this_coro::executor;
    const auto give_up = std::chrono::steady_clock::now() + g_hang_guard;
    while (!condition() && std::chrono::steady_clock::now() < give_up) {
        asio::steady_timer timer(executor, std::chrono::milliseconds(1));
        co_await timer.async_wait(asio::use_awaitable);
    }
    co_return condition();
}

/// The capabilities of a client that declares the tasks extension and whatever else is in @p more.
json tasks_client(json more = json::object()) {
    more["extensions"] = {{g_tasks, json::object()}};
    return more;
}

/// A 2026-07-28 request from a client that declares @p capabilities.
json modern(const json& id, std::string_view method, json params, json capabilities) {
    auto request = make_modern_request(id, method, std::move(params));
    request["params"]["_meta"]["io.modelcontextprotocol/clientCapabilities"] = std::move(capabilities);
    return request;
}

json call_tool(const json& id, std::string_view tool, json capabilities = tasks_client(),
               json arguments = json::object()) {
    return modern(id, "tools/call", {{"name", tool}, {"arguments", std::move(arguments)}},
                  std::move(capabilities));
}

json task_request(const json& id, std::string_view method, const std::string& task_id,
                  json capabilities = tasks_client(), json extra = json::object()) {
    extra["taskId"] = task_id;
    return modern(id, method, std::move(extra), std::move(capabilities));
}

json accept(const std::string& name) { return {{"action", "accept"}, {"content", {{"name", name}}}}; }

json form_elicitation(const std::string& message) {
    mcp::ElicitRequestFormParams params;
    params.message = message;
    params.requestedSchema = {{"type", "object"}, {"properties", {{"name", {{"type", "string"}}}}}};
    return params;
}

/// A TaskStore that counts the calls it forwards to an in-memory store.
class CountingStore final : public mcp::TaskStore {
   public:
    std::optional<mcp::TaskRecord> create(std::string_view owner,
                                          std::optional<std::chrono::milliseconds> ttl,
                                          std::optional<std::chrono::milliseconds> poll) override {
        ++creates;
        return inner.create(owner, ttl, poll);
    }
    std::optional<mcp::TaskRecord> get(std::string_view owner, std::string_view id) override {
        ++gets;
        return inner.get(owner, id);
    }
    std::optional<mcp::TaskRecord> set_status_message(std::string_view id, std::string m) override {
        return inner.set_status_message(id, std::move(m));
    }
    std::optional<mcp::TaskRecord> add_input_request(std::string_view id, std::string_view key,
                                                     json request) override {
        return inner.add_input_request(id, key, std::move(request));
    }
    std::optional<mcp::TaskRecord> submit_input_responses(std::string_view id,
                                                          const json& responses) override {
        return inner.submit_input_responses(id, responses);
    }
    std::optional<json> take_input_response(std::string_view id, std::string_view key) override {
        return inner.take_input_response(id, key);
    }
    std::optional<mcp::TaskRecord> complete(std::string_view id, json result) override {
        ++completes;
        return inner.complete(id, std::move(result));
    }
    std::optional<mcp::TaskRecord> fail(std::string_view id, json error, std::string m) override {
        auto stored = inner.fail(id, std::move(error), std::move(m));
        if (stored) {
            ++fails;
        }
        return stored;
    }
    std::optional<mcp::TaskRecord> cancel(std::string_view id, std::string m) override {
        if (before_cancel) {
            before_cancel();
        }
        return inner.cancel(id, std::move(m));
    }

    mcp::InMemoryTaskStore inner;
    std::atomic<int> creates{0};
    std::atomic<int> gets{0};
    std::atomic<int> completes{0};
    std::atomic<int> fails{0};  ///< The failures the store recorded.
    /// Runs at the start of cancel(), before the store changes.
    std::function<void()> before_cancel;
};

class TasksTest : public ::testing::Test {
   protected:
    asio::io_context io_ctx_;
    std::unique_ptr<mcp::Server> server_;
    std::atomic<bool> release_{false};
    std::atomic<bool> started_{false};
    std::atomic<int>* creates_probe_ = nullptr;
    std::atomic<int> creates_at_start_{-1};
    std::atomic<bool> saw_cancel_{false};
    std::atomic<int> middleware_runs_{0};
    std::atomic<int> preparations_{0};
    std::atomic<int> prepared_handler_runs_{0};
    std::thread::id handler_thread_;
    std::mutex handler_thread_mutex_;

    void SetUp() override { server_ = make_server(); }

    std::unique_ptr<mcp::Server> make_server() {
        mcp::ServerCapabilities caps;
        caps.tools = mcp::ServerCapabilities::ToolsCapability{};
        auto server = std::make_unique<mcp::Server>(mcp::Implementation{"tasks", "1.0"}, caps);
        server->add_tool<json, json>(
            "echo", "Echoes", json{{"type", "object"}},
            [](json args) -> mcp::Task<json> { co_return json{{"echo", std::move(args)}}; });
        server->add_tool<json, json>(
            "required", "Always a task", json{{"type", "object"}},
            [](json args) -> mcp::Task<json> { co_return json{{"echo", std::move(args)}}; });
        server->add_tool<json, json>(
            "never", "Never a task", json{{"type", "object"}},
            [](json args) -> mcp::Task<json> { co_return json{{"echo", std::move(args)}}; });
        server->add_tool<json, json>("gated", "Runs until released", json{{"type", "object"}},
                                     [this](json /*args*/) -> mcp::Task<json> {
                                         if (creates_probe_ != nullptr) {
                                             creates_at_start_ = creates_probe_->load();
                                         }
                                         started_ = true;
                                         {
                                             std::lock_guard lock(handler_thread_mutex_);
                                             handler_thread_ = std::this_thread::get_id();
                                         }
                                         co_await wait_until([this] { return release_.load(); });
                                         co_return json{{"released", true}};
                                     });
        server->add_tool<json, json>("cancellable", "Runs until cancelled", json{{"type", "object"}},
                                     [this](mcp::Context& ctx, json /*args*/) -> mcp::Task<json> {
                                         started_ = true;
                                         saw_cancel_ =
                                             co_await wait_until([&ctx] { return ctx.is_cancelled(); });
                                         co_return json{{"cancelled", saw_cancel_.load()}};
                                     });
        server->add_tool<json, json>("tool_error", "Fails as a tool", json{{"type", "object"}},
                                     [](json /*args*/) -> mcp::Task<json> {
                                         throw std::runtime_error("the tool failed");
                                         co_return json::object();
                                     });
        server->add_tool<json, json>(
            "broken_output", "Returns what its output schema forbids", json{{"type", "object"}},
            json{{"type", "object"},
                 {"properties", {{"x", {{"type", "integer"}}}}},
                 {"required", {"x"}}},
            [](json /*args*/) -> mcp::Task<json> { co_return json{{"x", "text"}}; });
        server->add_tool<json, json>(
            "ask", "Asks for a name", json{{"type", "object"}},
            [](mcp::Context& ctx, json /*args*/) -> mcp::Task<json> {
                const auto answer = co_await ctx.elicit(form_elicitation("Your name?"));
                co_return json{{"name", answer.content.value_or(json::object()).value("name", "")}};
            });
        server->add_tool<json, json>(
            "ask_named", "Asks for a name under a key of its own", json{{"type", "object"}},
            [](mcp::Context& ctx, json /*args*/) -> mcp::Task<json> {
                const mcp::InputOptions options{.key = "user_name"};
                const auto answer = co_await ctx.elicit(form_elicitation("Your name?"), options);
                co_return json{{"name", answer.content.value_or(json::object()).value("name", "")}};
            });
        server->add_tool<json, json>("ask_empty_key", "Asks under an empty key",
                                     json{{"type", "object"}},
                                     [](mcp::Context& ctx, json /*args*/) -> mcp::Task<json> {
                                         const mcp::InputOptions options{.key = ""};
                                         co_await ctx.elicit(form_elicitation("Your name?"), options);
                                         co_return json{{"asked", true}};
                                     });
        server->add_tool<json, json>(
            "ask_twice", "Asks two questions", json{{"type", "object"}},
            [](mcp::Context& ctx, json /*args*/) -> mcp::Task<json> {
                const auto first = co_await ctx.elicit(form_elicitation("First?"));
                const auto second = co_await ctx.elicit(form_elicitation("Second?"));
                co_return json{{"first", first.content->value("name", "")},
                               {"second", second.content->value("name", "")}};
            });
        server->add_tool<json, json>(
            "progress", "Reports progress, then waits", json{{"type", "object"}},
            [this](mcp::Context& ctx, json /*args*/) -> mcp::Task<json> {
                co_await ctx.report_progress(1, 4, std::string("quarter done"));
                started_ = true;
                co_await wait_until([this] { return release_.load(); });
                co_return json{{"done", true}};
            });
        server->add_tool<json, json>("progress_sync", "Reports progress without suspending",
                                     json{{"type", "object"}},
                                     [this](mcp::Context& ctx, json /*args*/) -> mcp::Task<json> {
                                         EXPECT_TRUE(ctx.report_progress_sync(2, 8));
                                         started_ = true;
                                         co_await wait_until([this] { return release_.load(); });
                                         co_return json{{"done", true}};
                                     });
        server->add_tool<json, json>("logs", "Logs", json{{"type", "object"}},
                                     [](mcp::Context& ctx, json /*args*/) -> mcp::Task<json> {
                                         co_await ctx.log(mcp::LoggingLevel::eError, "logged");
                                         co_return json{{"logged", true}};
                                     });
        server->add_tool<json, json>("prepared", "A task whose preparation asks first",
                                     json{{"type", "object"}}, [this](json args) -> mcp::Task<json> {
                                         ++prepared_handler_runs_;
                                         co_return json{{"args", std::move(args)}};
                                     });
        server->add_tool<json, json>(
            "prepared_strict", "A prepared task that needs an integer n",
            json{{"type", "object"},
                 {"properties", {{"n", {{"type", "integer"}}}}},
                 {"required", {"n"}}},
            [](json args) -> mcp::Task<json> { co_return json{{"args", std::move(args)}}; });
        server->add_tool<json, json>("sync_progress_outside", "Calls the sync progress outside a task",
                                     json{{"type", "object"}},
                                     [](mcp::Context& ctx, json /*args*/) -> mcp::Task<json> {
                                         co_return json{{"reported", ctx.report_progress_sync(1)}};
                                     });
        return server;
    }

    /// Registers @p preparation for the tool "prepared", which may become a task in @p mode.
    void prepare(mcp::ToolTaskPreparation preparation,
                 mcp::ToolTaskMode mode = mcp::ToolTaskMode::eAuto) {
        server_->set_tool_task_preparation("prepared", std::move(preparation));
        prepared_mode_ = mode;
    }

    mcp::ToolTaskMode prepared_mode_ = mcp::ToolTaskMode::eAuto;

    /// A preparation that asks for a name under the key "user_name" and hands it to the task.
    mcp::ToolTaskPreparation asking_preparation() {
        return [this](mcp::Context& ctx, const json& arguments) -> mcp::Task<mcp::TaskPreparation> {
            ++preparations_;
            const mcp::InputOptions options{.key = "user_name"};
            const auto answer = co_await ctx.elicit(form_elicitation("Your name?"), options);
            mcp::TaskPreparation preparation;
            preparation.arguments = arguments;
            preparation.arguments["name"] = answer.content.value_or(json::object()).value("name", "");
            co_return preparation;
        };
    }

    void enable(mcp::TaskOptions options = {}) {
        options.warning_handler = [this](const std::string& text) { warnings_.push_back(text); };
        server_->enable_tasks(std::move(options));
        server_->set_handler_executor(io_ctx_.get_executor());
        for (const auto* name :
             {"echo", "gated", "cancellable", "tool_error", "broken_output", "ask", "ask_named",
              "ask_empty_key", "ask_twice", "progress", "progress_sync", "logs"}) {
            server_->set_tool_task_mode(name, mcp::ToolTaskMode::eAuto);
        }
        server_->set_tool_task_mode("required", mcp::ToolTaskMode::eRequired);
        server_->set_tool_task_mode("prepared", prepared_mode_);
    }

    std::vector<std::string> warnings_;

    template <typename Body>
    void run(Body body) {
        asio::steady_timer guard(io_ctx_, g_hang_guard);
        guard.async_wait([this](const boost::system::error_code& error) {
            if (!error) {
                ADD_FAILURE() << "the test did not finish";
                io_ctx_.stop();
            }
        });
        asio::co_spawn(
            io_ctx_,
            [this, &body, &guard]() -> mcp::Task<void> {
                co_await body();
                guard.cancel();
                io_ctx_.stop();
            },
            asio::detached);
        io_ctx_.run();
    }

    mcp::Task<json> send(json request, std::string principal = std::string()) {
        mcp::RequestContext context;
        context.principal = std::move(principal);
        const auto wire =
            co_await server_->dispatch_request_direct(std::move(request), std::move(context));
        co_return json::parse(wire);
    }

    /// Starts @p tool as a task and returns its task id.
    mcp::Task<std::string> start(std::string tool, json capabilities = tasks_client(),
                                 std::string principal = std::string()) {
        const json request = call_tool(100, tool, std::move(capabilities));
        const auto response = co_await send(request, std::move(principal));
        EXPECT_EQ(response["result"]["resultType"], "task") << response;
        co_return response["result"].value("taskId", "");
    }

    /// The task's `tasks/get` result, once its status is @p status.
    mcp::Task<json> task_in_status(std::string task_id, std::string status,
                                   json capabilities = tasks_client(),
                                   std::string principal = std::string()) {
        json latest;
        const auto reached = co_await poll_until(
            [&] { return latest.value("status", "") == status; },
            [&]() -> mcp::Task<void> {
                const json request = task_request(101, "tasks/get", task_id, capabilities);
                const auto response = co_await send(request, principal);
                latest = response.value("result", json::object());
            });
        EXPECT_TRUE(reached) << latest;
        co_return latest;
    }

    // Runs @p probe, then checks @p condition, until it holds or g_hang_guard passes.
    static mcp::Task<bool> poll_until(std::function<bool()> condition,
                                      std::function<mcp::Task<void>()> probe) {
        auto executor = co_await asio::this_coro::executor;
        const auto give_up = std::chrono::steady_clock::now() + g_hang_guard;
        for (;;) {
            co_await probe();
            if (condition()) {
                co_return true;
            }
            if (std::chrono::steady_clock::now() >= give_up) {
                co_return false;
            }
            asio::steady_timer timer(executor, std::chrono::milliseconds(1));
            co_await timer.async_wait(asio::use_awaitable);
        }
    }
};

// ---- negotiation -------------------------------------------------------------------------------

TEST_F(TasksTest, DiscoverDoesNotAdvertiseTheExtensionUntilEnabled) {
    json answer;
    run([&]() -> mcp::Task<void> {
        const auto discover = make_discover_request(1);
        answer = co_await send(discover);
    });
    EXPECT_FALSE(answer["result"]["capabilities"].contains("extensions")) << answer;
}

TEST_F(TasksTest, DiscoverAdvertisesTheExtensionOnceEnabled) {
    enable();
    json answer;
    run([&]() -> mcp::Task<void> {
        const auto discover = make_discover_request(1);
        answer = co_await send(discover);
    });
    EXPECT_EQ(answer["result"]["capabilities"]["extensions"][g_tasks], json::object()) << answer;
}

TEST_F(TasksTest, ALegacyInitializeDoesNotAdvertiseTheExtension) {
    enable();
    json answer;
    run([&]() -> mcp::Task<void> {
        const json request = {{"jsonrpc", "2.0"},
                              {"id", 1},
                              {"method", "initialize"},
                              {"params",
                               {{"protocolVersion", "2025-11-25"},
                                {"capabilities", json::object()},
                                {"clientInfo", {{"name", "c"}, {"version", "1"}}}}}};
        answer = co_await send(request);
    });
    ASSERT_TRUE(answer.contains("result")) << answer;
    EXPECT_FALSE(answer["result"]["capabilities"].contains("extensions")) << answer;
}

TEST_F(TasksTest, TaskMethodsDoNotExistUntilTasksAreEnabled) {
    json answer;
    run([&]() -> mcp::Task<void> {
        const json request = task_request(1, "tasks/get", "0123456789abcdef0123456789abcdef");
        answer = co_await send(request);
    });
    EXPECT_EQ(answer["error"]["code"], mcp::constants::jsonrpc::g_METHOD_NOT_FOUND) << answer;
}

TEST_F(TasksTest, ModeNeedsAKnownTool) {
    server_->enable_tasks();
    EXPECT_THROW(server_->set_tool_task_mode("missing", mcp::ToolTaskMode::eAuto),
                 std::invalid_argument);
    EXPECT_NO_THROW(server_->set_tool_task_mode("echo", mcp::ToolTaskMode::eNever));
}

TEST_F(TasksTest, AModeSetBeforeTasksAreEnabledTakesEffect) {
    server_->set_handler_executor(io_ctx_.get_executor());
    EXPECT_NO_THROW(server_->set_tool_task_mode("echo", mcp::ToolTaskMode::eAuto));
    mcp::TaskOptions options;
    options.warning_handler = [this](const std::string& text) { warnings_.push_back(text); };
    server_->enable_tasks(options);
    json response;
    run([&]() -> mcp::Task<void> {
        const json request = call_tool(1, "echo");
        response = co_await send(request);
    });
    EXPECT_EQ(response["result"]["resultType"], "task") << response;
    EXPECT_TRUE(warnings_.empty());
}

TEST_F(TasksTest, TasksCannotBeEnabledOnceTheServerServes) {
    json answer;
    run([&]() -> mcp::Task<void> {
        const auto discover = make_discover_request(1);
        answer = co_await send(discover);
    });
    EXPECT_THROW(server_->enable_tasks(), std::logic_error);
}

TEST_F(TasksTest, ATaskToolWithoutAHandlerExecutorWarns) {
    mcp::TaskOptions options;
    options.warning_handler = [this](const std::string& text) { warnings_.push_back(text); };
    server_->enable_tasks(options);
    server_->set_tool_task_mode("echo", mcp::ToolTaskMode::eNever);
    EXPECT_TRUE(warnings_.empty());
    server_->set_tool_task_mode("echo", mcp::ToolTaskMode::eAuto);
    ASSERT_EQ(warnings_.size(), 1u);
    EXPECT_NE(warnings_[0].find("echo"), std::string::npos);
    EXPECT_NE(warnings_[0].find("handler executor"), std::string::npos);

    server_->set_handler_executor(io_ctx_.get_executor());
    server_->set_tool_task_mode("gated", mcp::ToolTaskMode::eAuto);
    EXPECT_EQ(warnings_.size(), 1u);
}

// ---- creation ----------------------------------------------------------------------------------

TEST_F(TasksTest, ADeclaringClientGetsATaskHandle) {
    mcp::TaskOptions options;
    options.ttl = std::chrono::milliseconds(60000);
    options.poll_interval = std::chrono::milliseconds(5000);
    enable(options);
    json response;
    run([&]() -> mcp::Task<void> {
        const auto request = call_tool(1, "gated");
        response = co_await send(request);
        release_ = true;
    });
    ASSERT_TRUE(response.contains("result")) << response;
    const auto& result = response["result"];
    EXPECT_EQ(result["resultType"], "task");
    EXPECT_EQ(result["status"], "working");
    EXPECT_EQ(result["taskId"].get<std::string>().size(), 32u);
    EXPECT_EQ(result["ttlMs"], 60000);
    EXPECT_EQ(result["pollIntervalMs"], 5000);
    EXPECT_TRUE(result["createdAt"].is_string());
    EXPECT_TRUE(result["lastUpdatedAt"].is_string());
    EXPECT_FALSE(result.contains("content"));
}

TEST_F(TasksTest, TheTaskExistsInTheStoreWhenTheHandlerStarts) {
    auto store = std::make_shared<CountingStore>();
    mcp::TaskOptions options;
    options.store = store;
    enable(options);
    creates_probe_ = &store->creates;
    run([&]() -> mcp::Task<void> {
        const auto id = co_await start("gated");
        const bool started = co_await wait_until([this] { return started_.load(); });
        EXPECT_TRUE(started);
        EXPECT_FALSE(id.empty());
        release_ = true;
    });
    EXPECT_EQ(creates_at_start_.load(), 1);
}

TEST_F(TasksTest, AClientWithoutTheExtensionGetsTheResultOfAnAutoTool) {
    enable();
    json response;
    run([&]() -> mcp::Task<void> {
        const auto request = call_tool(1, "echo", json::object(), {{"a", 1}});
        response = co_await send(request);
    });
    ASSERT_TRUE(response.contains("result")) << response;
    EXPECT_EQ(response["result"]["resultType"], "complete") << response;
    EXPECT_EQ(response["result"]["structuredContent"]["echo"]["a"], 1);
}

TEST_F(TasksTest, ARequiredToolRefusesAClientWithoutTheExtension) {
    enable();
    json response;
    run([&]() -> mcp::Task<void> {
        const auto request = call_tool(1, "required", json::object());
        response = co_await send(request);
    });
    ASSERT_TRUE(response.contains("error")) << response;
    EXPECT_EQ(response["error"]["code"], mcp::constants::jsonrpc::g_MISSING_REQUIRED_CLIENT_CAPABILITY);
    EXPECT_EQ(response["error"]["data"]["requiredCapabilities"],
              (json{{"extensions", {{g_tasks, json::object()}}}}));
}

TEST_F(TasksTest, ARequiredToolGivesADeclaringClientATask) {
    enable();
    json response;
    run([&]() -> mcp::Task<void> {
        const auto request = call_tool(1, "required");
        response = co_await send(request);
    });
    EXPECT_EQ(response["result"]["resultType"], "task") << response;
}

TEST_F(TasksTest, AToolThatIsNeverATaskRunsInTheRequest) {
    enable();
    json response;
    run([&]() -> mcp::Task<void> {
        const auto request = call_tool(1, "never");
        response = co_await send(request);
    });
    EXPECT_EQ(response["result"]["resultType"], "complete") << response;
}

TEST_F(TasksTest, ALegacyClientNeverSeesATask) {
    enable();
    json auto_tool;
    json required_tool;
    run([&]() -> mcp::Task<void> {
        const auto plain = make_tool_call_request("1", "echo");
        auto_tool = co_await send(plain);
        const auto needs_tasks = make_tool_call_request("2", "required");
        required_tool = co_await send(needs_tasks);
    });
    ASSERT_TRUE(auto_tool.contains("result")) << auto_tool;
    EXPECT_FALSE(auto_tool["result"].contains("taskId"));
    EXPECT_FALSE(auto_tool["result"].contains("resultType"));
    EXPECT_EQ(required_tool["error"]["code"],
              mcp::constants::jsonrpc::g_MISSING_REQUIRED_CLIENT_CAPABILITY)
        << required_tool;
}

TEST_F(TasksTest, ALegacyClientCannotUseTheTaskMethods) {
    enable();
    json answer;
    run([&]() -> mcp::Task<void> {
        const json request = {{"jsonrpc", "2.0"},
                              {"id", 1},
                              {"method", "tasks/get"},
                              {"params", {{"taskId", "0123456789abcdef0123456789abcdef"}}}};
        answer = co_await send(request);
    });
    EXPECT_EQ(answer["error"]["code"], mcp::constants::jsonrpc::g_METHOD_NOT_FOUND) << answer;
}

TEST_F(TasksTest, TheRemovedCoreTaskMethodsStayUnknown) {
    enable();
    std::vector<json> answers;
    run([&]() -> mcp::Task<void> {
        for (const char* method : {"tasks/list", "tasks/result", "tasks/getPayload"}) {
            const json request = task_request(1, method, "0123456789abcdef0123456789abcdef");
            answers.push_back(co_await send(request));
        }
    });
    for (const auto& answer : answers) {
        EXPECT_EQ(answer["error"]["code"], mcp::constants::jsonrpc::g_METHOD_NOT_FOUND) << answer;
    }
}

// ---- polling -----------------------------------------------------------------------------------

TEST_F(TasksTest, ATaskIsFindableAtOnceAndCompletesWithTheToolResult) {
    enable();
    json working;
    json completed;
    run([&]() -> mcp::Task<void> {
        const auto id = co_await start("gated");
        const json request = task_request(2, "tasks/get", id);
        working = co_await send(request);
        release_ = true;
        completed = co_await task_in_status(id, "completed");
    });
    ASSERT_TRUE(working.contains("result")) << working;
    EXPECT_EQ(working["result"]["resultType"], "complete");
    EXPECT_EQ(working["result"]["status"], "working");
    EXPECT_FALSE(working["result"].contains("result"));
    EXPECT_EQ(completed["resultType"], "complete");
    EXPECT_EQ(completed["result"]["structuredContent"]["released"], true);
    EXPECT_EQ(completed["result"]["content"][0]["type"], "text");
}

TEST_F(TasksTest, AToolErrorCompletesTheTask) {
    enable();
    json completed;
    run([&]() -> mcp::Task<void> {
        const auto id = co_await start("tool_error");
        completed = co_await task_in_status(id, "completed");
    });
    EXPECT_EQ(completed["result"]["isError"], true) << completed;
    EXPECT_FALSE(completed.contains("error"));
}

TEST_F(TasksTest, AJsonRpcFailureFailsTheTask) {
    enable();
    json failed;
    run([&]() -> mcp::Task<void> {
        const auto id = co_await start("broken_output");
        failed = co_await task_in_status(id, "failed");
    });
    EXPECT_EQ(failed["error"]["code"], mcp::constants::jsonrpc::g_INTERNAL_ERROR) << failed;
    EXPECT_TRUE(failed["error"]["message"].is_string());
    EXPECT_TRUE(failed["statusMessage"].is_string());
    EXPECT_FALSE(failed.contains("result"));
}

TEST_F(TasksTest, MiddlewareRunsForATasksToolCall) {
    server_->use(
        [this](mcp::Context& ctx, const json& params, mcp::TypeErasedHandler next) -> mcp::Task<json> {
            middleware_runs_.fetch_add(1);
            co_return co_await next(ctx, params);
        });
    enable();
    json completed;
    run([&]() -> mcp::Task<void> {
        const auto id = co_await start("echo");
        completed = co_await task_in_status(id, "completed");
    });
    EXPECT_EQ(middleware_runs_.load(), 1);
    EXPECT_EQ(completed["result"]["structuredContent"]["echo"], json::object());
}

TEST_F(TasksTest, ATaskHandlerSendsNoLogMessages) {
    enable();
    std::vector<json> notifications;
    run([&]() -> mcp::Task<void> {
        mcp::RequestContext context;
        context.notifications = [&notifications](std::string_view message) -> mcp::Task<void> {
            notifications.push_back(json::parse(message));
            co_return;
        };
        auto request = call_tool(1, "logs");
        request["params"]["_meta"]["io.modelcontextprotocol/logLevel"] = "debug";
        request["params"]["_meta"]["progressToken"] = "p1";
        const auto wire = co_await server_->dispatch_request_direct(request, std::move(context));
        const auto id = json::parse(wire)["result"].value("taskId", "");
        co_await task_in_status(id, "completed");
    });
    EXPECT_TRUE(notifications.empty()) << json(notifications);
}

TEST_F(TasksTest, ProgressBecomesTheStatusMessage) {
    enable();
    json working;
    json synchronous;
    run([&]() -> mcp::Task<void> {
        const auto id = co_await start("progress");
        const bool started = co_await wait_until([this] { return started_.load(); });
        EXPECT_TRUE(started);
        working = co_await task_in_status(id, "working");
        started_ = false;
        const auto other = co_await start("progress_sync");
        co_await wait_until([this] { return started_.load(); });
        synchronous = co_await task_in_status(other, "working");
        release_ = true;
    });
    EXPECT_EQ(working["statusMessage"], "quarter done") << working;
    EXPECT_EQ(synchronous["statusMessage"], "Progress: 2 of 8") << synchronous;
}

TEST_F(TasksTest, TheSyncProgressCallDoesNothingOutsideATask) {
    enable();
    json response;
    run([&]() -> mcp::Task<void> {
        const auto request = call_tool(1, "sync_progress_outside", json::object());
        response = co_await send(request);
    });
    EXPECT_EQ(response["result"]["structuredContent"]["reported"], false) << response;
}

TEST_F(TasksTest, ATaskRunsOnTheHandlerExecutor) {
    server_->enable_tasks();
    asio::thread_pool pool(1);
    server_->set_handler_executor(pool.get_executor());
    server_->set_tool_task_mode("gated", mcp::ToolTaskMode::eAuto);
    json completed;
    std::thread::id test_thread = std::this_thread::get_id();
    run([&]() -> mcp::Task<void> {
        const auto id = co_await start("gated");
        release_ = true;
        completed = co_await task_in_status(id, "completed");
    });
    EXPECT_EQ(completed["status"], "completed");
    std::lock_guard lock(handler_thread_mutex_);
    EXPECT_NE(handler_thread_, test_thread);
    pool.join();
}

// ---- errors and gating -------------------------------------------------------------------------

TEST_F(TasksTest, TheTaskMethodsNeedTheDeclaredExtension) {
    enable();
    std::vector<json> answers;
    run([&]() -> mcp::Task<void> {
        const auto id = co_await start("gated");
        for (const char* method : {"tasks/get", "tasks/update", "tasks/cancel"}) {
            const json request =
                task_request(2, method, id, json::object(), {{"inputResponses", json::object()}});
            answers.push_back(co_await send(request));
        }
        release_ = true;
    });
    ASSERT_EQ(answers.size(), 3u);
    for (const auto& answer : answers) {
        EXPECT_EQ(answer["error"]["code"],
                  mcp::constants::jsonrpc::g_MISSING_REQUIRED_CLIENT_CAPABILITY)
            << answer;
        EXPECT_EQ(answer["error"]["data"]["requiredCapabilities"],
                  (json{{"extensions", {{g_tasks, json::object()}}}}));
    }
}

TEST_F(TasksTest, AnUnknownTaskIsInvalidParams) {
    enable();
    std::vector<json> answers;
    run([&]() -> mcp::Task<void> {
        for (const char* method : {"tasks/get", "tasks/update", "tasks/cancel"}) {
            const json request = task_request(2, method, "0123456789abcdef0123456789abcdef",
                                              tasks_client(), {{"inputResponses", json::object()}});
            answers.push_back(co_await send(request));
        }
    });
    for (const auto& answer : answers) {
        EXPECT_EQ(answer["error"]["code"], mcp::constants::jsonrpc::g_INVALID_PARAMS) << answer;
        EXPECT_EQ(answer["error"]["message"], "Failed to retrieve task: Task not found");
    }
}

TEST_F(TasksTest, MalformedTaskRequestsAreInvalidParams) {
    enable();
    std::vector<json> answers;
    run([&]() -> mcp::Task<void> {
        const auto id = co_await start("gated");
        const json empty = json::object();
        const json bad_id = {{"taskId", 7}};
        const json no_responses = {{"taskId", id}};
        const json bad_responses = {{"taskId", id}, {"inputResponses", "x"}};
        const json client = tasks_client();
        const json r2 = modern(2, "tasks/get", empty, client);
        const json r3 = modern(3, "tasks/get", bad_id, client);
        const json r4 = modern(4, "tasks/update", no_responses, client);
        const json r5 = modern(5, "tasks/update", bad_responses, client);
        answers.push_back(co_await send(r2));
        answers.push_back(co_await send(r3));
        answers.push_back(co_await send(r4));
        answers.push_back(co_await send(r5));
        release_ = true;
    });
    ASSERT_EQ(answers.size(), 4u);
    for (const auto& answer : answers) {
        EXPECT_EQ(answer["error"]["code"], mcp::constants::jsonrpc::g_INVALID_PARAMS) << answer;
    }
}

// ---- principals --------------------------------------------------------------------------------

TEST_F(TasksTest, ATaskBelongsToThePrincipalThatStartedIt) {
    enable();
    std::vector<json> foreign;
    json owned;
    json after;
    run([&]() -> mcp::Task<void> {
        const auto id = co_await start("gated", tasks_client(), "alice");
        for (const char* method : {"tasks/get", "tasks/update", "tasks/cancel"}) {
            const json request =
                task_request(2, method, id, tasks_client(), {{"inputResponses", json::object()}});
            foreign.push_back(co_await send(request, "bob"));
        }
        const json anonymous = task_request(3, "tasks/get", id);
        foreign.push_back(co_await send(anonymous));
        const json mine = task_request(4, "tasks/get", id);
        owned = co_await send(mine, "alice");
        // bob's cancel did nothing.
        after = co_await task_in_status(id, "working", tasks_client(), "alice");
        release_ = true;
    });
    ASSERT_EQ(foreign.size(), 4u);
    for (const auto& answer : foreign) {
        EXPECT_EQ(answer["error"]["code"], mcp::constants::jsonrpc::g_INVALID_PARAMS) << answer;
        EXPECT_EQ(answer["error"]["message"], "Failed to retrieve task: Task not found");
    }
    EXPECT_EQ(owned["result"]["status"], "working");
    EXPECT_EQ(after["status"], "working");
}

TEST_F(TasksTest, AnAnonymousCallerCanUseItsOwnTasks) {
    enable();
    json completed;
    run([&]() -> mcp::Task<void> {
        const auto id = co_await start("echo");
        completed = co_await task_in_status(id, "completed");
    });
    EXPECT_EQ(completed["status"], "completed");
}

// ---- cancellation ------------------------------------------------------------------------------

TEST_F(TasksTest, CancellingAcknowledgesAndStopsTheHandler) {
    enable();
    json ack;
    json cancelled;
    run([&]() -> mcp::Task<void> {
        const auto id = co_await start("cancellable");
        const bool started = co_await wait_until([this] { return started_.load(); });
        EXPECT_TRUE(started);
        const json request = task_request(2, "tasks/cancel", id);
        ack = co_await send(request);
        cancelled = co_await task_in_status(id, "cancelled");
        const bool stopped = co_await wait_until([this] { return saw_cancel_.load(); });
        EXPECT_TRUE(stopped);
        // The handler's late return does not change a cancelled task.
        auto executor = co_await asio::this_coro::executor;
        asio::steady_timer settle(executor, std::chrono::milliseconds(20));
        co_await settle.async_wait(asio::use_awaitable);
        cancelled = co_await task_in_status(id, "cancelled");
    });
    EXPECT_EQ(ack["result"]["resultType"], "complete") << ack;
    EXPECT_FALSE(ack["result"].contains("taskId")) << ack;
    EXPECT_EQ(cancelled["status"], "cancelled");
    EXPECT_FALSE(cancelled.contains("result"));
}

TEST_F(TasksTest, CancellingAFinishedTaskIsAcknowledgedAndChangesNothing) {
    enable();
    json ack;
    json after;
    run([&]() -> mcp::Task<void> {
        const auto id = co_await start("echo");
        co_await task_in_status(id, "completed");
        const json request = task_request(2, "tasks/cancel", id);
        ack = co_await send(request);
        after = co_await task_in_status(id, "completed");
    });
    EXPECT_TRUE(ack.contains("result")) << ack;
    EXPECT_EQ(after["status"], "completed");
}

TEST_F(TasksTest, CancellationDoesNotUseTheCancelledNotification) {
    enable();
    json after;
    run([&]() -> mcp::Task<void> {
        const auto id = co_await start("gated");
        const json notification = {{"jsonrpc", "2.0"},
                                   {"method", "notifications/cancelled"},
                                   {"params", {{"requestId", 100}}}};
        co_await server_->dispatch(notification);
        after = co_await task_in_status(id, "working");
        release_ = true;
    });
    EXPECT_EQ(after["status"], "working");
}

// ---- input -------------------------------------------------------------------------------------

TEST_F(TasksTest, ATaskAsksForInputThroughItsInputRequests) {
    enable();
    json waiting;
    json again;
    json ack;
    json completed;
    run([&]() -> mcp::Task<void> {
        const auto capabilities = tasks_client({{"elicitation", json::object()}});
        const auto id = co_await start("ask", capabilities);
        waiting = co_await task_in_status(id, "input_required", capabilities);
        const json poll = task_request(3, "tasks/get", id, capabilities);
        again = co_await send(poll);
        const auto key = waiting["inputRequests"].begin().key();
        const json update = task_request(4, "tasks/update", id, capabilities,
                                         {{"inputResponses", {{key, accept("Luca")}}}});
        ack = co_await send(update);
        completed = co_await task_in_status(id, "completed", capabilities);
    });
    ASSERT_EQ(waiting["inputRequests"].size(), 1u) << waiting;
    const auto& request = waiting["inputRequests"].begin().value();
    EXPECT_EQ(request["method"], "elicitation/create");
    EXPECT_EQ(request["params"]["message"], "Your name?");
    EXPECT_FALSE(request["params"].contains("elicitationId"));
    // The same snapshot comes back until the client answers.
    EXPECT_EQ(again["result"]["inputRequests"], waiting["inputRequests"]);
    EXPECT_EQ(ack["result"]["resultType"], "complete") << ack;
    EXPECT_EQ(completed["result"]["structuredContent"]["name"], "Luca") << completed;
}

TEST_F(TasksTest, AHandlerCanNameItsQuestion) {
    enable();
    json waiting;
    json completed;
    run([&]() -> mcp::Task<void> {
        const auto capabilities = tasks_client({{"elicitation", json::object()}});
        const auto id = co_await start("ask_named", capabilities);
        waiting = co_await task_in_status(id, "input_required", capabilities);
        const json update = task_request(4, "tasks/update", id, capabilities,
                                         {{"inputResponses", {{"user_name", accept("Luca")}}}});
        co_await send(update);
        completed = co_await task_in_status(id, "completed", capabilities);
    });
    ASSERT_EQ(waiting["inputRequests"].size(), 1u) << waiting;
    EXPECT_TRUE(waiting["inputRequests"].contains("user_name")) << waiting;
    EXPECT_EQ(completed["result"]["structuredContent"]["name"], "Luca") << completed;
}

TEST_F(TasksTest, AnEmptyQuestionKeyEndsTheToolWithAnError) {
    enable();
    json failed;
    run([&]() -> mcp::Task<void> {
        const auto capabilities = tasks_client({{"elicitation", json::object()}});
        const auto id = co_await start("ask_empty_key", capabilities);
        failed = co_await task_in_status(id, "completed", capabilities);
    });
    EXPECT_TRUE(failed["result"]["isError"].get<bool>()) << failed;
}

TEST_F(TasksTest, AnswersForKeysThatAreNotOutstandingAreIgnored) {
    enable();
    json after_stray;
    json after_duplicate;
    json completed;
    run([&]() -> mcp::Task<void> {
        const auto capabilities = tasks_client({{"elicitation", json::object()}});
        const auto id = co_await start("ask", capabilities);
        const auto waiting = co_await task_in_status(id, "input_required", capabilities);
        const auto key = waiting["inputRequests"].begin().key();
        const json stray = task_request(3, "tasks/update", id, capabilities,
                                        {{"inputResponses", {{"nope", accept("x")}}}});
        const auto stray_ack = co_await send(stray);
        EXPECT_TRUE(stray_ack.contains("result")) << stray_ack;
        after_stray = co_await task_in_status(id, "input_required", capabilities);
        const json update = task_request(4, "tasks/update", id, capabilities,
                                         {{"inputResponses", {{key, accept("Luca")}}}});
        co_await send(update);
        completed = co_await task_in_status(id, "completed", capabilities);
        // The key was answered once; answering it again changes nothing.
        const json again = task_request(5, "tasks/update", id, capabilities,
                                        {{"inputResponses", {{key, accept("Other")}}}});
        const auto again_ack = co_await send(again);
        EXPECT_TRUE(again_ack.contains("result")) << again_ack;
        after_duplicate = co_await task_in_status(id, "completed", capabilities);
    });
    EXPECT_EQ(after_stray["status"], "input_required");
    EXPECT_EQ(completed["result"]["structuredContent"]["name"], "Luca");
    EXPECT_EQ(after_duplicate["result"]["structuredContent"]["name"], "Luca");
}

TEST_F(TasksTest, InputRequestKeysAreNeverReused) {
    enable();
    json completed;
    std::set<std::string> keys;
    run([&]() -> mcp::Task<void> {
        const auto capabilities = tasks_client({{"elicitation", json::object()}});
        const auto id = co_await start("ask_twice", capabilities);
        for (int round = 0; round < 2; ++round) {
            // The key of the question that is outstanding and was not asked before.
            std::string fresh;
            const bool asked = co_await poll_until(
                [&] { return !fresh.empty(); },
                [&]() -> mcp::Task<void> {
                    const json poll = task_request(50, "tasks/get", id, capabilities);
                    const auto response = co_await send(poll);
                    const auto& result = response["result"];
                    if (result.value("status", "") == "input_required") {
                        for (const auto& entry : result["inputRequests"].items()) {
                            if (!keys.contains(entry.key())) {
                                fresh = entry.key();
                            }
                        }
                    }
                });
            EXPECT_TRUE(asked);
            keys.insert(fresh);
            const json update =
                task_request(10 + round, "tasks/update", id, capabilities,
                             {{"inputResponses", {{fresh, accept("n" + std::to_string(round))}}}});
            co_await send(update);
        }
        completed = co_await task_in_status(id, "completed", capabilities);
    });
    EXPECT_EQ(keys.size(), 2u);
    EXPECT_EQ(completed["result"]["structuredContent"]["first"], "n0") << completed;
    EXPECT_EQ(completed["result"]["structuredContent"]["second"], "n1") << completed;
}

TEST_F(TasksTest, ATaskCannotAskWhatTheClientDidNotDeclare) {
    enable();
    json completed;
    run([&]() -> mcp::Task<void> {
        // The client declares tasks but not elicitation.
        const auto id = co_await start("ask");
        completed = co_await task_in_status(id, "completed");
    });
    EXPECT_EQ(completed["result"]["isError"], true) << completed;
    const auto text = completed["result"]["content"][0]["text"].get<std::string>();
    EXPECT_NE(text.find("did not declare"), std::string::npos) << text;
}

TEST_F(TasksTest, CancellingATaskThatWaitsForInputEndsIt) {
    enable();
    json cancelled;
    run([&]() -> mcp::Task<void> {
        const auto capabilities = tasks_client({{"elicitation", json::object()}});
        const auto id = co_await start("ask", capabilities);
        co_await task_in_status(id, "input_required", capabilities);
        const json request = task_request(3, "tasks/cancel", id, capabilities);
        co_await send(request);
        cancelled = co_await task_in_status(id, "cancelled", capabilities);
    });
    EXPECT_EQ(cancelled["status"], "cancelled");
    EXPECT_FALSE(cancelled.contains("inputRequests"));
}

TEST_F(TasksTest, CancellingATaskThatWaitsForInputRecordsNoFailure) {
    auto store = std::make_shared<CountingStore>();
    mcp::TaskOptions options;
    options.store = store;
    enable(options);
    asio::thread_pool pool(2);
    server_->set_handler_executor(pool.get_executor());
    // The cancel waits, in the store, for a failure that would race it: a handler that unwinds
    // because it was told to stop must not record one.
    CountingStore* const counting = store.get();
    store->before_cancel = [counting] {
        const auto give_up = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
        while (counting->fails.load() == 0 && std::chrono::steady_clock::now() < give_up) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };
    json after;
    run([&]() -> mcp::Task<void> {
        const auto capabilities = tasks_client({{"elicitation", json::object()}});
        const auto id = co_await start("ask", capabilities);
        co_await task_in_status(id, "input_required", capabilities);
        const json cancel = task_request(3, "tasks/cancel", id, capabilities);
        co_await send(cancel);
        auto executor = co_await asio::this_coro::executor;
        asio::steady_timer settle(executor, std::chrono::milliseconds(50));
        co_await settle.async_wait(asio::use_awaitable);
        const json get = task_request(4, "tasks/get", id, capabilities);
        after = (co_await send(get))["result"];
    });
    pool.join();
    EXPECT_EQ(after["status"], "cancelled") << after;
    EXPECT_EQ(store->fails.load(), 0);
}

// ---- store -------------------------------------------------------------------------------------

TEST_F(TasksTest, TheServerKeepsItsTasksInTheStoreItIsGiven) {
    auto store = std::make_shared<CountingStore>();
    mcp::TaskOptions options;
    options.store = store;
    enable(options);
    json completed;
    run([&]() -> mcp::Task<void> {
        const auto id = co_await start("echo");
        completed = co_await task_in_status(id, "completed");
    });
    EXPECT_EQ(store->creates.load(), 1);
    EXPECT_EQ(store->completes.load(), 1);
    EXPECT_GE(store->gets.load(), 1);
    EXPECT_EQ(completed["status"], "completed");
}

TEST_F(TasksTest, ATaskExpiresWithItsTimeToLive) {
    auto now = std::make_shared<std::atomic<std::int64_t>>(0);
    mcp::InMemoryTaskStoreOptions store_options;
    store_options.clock = [now] {
        return std::chrono::system_clock::time_point(std::chrono::milliseconds(now->load()));
    };
    mcp::TaskOptions options;
    options.store = std::make_shared<mcp::InMemoryTaskStore>(store_options);
    options.ttl = std::chrono::milliseconds(1000);
    enable(options);
    json before;
    json after;
    run([&]() -> mcp::Task<void> {
        const auto id = co_await start("gated");
        const json request = task_request(2, "tasks/get", id);
        before = co_await send(request);
        now->store(1000);
        after = co_await send(request);
        release_ = true;
    });
    EXPECT_EQ(before["result"]["ttlMs"], 1000);
    EXPECT_EQ(after["error"]["code"], mcp::constants::jsonrpc::g_INVALID_PARAMS) << after;
}

TEST_F(TasksTest, ATaskCanBeKeptWithoutALimitOnItsTime) {
    mcp::TaskOptions options;
    options.ttl = std::nullopt;
    enable(options);
    json response;
    run([&]() -> mcp::Task<void> {
        const auto request = call_tool(1, "gated");
        response = co_await send(request);
        release_ = true;
    });
    ASSERT_TRUE(response["result"].contains("ttlMs")) << response;
    EXPECT_TRUE(response["result"]["ttlMs"].is_null());
}

TEST_F(TasksTest, APrincipalCannotHoldMoreTasksThanTheLimit) {
    mcp::TaskOptions options;
    options.max_tasks_per_principal = 1;
    enable(options);
    json second;
    json other_principal;
    run([&]() -> mcp::Task<void> {
        co_await start("gated", tasks_client(), "alice");
        const json again = call_tool(2, "gated");
        second = co_await send(again, "alice");
        const json other = call_tool(3, "gated");
        other_principal = co_await send(other, "bob");
        release_ = true;
    });
    EXPECT_EQ(second["error"]["code"], mcp::constants::jsonrpc::g_INTERNAL_ERROR) << second;
    EXPECT_EQ(other_principal["result"]["resultType"], "task") << other_principal;
}

TEST_F(TasksTest, ACancelledTaskKeepsItsSlotUntilItsHandlerReturns) {
    mcp::TaskOptions options;
    options.max_tasks_per_principal = 2;
    enable(options);
    std::vector<json> refused;
    json reopened;
    run([&]() -> mcp::Task<void> {
        std::vector<std::string> ids;
        for (int index = 0; index < 2; ++index) {
            ids.push_back(co_await start("gated", tasks_client(), "alice"));
        }
        for (const auto& id : ids) {
            const json cancel = task_request(200, "tasks/cancel", id);
            co_await send(cancel, "alice");
        }
        // The handlers ignore the cancellation, so each cancel-and-create cycle would add a handler
        // if a cancelled task freed its slot.
        for (int index = 0; index < 5; ++index) {
            const json again = call_tool(300 + index, "gated");
            refused.push_back(co_await send(again, "alice"));
        }
        release_ = true;
        const bool freed = co_await poll_until([&] { return reopened.contains("result"); },
                                               [&]() -> mcp::Task<void> {
                                                   const json again = call_tool(400, "gated");
                                                   reopened = co_await send(again, "alice");
                                               });
        EXPECT_TRUE(freed);
    });
    for (const auto& answer : refused) {
        EXPECT_EQ(answer["error"]["code"], mcp::constants::jsonrpc::g_INTERNAL_ERROR) << answer;
    }
    EXPECT_EQ(refused.size(), 5u);
}

TEST_F(TasksTest, TheLimitOnAllTasksCountsHandlersThatKeepRunning) {
    mcp::TaskOptions options;
    options.max_tasks = 2;
    enable(options);
    json refused;
    run([&]() -> mcp::Task<void> {
        const auto first = co_await start("gated", tasks_client(), "alice");
        const auto second = co_await start("gated", tasks_client(), "bob");
        for (const auto* id : {&first, &second}) {
            const json cancel = task_request(200, "tasks/cancel", *id);
            co_await send(cancel, id == &first ? "alice" : "bob");
        }
        const json again = call_tool(300, "gated");
        refused = co_await send(again, "carol");
        release_ = true;
    });
    EXPECT_EQ(refused["error"]["code"], mcp::constants::jsonrpc::g_INTERNAL_ERROR) << refused;
}

TEST_F(TasksTest, ATaskThatExpiresAsksItsHandlerToStop) {
    mcp::TaskOptions options;
    options.ttl = std::chrono::milliseconds(30);
    enable(options);
    run([&]() -> mcp::Task<void> {
        co_await start("cancellable");
        const bool stopped = co_await wait_until([this] { return saw_cancel_.load(); });
        EXPECT_TRUE(stopped);
    });
}

TEST_F(TasksTest, AnExpiredTaskKeepsItsSlotUntilItsHandlerReturns) {
    mcp::TaskOptions options;
    options.ttl = std::chrono::milliseconds(30);
    options.max_tasks_per_principal = 1;
    enable(options);
    json refused;
    json reopened;
    run([&]() -> mcp::Task<void> {
        const auto id = co_await start("gated");
        json state;
        const bool expired = co_await poll_until([&] { return state.contains("error"); },
                                                 [&]() -> mcp::Task<void> {
                                                     const json get = task_request(2, "tasks/get", id);
                                                     state = co_await send(get);
                                                 });
        EXPECT_TRUE(expired);
        const json again = call_tool(3, "gated");
        refused = co_await send(again);
        release_ = true;
        const bool freed = co_await poll_until([&] { return reopened.contains("result"); },
                                               [&]() -> mcp::Task<void> {
                                                   const json next = call_tool(4, "gated");
                                                   reopened = co_await send(next);
                                               });
        EXPECT_TRUE(freed);
    });
    EXPECT_EQ(refused["error"]["code"], mcp::constants::jsonrpc::g_INTERNAL_ERROR) << refused;
}

TEST_F(TasksTest, DestroyingTheServerCancelsARunningTask) {
    enable();
    run([&]() -> mcp::Task<void> {
        co_await start("cancellable");
        const bool started = co_await wait_until([this] { return started_.load(); });
        EXPECT_TRUE(started);
        server_.reset();
        const bool stopped = co_await wait_until([this] { return saw_cancel_.load(); });
        EXPECT_TRUE(stopped);
    });
}

// ---- task preparation --------------------------------------------------------------------------

/// @p request retried with @p state and @p responses under a new id.
json retried(json request, const json& id, const json& state, json responses) {
    request["id"] = id;
    request["params"]["requestState"] = state;
    request["params"]["inputResponses"] = std::move(responses);
    return request;
}

/// Asks for a name before the task exists, and checks the rounds. @p server answers both rounds.
void expect_prepared_call_rounds(const json& first, const json& second) {
    ASSERT_TRUE(first.contains("result")) << first;
    EXPECT_EQ(first["result"]["resultType"], "input_required") << first;
    EXPECT_TRUE(first["result"]["inputRequests"].contains("user_name")) << first;
    EXPECT_FALSE(first["result"].contains("taskId")) << first;
    ASSERT_TRUE(second.contains("result")) << second;
    EXPECT_EQ(second["result"]["resultType"], "task") << second;
    EXPECT_TRUE(second["result"]["taskId"].is_string()) << second;
    EXPECT_FALSE(second["result"].contains("requestState")) << second;
    EXPECT_FALSE(second["result"].contains("inputRequests")) << second;
}

TEST_F(TasksTest, ATaskToolCanAskForInputBeforeItsTaskExists) {
    prepare(asking_preparation());
    enable();
    json first;
    json second;
    json completed;
    run([&]() -> mcp::Task<void> {
        const auto capabilities = tasks_client({{"elicitation", json::object()}});
        const auto request = call_tool(1, "prepared", capabilities, {{"topic", "x"}});
        first = co_await send(request);
        const json answers = {{"user_name", accept("Luca")}};
        const auto retry = retried(request, 2, first["result"]["requestState"], answers);
        second = co_await send(retry);
        completed =
            co_await task_in_status(second["result"].value("taskId", ""), "completed", capabilities);
    });
    expect_prepared_call_rounds(first, second);
    EXPECT_EQ(completed["result"]["structuredContent"]["args"],
              (json{{"topic", "x"}, {"name", "Luca"}}))
        << completed;
    EXPECT_EQ(prepared_handler_runs_.load(), 1);
}

TEST_F(TasksTest, ASealedRequestCanAskForInputBeforeItsTaskExists) {
    mcp::MrtrOptions mrtr;
    mrtr.state_mode = mcp::MrtrStateMode::eSealed;
    mrtr.sealing_keys = {std::string(32, 'a')};
    server_->set_mrtr_options(mrtr);
    prepare(asking_preparation());
    enable();
    json first;
    json second;
    json completed;
    run([&]() -> mcp::Task<void> {
        const auto capabilities = tasks_client({{"elicitation", json::object()}});
        const auto request = call_tool(1, "prepared", capabilities, {{"topic", "x"}});
        first = co_await send(request);
        const json answers = {{"user_name", accept("Luca")}};
        const auto retry = retried(request, 2, first["result"]["requestState"], answers);
        second = co_await send(retry);
        completed =
            co_await task_in_status(second["result"].value("taskId", ""), "completed", capabilities);
    });
    expect_prepared_call_rounds(first, second);
    EXPECT_EQ(completed["result"]["structuredContent"]["args"],
              (json{{"topic", "x"}, {"name", "Luca"}}))
        << completed;
    EXPECT_EQ(prepared_handler_runs_.load(), 1);
}

TEST_F(TasksTest, ATaskPreparationCanAnswerTheCallWithoutATask) {
    prepare([this](mcp::Context&, const json&) -> mcp::Task<mcp::TaskPreparation> {
        ++preparations_;
        mcp::TaskPreparation preparation;
        mcp::CallToolResult result;
        result.content.push_back(mcp::TextContent{.text = "answered at once"});
        preparation.result = std::move(result);
        co_return preparation;
    });
    auto store = std::make_shared<CountingStore>();
    mcp::TaskOptions options;
    options.store = store;
    enable(std::move(options));
    json response;
    run([&]() -> mcp::Task<void> {
        const auto request = call_tool(1, "prepared");
        response = co_await send(request);
    });
    ASSERT_TRUE(response.contains("result")) << response;
    EXPECT_EQ(response["result"]["resultType"], "complete") << response;
    EXPECT_EQ(response["result"]["content"][0]["text"], "answered at once") << response;
    EXPECT_EQ(store->creates.load(), 0);
    EXPECT_EQ(prepared_handler_runs_.load(), 0);
}

TEST_F(TasksTest, ATaskPreparationIsNotCalledForAClientWithoutTheExtension) {
    prepare(asking_preparation());
    enable();
    json response;
    run([&]() -> mcp::Task<void> {
        const auto request = call_tool(1, "prepared", json::object(), {{"topic", "x"}});
        response = co_await send(request);
    });
    ASSERT_TRUE(response.contains("result")) << response;
    EXPECT_EQ(response["result"]["resultType"], "complete") << response;
    EXPECT_EQ(response["result"]["structuredContent"]["args"], (json{{"topic", "x"}})) << response;
    EXPECT_EQ(preparations_.load(), 0);
}

TEST_F(TasksTest, ATaskPreparationIsNotCalledWhenARequiredToolRefusesTheClient) {
    prepare(asking_preparation(), mcp::ToolTaskMode::eRequired);
    enable();
    json response;
    run([&]() -> mcp::Task<void> {
        const auto request = call_tool(1, "prepared", json::object());
        response = co_await send(request);
    });
    ASSERT_TRUE(response.contains("error")) << response;
    EXPECT_EQ(response["error"]["code"], mcp::constants::jsonrpc::g_MISSING_REQUIRED_CLIENT_CAPABILITY);
    EXPECT_EQ(preparations_.load(), 0);
}

// The refusal comes from the bundled auth middleware, which refuses a request without a token:
// the exception it throws for that is internal to the library.
TEST_F(TasksTest, MiddlewareCanRejectATaskPreparation) {
    server_->use(
        mcp::auth::make_auth_middleware([](const std::string&) -> mcp::Task<bool> { co_return true; }));
    prepare(asking_preparation());
    auto store = std::make_shared<CountingStore>();
    mcp::TaskOptions options;
    options.store = store;
    enable(std::move(options));
    json response;
    run([&]() -> mcp::Task<void> {
        const auto capabilities = tasks_client({{"elicitation", json::object()}});
        const auto request = call_tool(1, "prepared", capabilities);
        response = co_await send(request);
    });
    ASSERT_TRUE(response.contains("result")) << response;
    EXPECT_EQ(response["result"]["resultType"], "complete") << response;
    EXPECT_EQ(response["result"]["isError"], true) << response;
    EXPECT_EQ(response["result"]["content"][0]["text"], "Unauthorized: missing Bearer token")
        << response;
    EXPECT_EQ(preparations_.load(), 0);
    EXPECT_EQ(store->creates.load(), 0);
}

TEST_F(TasksTest, MiddlewareCanAnswerATaskPreparationItself) {
    server_->use([](mcp::Context&, const json&, mcp::TypeErasedHandler) -> mcp::Task<json> {
        co_return json{{"content", json::array({{{"type", "text"}, {"text", "from middleware"}}})}};
    });
    prepare(asking_preparation());
    auto store = std::make_shared<CountingStore>();
    mcp::TaskOptions options;
    options.store = store;
    enable(std::move(options));
    json response;
    run([&]() -> mcp::Task<void> {
        const auto capabilities = tasks_client({{"elicitation", json::object()}});
        const auto request = call_tool(1, "prepared", capabilities);
        response = co_await send(request);
    });
    ASSERT_TRUE(response.contains("result")) << response;
    EXPECT_EQ(response["result"]["resultType"], "complete") << response;
    EXPECT_EQ(response["result"]["content"][0]["text"], "from middleware") << response;
    EXPECT_EQ(preparations_.load(), 0);
    EXPECT_EQ(store->creates.load(), 0);
}

TEST_F(TasksTest, MiddlewareCanRewriteTheArgumentsOfATaskPreparation) {
    server_->use(
        [this](mcp::Context& ctx, const json& params, mcp::TypeErasedHandler next) -> mcp::Task<json> {
            middleware_runs_.fetch_add(1);
            json rewritten = params;
            rewritten["arguments"]["rewritten"] = true;
            co_return co_await next(ctx, rewritten);
        });
    json seen;
    prepare([this, &seen](mcp::Context&, const json& arguments) -> mcp::Task<mcp::TaskPreparation> {
        ++preparations_;
        seen = arguments;
        mcp::TaskPreparation preparation;
        preparation.arguments = arguments;
        co_return preparation;
    });
    enable();
    json completed;
    run([&]() -> mcp::Task<void> {
        const auto request = call_tool(1, "prepared", tasks_client(), {{"topic", "x"}});
        const auto response = co_await send(request);
        completed = co_await task_in_status(response["result"].value("taskId", ""), "completed");
    });
    EXPECT_EQ(seen, (json{{"topic", "x"}, {"rewritten", true}}));
    EXPECT_EQ(completed["result"]["structuredContent"]["args"], seen) << completed;
    // The preparation and the task's handler each pass through the middleware.
    EXPECT_EQ(middleware_runs_.load(), 2);
}

TEST_F(TasksTest, MiddlewareSeesAWellFormedResultWhenATaskPreparationReturnsArguments) {
    std::vector<json> seen;
    server_->use(
        [&seen](mcp::Context& ctx, const json& params, mcp::TypeErasedHandler next) -> mcp::Task<json> {
            auto result = co_await next(ctx, params);
            seen.push_back(result);
            co_return result;
        });
    prepare([this](mcp::Context&, const json& arguments) -> mcp::Task<mcp::TaskPreparation> {
        ++preparations_;
        mcp::TaskPreparation preparation;
        preparation.arguments = arguments;
        co_return preparation;
    });
    enable();
    run([&]() -> mcp::Task<void> {
        const auto request = call_tool(1, "prepared", tasks_client(), {{"topic", "x"}});
        const auto response = co_await send(request);
        co_await task_in_status(response["result"].value("taskId", ""), "completed");
    });
    ASSERT_FALSE(seen.empty());
    EXPECT_EQ(seen.front(), json(mcp::CallToolResult{})) << seen.front();
    EXPECT_TRUE(seen.front().contains("content")) << seen.front();
}

TEST_F(TasksTest, APrincipalAtTheTaskLimitIsAnsweredBeforeTheTaskPreparationRuns) {
    prepare(asking_preparation());
    mcp::TaskOptions options;
    options.max_tasks_per_principal = 1;
    enable(options);
    json second;
    run([&]() -> mcp::Task<void> {
        const json capabilities = tasks_client({{"elicitation", json::object()}});
        co_await start("gated", capabilities, "alice");
        const auto again = call_tool(2, "prepared", capabilities);
        second = co_await send(again, "alice");
        release_ = true;
    });
    ASSERT_TRUE(second.contains("error")) << second;
    EXPECT_EQ(second["error"]["code"], mcp::constants::jsonrpc::g_INTERNAL_ERROR) << second;
    EXPECT_EQ(preparations_.load(), 0);
}

TEST_F(TasksTest, ARequestCancelledDuringATaskPreparationCreatesNoTask) {
    auto cancelled = std::make_shared<std::atomic<bool>>(false);
    prepare([this, cancelled](mcp::Context&, const json& arguments) -> mcp::Task<mcp::TaskPreparation> {
        ++preparations_;
        cancelled->store(true);
        mcp::TaskPreparation preparation;
        preparation.arguments = arguments;
        co_return preparation;
    });
    auto store = std::make_shared<CountingStore>();
    mcp::TaskOptions options;
    options.store = store;
    enable(std::move(options));
    json response;
    run([&]() -> mcp::Task<void> {
        mcp::RequestContext context;
        context.cancelled = cancelled;
        const json request = call_tool(1, "prepared", tasks_client(), {{"topic", "x"}});
        const auto wire = co_await server_->dispatch_request_direct(request, std::move(context));
        response = json::parse(wire);
    });
    ASSERT_TRUE(response.contains("error")) << response;
    EXPECT_EQ(response["error"]["message"], "Request cancelled") << response;
    EXPECT_EQ(preparations_.load(), 1);
    EXPECT_EQ(store->creates.load(), 0);
    EXPECT_EQ(prepared_handler_runs_.load(), 0);
}

TEST_F(TasksTest, ATaskPreparationThatThrowsAnswersLikeAToolThatThrows) {
    prepare([this](mcp::Context&, const json&) -> mcp::Task<mcp::TaskPreparation> {
        ++preparations_;
        throw std::runtime_error("the preparation failed");
        co_return mcp::TaskPreparation{};
    });
    auto store = std::make_shared<CountingStore>();
    mcp::TaskOptions options;
    options.store = store;
    enable(std::move(options));
    json response;
    run([&]() -> mcp::Task<void> {
        const auto request = call_tool(1, "prepared");
        response = co_await send(request);
    });
    ASSERT_TRUE(response.contains("result")) << response;
    EXPECT_EQ(response["result"]["resultType"], "complete") << response;
    EXPECT_EQ(response["result"]["isError"], true) << response;
    EXPECT_EQ(response["result"]["content"][0]["text"], "the preparation failed") << response;
    EXPECT_EQ(store->creates.load(), 0);
}

TEST_F(TasksTest, ATaskPreparationCannotAskWhatTheClientDidNotDeclare) {
    prepare(asking_preparation());
    enable();
    json response;
    run([&]() -> mcp::Task<void> {
        const auto request = call_tool(1, "prepared", tasks_client());
        response = co_await send(request);
    });
    ASSERT_TRUE(response.contains("error")) << response;
    EXPECT_EQ(response["error"]["code"], mcp::constants::jsonrpc::g_MISSING_REQUIRED_CLIENT_CAPABILITY)
        << response;
    EXPECT_EQ(response["error"]["data"]["requiredCapabilities"],
              (json{{"elicitation", {{"form", json::object()}}}}));
}

TEST_F(TasksTest, ATaskPreparationSeesOnlyArgumentsThatConformToTheInputSchema) {
    server_->set_tool_task_preparation(
        "prepared_strict", [this](mcp::Context&, const json&) -> mcp::Task<mcp::TaskPreparation> {
            ++preparations_;
            co_return mcp::TaskPreparation{};
        });
    auto store = std::make_shared<CountingStore>();
    mcp::TaskOptions options;
    options.store = store;
    enable(std::move(options));
    server_->set_tool_task_mode("prepared_strict", mcp::ToolTaskMode::eAuto);
    json rejected;
    json synchronous;
    run([&]() -> mcp::Task<void> {
        const auto request = call_tool(1, "prepared_strict", tasks_client(), {{"n", "text"}});
        rejected = co_await send(request);
        const auto plain = call_tool(2, "prepared_strict", json::object(), {{"n", "text"}});
        synchronous = co_await send(plain);
    });
    ASSERT_TRUE(rejected.contains("result")) << rejected;
    EXPECT_EQ(rejected["result"]["resultType"], "complete") << rejected;
    EXPECT_EQ(rejected["result"]["isError"], true) << rejected;
    EXPECT_EQ(rejected["result"]["content"], synchronous["result"]["content"]);
    EXPECT_EQ(preparations_.load(), 0);
    EXPECT_EQ(store->creates.load(), 0);
    EXPECT_EQ(server_->tool_validation_stats().argument_rejections, 2u);
}

TEST_F(TasksTest, ATaskPreparationNeedsAKnownTool) {
    EXPECT_THROW(server_->set_tool_task_preparation(
                     "missing",
                     [](mcp::Context&, const json&) -> mcp::Task<mcp::TaskPreparation> {
                         co_return mcp::TaskPreparation{};
                     }),
                 std::invalid_argument);
}

TEST_F(TasksTest, ATaskPreparationCannotChangeOnceTheServerServes) {
    json answer;
    run([&]() -> mcp::Task<void> {
        const auto discover = make_discover_request(1);
        answer = co_await send(discover);
    });
    EXPECT_THROW(server_->set_tool_task_preparation(
                     "prepared",
                     [](mcp::Context&, const json&) -> mcp::Task<mcp::TaskPreparation> {
                         co_return mcp::TaskPreparation{};
                     }),
                 std::logic_error);
}

// ---- notifications over subscriptions/listen ---------------------------------------------------

class TaskListen : public TasksTest {
   protected:
    std::shared_ptr<ScriptedTransport> transport_;
    std::vector<json> written_;
    std::atomic<bool> run_done_{false};

    void start_session() {
        transport_ = std::make_shared<ScriptedTransport>(io_ctx_.get_executor());
        transport_->set_on_write(
            [this](std::string_view message) { written_.push_back(json::parse(message)); });
        asio::co_spawn(
            io_ctx_,
            [this]() -> mcp::Task<void> {
                try {
                    co_await server_->run(transport_, io_ctx_.get_executor());
                } catch (const std::exception&) {
                }
                run_done_.store(true);
            },
            asio::detached);
    }

    void send_message(const json& message) { transport_->enqueue_message(message.dump()); }

    mcp::Task<json> response_to(const json& id) {
        json found;
        co_await wait_until([&] {
            for (const auto& message : written_) {
                if (message.contains("id") && message["id"] == id && !message.contains("method")) {
                    found = message;
                    return true;
                }
            }
            return false;
        });
        co_return found;
    }

    std::vector<json> notifications(std::string_view method) const {
        std::vector<json> matching;
        for (const auto& message : written_) {
            if (message.value("method", "") == method) {
                matching.push_back(message);
            }
        }
        return matching;
    }

    /// Polls the task over the session until it has completed.
    mcp::Task<void> task_in_status_on_stream(const std::string& id) {
        json latest = json::object();
        for (int attempt = 0; attempt < 5000 && latest.value("status", "") != "completed"; ++attempt) {
            send_message(task_request(1000 + attempt, "tasks/get", id));
            const auto response = co_await response_to(1000 + attempt);
            latest = response.value("result", json::object());
        }
        EXPECT_EQ(latest.value("status", ""), "completed");
    }

    mcp::Task<void> finish() {
        transport_->close();
        const bool done = co_await wait_until([this] { return run_done_.load(); });
        EXPECT_TRUE(done);
    }
};

TEST_F(TaskListen, StatusNotificationsFollowTheTaskOnTheStream) {
    enable();
    start_session();
    json ack;
    json created;
    run([&]() -> mcp::Task<void> {
        send_message(call_tool(1, "gated"));
        created = co_await response_to(1);
        const auto id = created["result"]["taskId"].get<std::string>();
        send_message(modern(2, "subscriptions/listen",
                            {{"notifications", {{"taskIds", {id, "unknown"}}}}}, tasks_client()));
        const bool acknowledged = co_await wait_until(
            [this] { return !notifications("notifications/subscriptions/acknowledged").empty(); });
        EXPECT_TRUE(acknowledged);
        ack = notifications("notifications/subscriptions/acknowledged")[0];
        release_ = true;
        const bool notified = co_await wait_until([this] {
            const auto sent = notifications("notifications/tasks");
            return !sent.empty() && sent.back()["params"]["status"] == "completed";
        });
        EXPECT_TRUE(notified);
        co_await finish();
    });
    const auto id = created["result"]["taskId"];
    EXPECT_EQ(ack["params"]["notifications"]["taskIds"], json::array({id})) << ack;
    const auto sent = notifications("notifications/tasks");
    // The state at the acknowledgment comes first, then the change.
    ASSERT_EQ(sent.size(), 2u);
    EXPECT_EQ(sent[0]["params"]["status"], "working");
    const auto& params = sent[1]["params"];
    EXPECT_EQ(params["taskId"], id);
    EXPECT_EQ(params["status"], "completed");
    EXPECT_EQ(params["result"]["structuredContent"]["released"], true);
    EXPECT_EQ(params["_meta"]["io.modelcontextprotocol/subscriptionId"], 2);
}

TEST_F(TaskListen, ATaskThatFinishedBeforeTheStreamOpenedIsReportedAfterTheAcknowledgment) {
    enable();
    start_session();
    run([&]() -> mcp::Task<void> {
        send_message(call_tool(1, "echo"));
        const auto created = co_await response_to(1);
        const auto id = created["result"]["taskId"].get<std::string>();
        co_await task_in_status_on_stream(id);
        send_message(modern(2, "subscriptions/listen", {{"notifications", {{"taskIds", {id}}}}},
                            tasks_client()));
        const bool notified =
            co_await wait_until([this] { return !notifications("notifications/tasks").empty(); });
        EXPECT_TRUE(notified);
        co_await finish();
    });
    const auto sent = notifications("notifications/tasks");
    ASSERT_EQ(sent.size(), 1u);
    EXPECT_EQ(sent[0]["params"]["status"], "completed");
    EXPECT_EQ(sent[0]["params"]["result"]["structuredContent"]["echo"], json::object());
    // The acknowledgment is written before the state it precedes.
    std::size_t acknowledgment = written_.size();
    std::size_t notification = written_.size();
    for (std::size_t index = 0; index < written_.size(); ++index) {
        const auto method = written_[index].value("method", "");
        if (method == "notifications/subscriptions/acknowledged") {
            acknowledgment = index;
        } else if (method == "notifications/tasks") {
            notification = index;
        }
    }
    EXPECT_LT(acknowledgment, notification);
}

TEST_F(TaskListen, TheStreamCarriesNoProgressOrLogMessages) {
    enable();
    start_session();
    run([&]() -> mcp::Task<void> {
        send_message(call_tool(1, "progress"));
        const auto created = co_await response_to(1);
        const auto id = created["result"]["taskId"].get<std::string>();
        send_message(modern(2, "subscriptions/listen", {{"notifications", {{"taskIds", {id}}}}},
                            tasks_client()));
        co_await wait_until(
            [this] { return !notifications("notifications/subscriptions/acknowledged").empty(); });
        release_ = true;
        co_await wait_until([this] { return !notifications("notifications/tasks").empty(); });
        co_await finish();
    });
    EXPECT_TRUE(notifications("notifications/progress").empty());
    EXPECT_TRUE(notifications("notifications/message").empty());
}

TEST_F(TaskListen, OnlyTheTasksOfThePrincipalAreHonored) {
    enable();
    start_session();
    json ack;
    run([&]() -> mcp::Task<void> {
        // The session's requests carry no principal, so a task another principal owns is not theirs.
        const auto foreign = co_await start("gated", tasks_client(), "alice");
        send_message(modern(2, "subscriptions/listen", {{"notifications", {{"taskIds", {foreign}}}}},
                            tasks_client()));
        co_await wait_until(
            [this] { return !notifications("notifications/subscriptions/acknowledged").empty(); });
        ack = notifications("notifications/subscriptions/acknowledged")[0];
        release_ = true;
        co_await finish();
    });
    EXPECT_FALSE(ack["params"]["notifications"].contains("taskIds")) << ack;
}

TEST_F(TaskListen, AskingForTaskNotificationsNeedsTheExtension) {
    enable();
    start_session();
    json answer;
    run([&]() -> mcp::Task<void> {
        send_message(modern(1, "subscriptions/listen", {{"notifications", {{"taskIds", {"abc"}}}}},
                            json::object()));
        answer = co_await response_to(1);
        co_await finish();
    });
    EXPECT_EQ(answer["error"]["code"], mcp::constants::jsonrpc::g_MISSING_REQUIRED_CLIENT_CAPABILITY)
        << answer;
    EXPECT_EQ(answer["error"]["data"]["requiredCapabilities"],
              (json{{"extensions", {{g_tasks, json::object()}}}}));
}

TEST_F(TaskListen, TaskIdsMustBeAnArrayOfStrings) {
    enable();
    start_session();
    json answer;
    run([&]() -> mcp::Task<void> {
        send_message(modern(1, "subscriptions/listen", {{"notifications", {{"taskIds", "abc"}}}},
                            tasks_client()));
        answer = co_await response_to(1);
        co_await finish();
    });
    EXPECT_EQ(answer["error"]["code"], mcp::constants::jsonrpc::g_INVALID_PARAMS) << answer;
}

TEST_F(TaskListen, ASessionServesTasksOverItsOwnTransport) {
    enable();
    start_session();
    json completed;
    run([&]() -> mcp::Task<void> {
        send_message(call_tool(1, "echo"));
        const auto created = co_await response_to(1);
        const auto id = created["result"]["taskId"].get<std::string>();
        json latest = json::object();
        for (int attempt = 0; attempt < 5000 && latest.value("status", "") != "completed"; ++attempt) {
            send_message(task_request(100 + attempt, "tasks/get", id));
            const auto response = co_await response_to(100 + attempt);
            latest = response.value("result", json::object());
        }
        completed = latest;
        co_await finish();
    });
    EXPECT_EQ(completed["status"], "completed");
}

}  // namespace

}  // namespace server_tasks_test

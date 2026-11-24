#include "mcp/protocol/extensions.hpp"
#include "mcp/protocol/protocol.hpp"
#include "mcp/server/task_store.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <mcp/core/constants.hpp>
#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace tasks_test {

namespace {

using nlohmann::json;
using namespace std::chrono_literals;

// Each example below is copied unchanged from an example of the tasks extension specification
// (revision 2026-07-28): a result as it stands under `result`, the notification with its envelope.

const char* g_create_result = R"json({
    "resultType": "task",
    "taskId": "786512e2-9e0d-44bd-8f29-789f320fe840",
    "status": "working",
    "statusMessage": "The operation is now in progress.",
    "createdAt": "2025-11-25T10:30:00Z",
    "lastUpdatedAt": "2025-11-25T10:40:00Z",
    "ttlMs": 60000,
    "pollIntervalMs": 5000
})json";

const char* g_input_required = R"json({
    "resultType": "complete",
    "taskId": "786512e2-9e0d-44bd-8f29-789f320fe840",
    "status": "input_required",
    "createdAt": "2025-11-25T10:30:00Z",
    "lastUpdatedAt": "2025-11-25T10:50:00Z",
    "ttlMs": 3600000,
    "pollIntervalMs": 5000,
    "inputRequests": {
        "name": {
            "method": "elicitation/create",
            "params": {
                "mode": "form",
                "message": "Please enter your name.",
                "requestedSchema": {
                    "type": "object",
                    "properties": {"name": {"type": "string"}},
                    "required": ["name"]
                }
            }
        }
    }
})json";

const char* g_completed = R"json({
    "resultType": "complete",
    "taskId": "786512e2-9e0d-44bd-8f29-789f320fe840",
    "status": "completed",
    "createdAt": "2025-11-25T10:30:00Z",
    "lastUpdatedAt": "2025-11-25T10:50:00Z",
    "ttlMs": 3600000,
    "pollIntervalMs": 5000,
    "result": {"content": [{"type": "text", "text": "Hello, Luca!"}], "isError": false}
})json";

const char* g_failed = R"json({
    "resultType": "complete",
    "taskId": "786512e2-9e0d-44bd-8f29-789f820fe840",
    "status": "failed",
    "createdAt": "2025-11-25T10:30:00Z",
    "lastUpdatedAt": "2025-11-25T10:40:00Z",
    "ttlMs": 3600000,
    "statusMessage": "Tool execution failed: API rate limit exceeded",
    "error": {"code": -32603, "message": "API rate limit exceeded"}
})json";

const char* g_notification = R"json({
    "jsonrpc": "2.0",
    "method": "notifications/tasks",
    "params": {
        "taskId": "786512e2-9e0d-44bd-8f29-789f320fe840",
        "status": "completed",
        "createdAt": "2025-11-25T10:30:00Z",
        "lastUpdatedAt": "2025-11-25T10:50:00Z",
        "ttlMs": 60000,
        "pollIntervalMs": 5000,
        "result": {"content": [{"type": "text", "text": "Operation completed successfully."}],
                   "isError": false}
    }
})json";

}  // namespace

TEST(TasksWire, CreateTaskResultRoundTripsTheSpecExample) {
    const auto wire = json::parse(g_create_result);
    const auto created = wire.get<mcp::CreateTaskResult>();
    EXPECT_EQ(created.task.taskId, "786512e2-9e0d-44bd-8f29-789f320fe840");
    EXPECT_EQ(created.task.status, mcp::TaskStatus::eWorking);
    EXPECT_EQ(created.task.statusMessage, "The operation is now in progress.");
    EXPECT_EQ(created.task.ttlMs, 60000);
    EXPECT_EQ(created.task.pollIntervalMs, 5000);
    EXPECT_EQ(json(created), wire);
}

TEST(TasksWire, CreateTaskResultNeedsTheTaskDiscriminator) {
    auto wire = json::parse(g_create_result);
    wire["resultType"] = "complete";
    EXPECT_THROW(wire.get<mcp::CreateTaskResult>(), std::invalid_argument);
    wire.erase("resultType");
    EXPECT_THROW(wire.get<mcp::CreateTaskResult>(), std::invalid_argument);
}

TEST(TasksWire, TtlMayBeNullAndIsRequired) {
    auto wire = json::parse(g_create_result);
    wire["ttlMs"] = nullptr;
    const auto created = wire.get<mcp::CreateTaskResult>();
    EXPECT_FALSE(created.task.ttlMs.has_value());
    EXPECT_TRUE(json(created).at("ttlMs").is_null());

    wire.erase("ttlMs");
    EXPECT_THROW(wire.get<mcp::CreateTaskResult>(), json::exception);
}

TEST(TasksWire, InputRequiredTaskCarriesItsRequests) {
    const auto wire = json::parse(g_input_required);
    const auto task = wire.get<mcp::DetailedTask>();
    EXPECT_EQ(task.task.status, mcp::TaskStatus::eInputRequired);
    ASSERT_TRUE(task.inputRequests.has_value());
    EXPECT_EQ(task.inputRequests->at("name").at("method"), "elicitation/create");
    EXPECT_FALSE(task.result.has_value());
    EXPECT_FALSE(task.error.has_value());

    // resultType belongs to the response, so the task itself leaves it out.
    auto expected = wire;
    expected.erase("resultType");
    EXPECT_EQ(json(task), expected);
}

TEST(TasksWire, CompletedTaskCarriesItsResult) {
    const auto task = json::parse(g_completed).get<mcp::DetailedTask>();
    EXPECT_EQ(task.task.status, mcp::TaskStatus::eCompleted);
    ASSERT_TRUE(task.result.has_value());
    EXPECT_EQ(task.result->at("content").at(0).at("text"), "Hello, Luca!");
    EXPECT_FALSE(task.inputRequests.has_value());
}

TEST(TasksWire, FailedTaskCarriesItsJsonRpcError) {
    const auto task = json::parse(g_failed).get<mcp::DetailedTask>();
    EXPECT_EQ(task.task.status, mcp::TaskStatus::eFailed);
    ASSERT_TRUE(task.error.has_value());
    EXPECT_EQ(task.error->at("code"), -32603);
    EXPECT_EQ(task.task.statusMessage, "Tool execution failed: API rate limit exceeded");
    EXPECT_FALSE(task.task.pollIntervalMs.has_value());
}

TEST(TasksWire, ATaskWithoutThePayloadItsStatusNeedsIsRejected) {
    auto completed = json::parse(g_completed);
    completed.erase("result");
    EXPECT_THROW(completed.get<mcp::DetailedTask>(), std::invalid_argument);

    auto failed = json::parse(g_failed);
    failed.erase("error");
    EXPECT_THROW(failed.get<mcp::DetailedTask>(), std::invalid_argument);

    auto input_required = json::parse(g_input_required);
    input_required.erase("inputRequests");
    EXPECT_THROW(input_required.get<mcp::DetailedTask>(), std::invalid_argument);

    auto not_an_object = json::parse(g_completed);
    not_an_object["result"] = "text";
    EXPECT_THROW(not_an_object.get<mcp::DetailedTask>(), std::invalid_argument);
}

TEST(TasksWire, WorkingAndCancelledTasksCarryNoPayload) {
    for (const char* status : {"working", "cancelled"}) {
        auto wire = json::parse(g_create_result);
        wire["status"] = status;
        const auto task = wire.get<mcp::DetailedTask>();
        EXPECT_FALSE(task.inputRequests || task.result || task.error) << status;
    }
}

TEST(TasksWire, TheSerializedPayloadFollowsTheStatus) {
    mcp::DetailedTask task = json::parse(g_completed).get<mcp::DetailedTask>();
    task.error = json{{"code", 1}};
    task.task.status = mcp::TaskStatus::eWorking;
    const json working = task;
    EXPECT_FALSE(working.contains("result"));
    EXPECT_FALSE(working.contains("error"));
}

TEST(TasksWire, StatusNotificationRoundTripsTheSpecExample) {
    const auto wire = json::parse(g_notification);
    const auto notification = wire.get<mcp::TaskStatusNotification>();
    EXPECT_EQ(notification.method, "notifications/tasks");
    EXPECT_EQ(notification.params.task.status, mcp::TaskStatus::eCompleted);
    ASSERT_TRUE(notification.params.result.has_value());
    json round_trip = notification;
    round_trip["jsonrpc"] = "2.0";
    EXPECT_EQ(round_trip, wire);
}

TEST(TasksWire, TaskStatusDecodesOnlyTheSpecificationsStatuses) {
    for (const char* status : {"working", "input_required", "completed", "failed", "cancelled"}) {
        EXPECT_EQ(json(json(status).get<mcp::TaskStatus>()), json(status));
    }
    for (const json& value : {json(), json(3), json(true), json::object(), json::array(),
                              json("paused"), json(""), json("Working"), json("canceled")}) {
        SCOPED_TRACE(value.dump());
        EXPECT_THROW(value.get<mcp::TaskStatus>(), std::exception);
    }
}

// The decoders below are written by hand, so the generated peer-input matrix does not cover them
// (see test/core/json_matrix_manifest.json). Each member of each type is varied the way the matrix
// varies a generated one: absent, null, of the wrong JSON type, and, for the status, unknown.
namespace {

/// Whether decoding @p wire as T throws a JSON error or std::invalid_argument.
template <typename T>
bool decode_throws(const json& wire) {
    try {
        static_cast<void>(wire.get<T>());
        return false;
    } catch (const json::exception&) {
        return true;
    } catch (const std::invalid_argument&) {
        return true;
    }
}

/// One member of a document and how its decoder treats it.
struct Member {
    const char* key;
    bool required;    ///< A missing member throws.
    bool null_ok;     ///< An explicit null is accepted.
    json wrong_type;  ///< A value of a type the member cannot hold.
};

/// Checks every member of @p members against T, starting from the valid document @p baseline.
template <typename T>
void check_members(const json& baseline, const std::vector<Member>& members) {
    ASSERT_FALSE(decode_throws<T>(baseline)) << baseline;
    for (const auto& member : members) {
        SCOPED_TRACE(member.key);
        json absent = baseline;
        absent.erase(member.key);
        json null_valued = baseline;
        null_valued[member.key] = nullptr;
        json wrong = baseline;
        wrong[member.key] = member.wrong_type;
        EXPECT_EQ(decode_throws<T>(absent), member.required) << "absent";
        EXPECT_EQ(decode_throws<T>(null_valued), !member.null_ok) << "null";
        EXPECT_TRUE(decode_throws<T>(wrong)) << "wrong type";
    }
}

const std::vector<Member>& task_members() {
    static const std::vector<Member> members = {
        {"taskId", true, false, json(5)},
        {"status", true, false, json(5)},
        {"createdAt", true, false, json(5)},
        {"lastUpdatedAt", true, false, json(5)},
        {"ttlMs", true, true, json("soon")},
        {"statusMessage", false, true, json(5)},
        {"pollIntervalMs", false, true, json("soon")},
    };
    return members;
}

}  // namespace

TEST(TasksWire, TaskDataDecodesItsMembersStrictly) {
    check_members<mcp::TaskData>(json::parse(g_create_result), task_members());
    auto bad_status = json::parse(g_create_result);
    bad_status["status"] = "paused";
    EXPECT_TRUE(decode_throws<mcp::TaskData>(bad_status));
}

TEST(TasksWire, CreateTaskResultDecodesItsMembersStrictly) {
    check_members<mcp::CreateTaskResult>(json::parse(g_create_result), task_members());
    auto bad_status = json::parse(g_create_result);
    bad_status["status"] = "paused";
    EXPECT_TRUE(decode_throws<mcp::CreateTaskResult>(bad_status));
    for (const json& discriminator : {json(), json(1), json("complete")}) {
        auto wire = json::parse(g_create_result);
        wire["resultType"] = discriminator;
        EXPECT_TRUE(decode_throws<mcp::CreateTaskResult>(wire)) << discriminator;
    }
}

TEST(TasksWire, DetailedTaskDecodesItsMembersAndPayloadStrictly) {
    check_members<mcp::DetailedTask>(json::parse(g_completed), task_members());
    check_members<mcp::DetailedTask>(json::parse(g_completed), {{"result", true, false, json("text")}});
    check_members<mcp::DetailedTask>(json::parse(g_failed), {{"error", true, false, json("text")}});
    check_members<mcp::DetailedTask>(json::parse(g_input_required),
                                     {{"inputRequests", true, false, json("text")}});
    auto bad_status = json::parse(g_completed);
    bad_status["status"] = "paused";
    EXPECT_TRUE(decode_throws<mcp::DetailedTask>(bad_status));
}

TEST(TasksWire, StatusNotificationDecodesItsMembersStrictly) {
    const auto wire = json::parse(g_notification);
    ASSERT_FALSE(decode_throws<mcp::TaskStatusNotification>(wire));
    for (const char* key : {"method", "params"}) {
        SCOPED_TRACE(key);
        auto absent = wire;
        absent.erase(key);
        EXPECT_TRUE(decode_throws<mcp::TaskStatusNotification>(absent));
        auto null_valued = wire;
        null_valued[key] = nullptr;
        EXPECT_TRUE(decode_throws<mcp::TaskStatusNotification>(null_valued));
        auto wrong = wire;
        wrong[key] = 5;
        EXPECT_TRUE(decode_throws<mcp::TaskStatusNotification>(wrong));
    }
    auto bad_status = wire;
    bad_status["params"]["status"] = "paused";
    EXPECT_TRUE(decode_throws<mcp::TaskStatusNotification>(bad_status));
    auto bad_member = wire;
    bad_member["params"]["taskId"] = nullptr;
    EXPECT_TRUE(decode_throws<mcp::TaskStatusNotification>(bad_member));
}

TEST(TasksWire, RequestParamsUseTheTaskId) {
    const auto get = json::parse(R"({"taskId": "t1"})").get<mcp::GetTaskRequestParams>();
    EXPECT_EQ(get.taskId, "t1");
    const auto cancel = json::parse(R"({"taskId": "t1"})").get<mcp::CancelTaskRequestParams>();
    EXPECT_EQ(cancel.taskId, "t1");
    const auto update =
        json::parse(R"({"taskId": "t1", "inputResponses": {"name": {"action": "accept"}}})")
            .get<mcp::UpdateTaskRequestParams>();
    EXPECT_EQ(update.taskId, "t1");
    EXPECT_EQ(update.inputResponses.at("name").at("action"), "accept");
    EXPECT_THROW(json::parse(R"({"id": "t1"})").get<mcp::GetTaskRequestParams>(), json::exception);
}

TEST(TasksWire, ExtensionIdentifierIsReserved) {
    EXPECT_EQ(mcp::constants::tasks::g_EXTENSION_ID, "io.modelcontextprotocol/tasks");
}

// A peer of 2025-11-25 sends the experimental shapes: they parse and are dropped.
TEST(TasksWire, LegacyTaskShapesAreIgnored) {
    const auto tool = json::parse(R"({"name": "t", "inputSchema": {"type": "object"},
                                       "execution": {"taskSupport": "optional"}})")
                          .get<mcp::Tool>();
    EXPECT_EQ(tool.name, "t");
    EXPECT_FALSE(json(tool).contains("execution"));

    const auto elicit = json::parse(R"({"mode": "form", "message": "m", "requestedSchema": {},
                                         "task": {"ttl": 1000}})")
                            .get<mcp::ElicitRequestFormParams>();
    EXPECT_EQ(elicit.message, "m");
    EXPECT_FALSE(json(elicit).contains("task"));
}

TEST(Extensions, DeclareAndReadOnClientCapabilities) {
    mcp::ClientCapabilities capabilities;
    EXPECT_FALSE(mcp::declares_extension(capabilities, "io.example/ext"));
    EXPECT_FALSE(mcp::extension_settings(capabilities, "io.example/ext").has_value());

    mcp::declare_extension(capabilities, "io.example/ext");
    EXPECT_TRUE(mcp::declares_extension(capabilities, "io.example/ext"));
    EXPECT_EQ(mcp::extension_settings(capabilities, "io.example/ext").value(), json::object());

    mcp::declare_extension(capabilities, "io.example/ext", json{{"level", 2}});
    EXPECT_EQ(mcp::extension_settings(capabilities, "io.example/ext").value(), (json{{"level", 2}}));
    EXPECT_FALSE(mcp::declares_extension(capabilities, "io.example/other"));
    EXPECT_EQ(json(capabilities).at("extensions").at("io.example/ext"), (json{{"level", 2}}));
}

TEST(Extensions, DeclareAndReadOnServerCapabilities) {
    mcp::ServerCapabilities capabilities;
    mcp::declare_extension(capabilities, mcp::constants::tasks::g_EXTENSION_ID);
    EXPECT_TRUE(mcp::declares_extension(capabilities, mcp::constants::tasks::g_EXTENSION_ID));
    EXPECT_EQ(json(capabilities),
              (json{{"extensions", {{"io.modelcontextprotocol/tasks", json::object()}}}}));

    (*capabilities.extensions)["io.example/null"] = nullptr;
    EXPECT_EQ(mcp::extension_settings(capabilities, "io.example/null").value(), json::object());
}

TEST(Extensions, ReadOnTheMetadataOfARequest) {
    EXPECT_FALSE(
        mcp::declares_extension(static_cast<const mcp::RequestMetadata*>(nullptr), "io.example/ext"));
    EXPECT_FALSE(
        mcp::extension_settings(static_cast<const mcp::RequestMetadata*>(nullptr), "io.example/ext")
            .has_value());

    mcp::RequestMetadata meta;
    EXPECT_FALSE(mcp::declares_extension(&meta, "io.example/ext"));
    mcp::declare_extension(meta.clientCapabilities, "io.example/ext", json{{"a", 1}});
    EXPECT_TRUE(mcp::declares_extension(&meta, "io.example/ext"));
    EXPECT_EQ(mcp::extension_settings(&meta, "io.example/ext").value(), (json{{"a", 1}}));
}

namespace {

/// A clock the tests move by hand.
struct FakeClock {
    std::shared_ptr<std::atomic<std::int64_t>> millis = std::make_shared<std::atomic<std::int64_t>>(0);

    mcp::InMemoryTaskStoreOptions options() const {
        mcp::InMemoryTaskStoreOptions result;
        auto shared = millis;
        result.clock = [shared] {
            return std::chrono::system_clock::time_point(std::chrono::milliseconds(shared->load()));
        };
        return result;
    }

    void advance(std::chrono::milliseconds amount) { millis->fetch_add(amount.count()); }
};

}  // namespace

TEST(TaskTime, FormatsAsIso8601WithMilliseconds) {
    using namespace std::chrono;
    EXPECT_EQ(mcp::format_task_time(system_clock::time_point{}), "1970-01-01T00:00:00.000Z");
    // 2025-11-25T10:30:00.250Z
    const auto time = system_clock::time_point(milliseconds(1764066600250LL));
    EXPECT_EQ(mcp::format_task_time(time), "2025-11-25T10:30:00.250Z");
    // 2024-02-29T23:59:59.999Z, a leap day.
    EXPECT_EQ(mcp::format_task_time(system_clock::time_point(milliseconds(1709251199999LL))),
              "2024-02-29T23:59:59.999Z");
    EXPECT_EQ(mcp::format_task_time(system_clock::time_point(milliseconds(-1))),
              "1969-12-31T23:59:59.999Z");
}

TEST(TaskStore, CreatesAWorkingTaskThatGetFinds) {
    FakeClock clock;
    mcp::InMemoryTaskStore store(clock.options());
    const auto created = store.create("alice", std::chrono::hours(1), 2s);
    ASSERT_TRUE(created.has_value());
    EXPECT_EQ(created->status, mcp::TaskStatus::eWorking);
    EXPECT_EQ(created->owner, "alice");

    const auto found = store.get("alice", created->id);
    ASSERT_TRUE(found.has_value());
    const auto detailed = found->to_detailed();
    EXPECT_EQ(detailed.task.taskId, created->id);
    EXPECT_EQ(detailed.task.ttlMs, 3600000);
    EXPECT_EQ(detailed.task.pollIntervalMs, 2000);
    EXPECT_EQ(detailed.task.createdAt, "1970-01-01T00:00:00.000Z");
}

TEST(TaskStore, IdsAreRandomAndLongEnoughToGuessNever) {
    mcp::InMemoryTaskStore store;
    std::set<std::string> ids;
    for (int i = 0; i < 50; ++i) {
        const auto created = store.create("", std::nullopt, std::nullopt);
        ASSERT_TRUE(created.has_value());
        EXPECT_EQ(created->id.size(), 32u);
        EXPECT_EQ(created->id.find_first_not_of("0123456789abcdef"), std::string::npos);
        ids.insert(created->id);
    }
    EXPECT_EQ(ids.size(), 50u);
}

TEST(TaskStore, AnotherOwnerSeesNoDifferenceFromAnUnknownId) {
    mcp::InMemoryTaskStore store;
    const auto created = store.create("alice", std::nullopt, std::nullopt);
    ASSERT_TRUE(created.has_value());
    EXPECT_FALSE(store.get("bob", created->id).has_value());
    EXPECT_FALSE(store.get("", created->id).has_value());
    EXPECT_FALSE(store.get("alice", "0123456789abcdef0123456789abcdef").has_value());
    EXPECT_TRUE(store.get("alice", created->id).has_value());
}

TEST(TaskStore, AnAnonymousOwnerSharesOneScope) {
    mcp::InMemoryTaskStore store;
    const auto created = store.create("", std::nullopt, std::nullopt);
    ASSERT_TRUE(created.has_value());
    EXPECT_TRUE(store.get("", created->id).has_value());
    EXPECT_FALSE(store.get("alice", created->id).has_value());
}

TEST(TaskStore, ATaskExpiresAfterItsTimeToLive) {
    FakeClock clock;
    mcp::InMemoryTaskStore store(clock.options());
    const auto limited = store.create("alice", 1000ms, std::nullopt);
    const auto unlimited = store.create("alice", std::nullopt, std::nullopt);
    ASSERT_TRUE(limited && unlimited);

    clock.advance(999ms);
    EXPECT_TRUE(store.get("alice", limited->id).has_value());
    clock.advance(1ms);
    EXPECT_FALSE(store.get("alice", limited->id).has_value());
    EXPECT_FALSE(store.complete(limited->id, json::object()).has_value());
    clock.advance(std::chrono::hours(1000));
    EXPECT_TRUE(store.get("alice", unlimited->id).has_value());
    EXPECT_EQ(store.size(), 1u);
}

TEST(TaskStore, RefusesTasksPastTheLimitOfAnOwner) {
    mcp::InMemoryTaskStoreOptions options;
    options.max_tasks_per_owner = 2;
    mcp::InMemoryTaskStore store(options);
    ASSERT_TRUE(store.create("alice", std::nullopt, std::nullopt));
    ASSERT_TRUE(store.create("alice", std::nullopt, std::nullopt));
    EXPECT_FALSE(store.create("alice", std::nullopt, std::nullopt).has_value());
    // Another owner has a limit of its own.
    EXPECT_TRUE(store.create("bob", std::nullopt, std::nullopt).has_value());
}

// Expired tasks stop counting against their owner's limit even when nobody asked for them since.
TEST(TaskStore, ExpiredTasksMakeRoomUnderTheLimitOfAnOwner) {
    FakeClock clock;
    auto options = clock.options();
    options.max_tasks_per_owner = 2;
    mcp::InMemoryTaskStore store(options);
    ASSERT_TRUE(store.create("alice", 1000ms, std::nullopt));
    ASSERT_TRUE(store.create("alice", 2000ms, std::nullopt));
    EXPECT_FALSE(store.create("alice", std::nullopt, std::nullopt).has_value());

    clock.advance(1000ms);
    const auto replacement = store.create("alice", std::nullopt, std::nullopt);
    ASSERT_TRUE(replacement.has_value());
    EXPECT_FALSE(store.create("alice", std::nullopt, std::nullopt).has_value());
    EXPECT_EQ(store.size(), 2u);
    clock.advance(1000ms);
    EXPECT_EQ(store.size(), 1u);
    EXPECT_TRUE(store.get("alice", replacement->id).has_value());
}

TEST(TaskStore, AFinishedTaskMakesRoomForTheOldestFirst) {
    FakeClock clock;
    auto options = clock.options();
    options.max_tasks_per_owner = 2;
    mcp::InMemoryTaskStore store(options);
    const auto first = store.create("alice", std::nullopt, std::nullopt);
    clock.advance(1ms);
    const auto second = store.create("alice", std::nullopt, std::nullopt);
    ASSERT_TRUE(first && second);
    ASSERT_TRUE(store.complete(first->id, json::object()).has_value());
    clock.advance(1ms);
    ASSERT_TRUE(store.complete(second->id, json::object()).has_value());

    const auto third = store.create("alice", std::nullopt, std::nullopt);
    ASSERT_TRUE(third.has_value());
    EXPECT_FALSE(store.get("alice", first->id).has_value());
    EXPECT_TRUE(store.get("alice", second->id).has_value());
}

TEST(TaskStore, RefusesTasksPastTheLimitOfTheStore) {
    mcp::InMemoryTaskStoreOptions options;
    options.max_tasks = 2;
    mcp::InMemoryTaskStore store(options);
    ASSERT_TRUE(store.create("a", std::nullopt, std::nullopt));
    ASSERT_TRUE(store.create("b", std::nullopt, std::nullopt));
    EXPECT_FALSE(store.create("c", std::nullopt, std::nullopt).has_value());
}

TEST(TaskStore, CompleteFailAndCancelFinishATaskOnce) {
    mcp::InMemoryTaskStore store;
    const auto done = store.create("", std::nullopt, std::nullopt);
    const auto broken = store.create("", std::nullopt, std::nullopt);
    const auto stopped = store.create("", std::nullopt, std::nullopt);
    ASSERT_TRUE(done && broken && stopped);

    const auto completed = store.complete(done->id, json{{"content", json::array()}});
    ASSERT_TRUE(completed.has_value());
    EXPECT_EQ(completed->to_detailed().task.status, mcp::TaskStatus::eCompleted);
    EXPECT_EQ(completed->to_detailed().result.value(), (json{{"content", json::array()}}));

    const auto failed = store.fail(broken->id, json{{"code", -32603}, {"message", "boom"}}, "it broke");
    ASSERT_TRUE(failed.has_value());
    EXPECT_EQ(failed->to_detailed().task.statusMessage, "it broke");
    EXPECT_EQ(failed->to_detailed().error->at("message"), "boom");

    const auto cancelled = store.cancel(stopped->id, "stopped");
    ASSERT_TRUE(cancelled.has_value());
    EXPECT_EQ(cancelled->to_detailed().task.status, mcp::TaskStatus::eCancelled);

    // A finished task does not change again.
    EXPECT_FALSE(store.complete(done->id, json::object()).has_value());
    EXPECT_FALSE(store.fail(done->id, json::object(), "x").has_value());
    EXPECT_FALSE(store.cancel(done->id, "x").has_value());
    EXPECT_FALSE(store.complete(stopped->id, json::object()).has_value());
    EXPECT_FALSE(store.set_status_message(stopped->id, "late").has_value());
    EXPECT_EQ(store.get("", stopped->id)->status, mcp::TaskStatus::eCancelled);
}

TEST(TaskStore, StatusMessageUpdatesAndMovesLastUpdated) {
    FakeClock clock;
    mcp::InMemoryTaskStore store(clock.options());
    const auto created = store.create("", std::nullopt, std::nullopt);
    ASSERT_TRUE(created.has_value());
    clock.advance(250ms);
    const auto updated = store.set_status_message(created->id, "halfway");
    ASSERT_TRUE(updated.has_value());
    EXPECT_EQ(updated->status_message, "halfway");
    EXPECT_EQ(updated->to_detailed().task.lastUpdatedAt, "1970-01-01T00:00:00.250Z");
    EXPECT_EQ(updated->to_detailed().task.createdAt, "1970-01-01T00:00:00.000Z");
}

TEST(TaskStore, InputRequestsAndResponses) {
    mcp::InMemoryTaskStore store;
    const auto created = store.create("", std::nullopt, std::nullopt);
    ASSERT_TRUE(created.has_value());

    const json request = {{"method", "elicitation/create"}, {"params", {{"message", "Name?"}}}};
    const auto waiting = store.add_input_request(created->id, "input-1", request);
    ASSERT_TRUE(waiting.has_value());
    EXPECT_EQ(waiting->status, mcp::TaskStatus::eInputRequired);
    EXPECT_EQ(waiting->to_detailed().inputRequests->at("input-1"), request);
    EXPECT_FALSE(store.take_input_response(created->id, "input-1").has_value());

    const auto second = store.add_input_request(created->id, "input-2", request);
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second->input_requests.size(), 2u);

    // An answer for a key that is not outstanding is ignored.
    EXPECT_FALSE(
        store.submit_input_responses(created->id, json{{"nope", {{"action", "accept"}}}}).has_value());
    EXPECT_FALSE(store.submit_input_responses(created->id, json::array()).has_value());

    // A partial answer leaves the task waiting for the rest.
    const auto partial =
        store.submit_input_responses(created->id, json{{"input-1", {{"action", "accept"}}}});
    ASSERT_TRUE(partial.has_value());
    EXPECT_EQ(partial->status, mcp::TaskStatus::eInputRequired);
    EXPECT_EQ(partial->input_requests.size(), 1u);
    EXPECT_TRUE(partial->input_requests.contains("input-2"));

    // The key that was answered cannot be answered again.
    EXPECT_FALSE(store.submit_input_responses(created->id, json{{"input-1", {{"action", "decline"}}}})
                     .has_value());

    const auto done =
        store.submit_input_responses(created->id, json{{"input-2", {{"action", "decline"}}}});
    ASSERT_TRUE(done.has_value());
    EXPECT_EQ(done->status, mcp::TaskStatus::eWorking);
    EXPECT_FALSE(done->to_detailed().inputRequests.has_value());

    EXPECT_EQ(store.take_input_response(created->id, "input-1").value(), (json{{"action", "accept"}}));
    EXPECT_FALSE(store.take_input_response(created->id, "input-1").has_value());
    EXPECT_EQ(store.take_input_response(created->id, "input-2").value(), (json{{"action", "decline"}}));
}

TEST(TaskStore, CancellingDropsOutstandingRequests) {
    mcp::InMemoryTaskStore store;
    const auto created = store.create("", std::nullopt, std::nullopt);
    ASSERT_TRUE(created.has_value());
    ASSERT_TRUE(store.add_input_request(created->id, "input-1", json::object()).has_value());
    const auto cancelled = store.cancel(created->id, "stopped");
    ASSERT_TRUE(cancelled.has_value());
    EXPECT_TRUE(cancelled->input_requests.empty());
    EXPECT_FALSE(store.add_input_request(created->id, "input-2", json::object()).has_value());
}

TEST(TaskStore, ConcurrentCreatesRespectTheLimit) {
    mcp::InMemoryTaskStoreOptions options;
    options.max_tasks_per_owner = 40;
    mcp::InMemoryTaskStore store(options);
    std::atomic<int> accepted{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&store, &accepted] {
            for (int i = 0; i < 20; ++i) {
                if (store.create("alice", std::nullopt, std::nullopt)) {
                    ++accepted;
                }
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    EXPECT_EQ(accepted.load(), 40);
    EXPECT_EQ(store.size(), 40u);
}

}  // namespace tasks_test

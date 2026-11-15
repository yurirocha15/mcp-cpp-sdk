#pragma once

#include <mcp/core/constants.hpp>
#include <mcp/protocol/base.hpp>

#include <cstdint>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>

namespace mcp {

/**
 * @brief The state a task carries in every message that describes it.
 *
 * Named TaskData to avoid conflict with mcp::Task (boost::asio::awaitable) in core.hpp.
 */
struct TaskData {
    std::string taskId;                        ///< The task identifier.
    TaskStatus status = TaskStatus::eWorking;  ///< Current task state.
    std::optional<std::string> statusMessage;  ///< Human-readable message describing current state.
    std::string createdAt;                     ///< ISO 8601 timestamp when the task was created.
    std::string lastUpdatedAt;                 ///< ISO 8601 timestamp when the task was last updated.
    /// Time to live from creation in milliseconds; null (nullopt) means the task is kept
    /// without limit.
    std::optional<std::int64_t> ttlMs;
    std::optional<std::int64_t> pollIntervalMs;  ///< Suggested polling interval in milliseconds.
};

inline void to_json(nlohmann::json& json_obj, const TaskData& task) {
    json_obj = nlohmann::json{{"taskId", task.taskId},
                              {"status", task.status},
                              {"createdAt", task.createdAt},
                              {"lastUpdatedAt", task.lastUpdatedAt},
                              {"ttlMs", task.ttlMs ? nlohmann::json(*task.ttlMs) : nlohmann::json()}};
    if (task.statusMessage) {
        json_obj["statusMessage"] = *task.statusMessage;
    }
    if (task.pollIntervalMs) {
        json_obj["pollIntervalMs"] = *task.pollIntervalMs;
    }
}

inline void from_json(const nlohmann::json& json_obj, TaskData& task) {
    json_obj.at("taskId").get_to(task.taskId);
    json_obj.at("status").get_to(task.status);
    json_obj.at("createdAt").get_to(task.createdAt);
    json_obj.at("lastUpdatedAt").get_to(task.lastUpdatedAt);
    const auto& ttl = json_obj.at("ttlMs");
    task.ttlMs = ttl.is_null() ? std::nullopt : std::optional<std::int64_t>(ttl.get<std::int64_t>());
    task.statusMessage.reset();
    if (detail::has_json_value(json_obj, "statusMessage")) {
        task.statusMessage = json_obj.at("statusMessage").get<std::string>();
    }
    task.pollIntervalMs.reset();
    if (detail::has_json_value(json_obj, "pollIntervalMs")) {
        task.pollIntervalMs = json_obj.at("pollIntervalMs").get<std::int64_t>();
    }
}

/**
 * @brief A task together with the payload its status carries: the DetailedTask of the
 * specification, which `tasks/get` answers with and `notifications/tasks` carries.
 *
 * @details The status decides which payload member is set: `input_required` carries
 * `inputRequests`, `completed` the `result`, and `failed` the JSON-RPC `error`. Parsing a task
 * whose status lacks its payload member throws.
 */
struct DetailedTask {
    TaskData task;  ///< The task's id, status and timing members.
    /// The outstanding server-to-client requests, keyed by identifier, of an `input_required` task.
    std::optional<nlohmann::json> inputRequests;
    /// The result of the original request, of a `completed` task.
    std::optional<nlohmann::json> result;
    /// The JSON-RPC error object the request failed with, of a `failed` task.
    std::optional<nlohmann::json> error;
};

inline void to_json(nlohmann::json& json_obj, const DetailedTask& detailed) {
    json_obj = detailed.task;
    if (detailed.task.status == TaskStatus::eInputRequired && detailed.inputRequests) {
        json_obj["inputRequests"] = *detailed.inputRequests;
    }
    if (detailed.task.status == TaskStatus::eCompleted && detailed.result) {
        json_obj["result"] = *detailed.result;
    }
    if (detailed.task.status == TaskStatus::eFailed && detailed.error) {
        json_obj["error"] = *detailed.error;
    }
}

inline void from_json(const nlohmann::json& json_obj, DetailedTask& detailed) {
    json_obj.get_to(detailed.task);
    detailed.inputRequests.reset();
    detailed.result.reset();
    detailed.error.reset();
    const auto payload = [&json_obj](const char* name) -> const nlohmann::json& {
        const auto found = json_obj.find(name);
        if (found == json_obj.end() || !found->is_object()) {
            throw std::invalid_argument(std::string("task is missing its ") + name + " object");
        }
        return *found;
    };
    switch (detailed.task.status) {
        case TaskStatus::eInputRequired:
            detailed.inputRequests = payload("inputRequests");
            break;
        case TaskStatus::eCompleted:
            detailed.result = payload("result");
            break;
        case TaskStatus::eFailed:
            detailed.error = payload("error");
            break;
        case TaskStatus::eWorking:
        case TaskStatus::eCancelled:
            break;
    }
}

/// @brief The result of `tasks/get`.
using GetTaskResult = DetailedTask;

/**
 * @brief The result a server returns in place of a tool result to start a task.
 *
 * @details On the wire the task's members sit beside `resultType: "task"`.
 */
struct CreateTaskResult {
    TaskData task;  ///< The task that was created.
};

inline void to_json(nlohmann::json& json_obj, const CreateTaskResult& result) {
    json_obj = result.task;
    json_obj["resultType"] = mcp::constants::tasks::g_RESULT_TYPE_TASK;
}

inline void from_json(const nlohmann::json& json_obj, CreateTaskResult& result) {
    if (json_obj.value("resultType", "") != mcp::constants::tasks::g_RESULT_TYPE_TASK) {
        throw std::invalid_argument("result is not a task");
    }
    json_obj.get_to(result.task);
}

/// @brief Parameters of `tasks/get`.
struct GetTaskRequestParams {
    std::string taskId;  ///< The task to query.
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(GetTaskRequestParams, taskId)

/// @brief Parameters of `tasks/cancel`.
struct CancelTaskRequestParams {
    std::string taskId;  ///< The task to cancel.
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(CancelTaskRequestParams, taskId)

/// @brief Parameters of `tasks/update`.
struct UpdateTaskRequestParams {
    std::string taskId;                                    ///< The task to answer.
    std::map<std::string, nlohmann::json> inputResponses;  ///< Responses to its inputRequests, by key.
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(UpdateTaskRequestParams, taskId, inputResponses)

/**
 * @brief A notification that carries a task's current state, for a client that listened for it
 * with `notifications.taskIds` on subscriptions/listen.
 */
struct TaskStatusNotification {
    std::string method = "notifications/tasks";  ///< The method name.
    DetailedTask params;                         ///< The task as `tasks/get` would return it.
};

inline void to_json(nlohmann::json& json_obj, const TaskStatusNotification& notification) {
    json_obj = nlohmann::json{{"method", notification.method}, {"params", notification.params}};
}

inline void from_json(const nlohmann::json& json_obj, TaskStatusNotification& notification) {
    json_obj.at("method").get_to(notification.method);
    json_obj.at("params").get_to(notification.params);
}

}  // namespace mcp

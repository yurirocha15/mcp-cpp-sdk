#pragma once

#include <mcp/core/constants.hpp>
#include <mcp/core/export.hpp>
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

MCP_API void to_json(nlohmann::json& json_obj, const TaskData& task);

MCP_API void from_json(const nlohmann::json& json_obj, TaskData& task);

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

MCP_API void to_json(nlohmann::json& json_obj, const DetailedTask& detailed);

MCP_API void from_json(const nlohmann::json& json_obj, DetailedTask& detailed);

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

MCP_API void to_json(nlohmann::json& json_obj, const CreateTaskResult& result);

MCP_API void from_json(const nlohmann::json& json_obj, CreateTaskResult& result);

/// @brief Parameters of `tasks/get`.
struct GetTaskRequestParams {
    std::string taskId;  ///< The task to query.
};
MCP_API void to_json(nlohmann::json& json_obj, const GetTaskRequestParams& value);
MCP_API void from_json(const nlohmann::json& json_obj, GetTaskRequestParams& value);

/// @brief Parameters of `tasks/cancel`.
struct CancelTaskRequestParams {
    std::string taskId;  ///< The task to cancel.
};
MCP_API void to_json(nlohmann::json& json_obj, const CancelTaskRequestParams& value);
MCP_API void from_json(const nlohmann::json& json_obj, CancelTaskRequestParams& value);

/// @brief Parameters of `tasks/update`.
struct UpdateTaskRequestParams {
    std::string taskId;                                    ///< The task to answer.
    std::map<std::string, nlohmann::json> inputResponses;  ///< Responses to its inputRequests, by key.
};
MCP_API void to_json(nlohmann::json& json_obj, const UpdateTaskRequestParams& value);
MCP_API void from_json(const nlohmann::json& json_obj, UpdateTaskRequestParams& value);

/**
 * @brief A notification that carries a task's current state, for a client that listened for it
 * with `notifications.taskIds` on subscriptions/listen.
 */
struct TaskStatusNotification {
    std::string method = "notifications/tasks";  ///< The method name.
    DetailedTask params;                         ///< The task as `tasks/get` would return it.
};

MCP_API void to_json(nlohmann::json& json_obj, const TaskStatusNotification& notification);

MCP_API void from_json(const nlohmann::json& json_obj, TaskStatusNotification& notification);

}  // namespace mcp

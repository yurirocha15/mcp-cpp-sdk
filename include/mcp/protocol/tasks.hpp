#pragma once

#include <mcp/protocol/base.hpp>

namespace mcp {

/**
 * @brief Data associated with a task.
 *
 * Named TaskData to avoid conflict with mcp::Task (boost::asio::awaitable) in core.hpp.
 */
struct TaskData {
    std::string taskId;                        ///< The task identifier.
    TaskStatus status;                         ///< Current task state.
    std::string createdAt;                     ///< ISO 8601 timestamp when the task was created.
    std::string lastUpdatedAt;                 ///< ISO 8601 timestamp when the task was last updated.
    int64_t ttl;                               ///< Retention duration from creation in milliseconds.
    std::optional<int64_t> pollInterval;       ///< Suggested polling interval in milliseconds.
    std::optional<std::string> statusMessage;  ///< Human-readable message describing current state.
};

inline void to_json(nlohmann::json& json_obj, const TaskData& task) {
    json_obj = nlohmann::json{{"taskId", task.taskId},
                              {"status", task.status},
                              {"createdAt", task.createdAt},
                              {"lastUpdatedAt", task.lastUpdatedAt},
                              {"ttl", task.ttl}};
    if (task.pollInterval) {
        json_obj["pollInterval"] = *task.pollInterval;
    }
    if (task.statusMessage) {
        json_obj["statusMessage"] = *task.statusMessage;
    }
}

inline void from_json(const nlohmann::json& json_obj, TaskData& task) {
    json_obj.at("taskId").get_to(task.taskId);
    json_obj.at("status").get_to(task.status);
    json_obj.at("createdAt").get_to(task.createdAt);
    json_obj.at("lastUpdatedAt").get_to(task.lastUpdatedAt);
    json_obj.at("ttl").get_to(task.ttl);
    if (json_obj.contains("pollInterval")) {
        task.pollInterval = json_obj.at("pollInterval").get<int64_t>();
    }
    if (json_obj.contains("statusMessage")) {
        task.statusMessage = json_obj.at("statusMessage").get<std::string>();
    }
}

/**
 * @brief The result returned for a task-augmented request.
 */
struct CreateTaskResult {
    TaskData task;                       ///< The task data.
    std::optional<nlohmann::json> meta;  ///< Reserved for protocol use.
};

inline void to_json(nlohmann::json& json_obj, const CreateTaskResult& result) {
    json_obj = nlohmann::json{{"task", result.task}};
    if (result.meta) {
        json_obj["_meta"] = *result.meta;
    }
}

inline void from_json(const nlohmann::json& json_obj, CreateTaskResult& result) {
    json_obj.at("task").get_to(result.task);
    if (json_obj.contains("_meta")) {
        result.meta = json_obj.at("_meta").get<nlohmann::json>();
    }
}

/**
 * @brief The result returned for a tasks/get request.
 *
 * Merges Result (_meta) and Task fields per schema allOf.
 */
struct GetTaskResult {
    std::string taskId;                        ///< The task identifier.
    TaskStatus status;                         ///< Current task state.
    std::string createdAt;                     ///< ISO 8601 timestamp when the task was created.
    std::string lastUpdatedAt;                 ///< ISO 8601 timestamp when the task was last updated.
    int64_t ttl;                               ///< Retention duration from creation in milliseconds.
    std::optional<int64_t> pollInterval;       ///< Suggested polling interval in milliseconds.
    std::optional<std::string> statusMessage;  ///< Human-readable message describing current state.
    std::optional<nlohmann::json> meta;        ///< Reserved for protocol use.
};

inline void to_json(nlohmann::json& json_obj, const GetTaskResult& result) {
    json_obj = nlohmann::json{{"taskId", result.taskId},
                              {"status", result.status},
                              {"createdAt", result.createdAt},
                              {"lastUpdatedAt", result.lastUpdatedAt},
                              {"ttl", result.ttl}};
    if (result.pollInterval) {
        json_obj["pollInterval"] = *result.pollInterval;
    }
    if (result.statusMessage) {
        json_obj["statusMessage"] = *result.statusMessage;
    }
    if (result.meta) {
        json_obj["_meta"] = *result.meta;
    }
}

inline void from_json(const nlohmann::json& json_obj, GetTaskResult& result) {
    json_obj.at("taskId").get_to(result.taskId);
    json_obj.at("status").get_to(result.status);
    json_obj.at("createdAt").get_to(result.createdAt);
    json_obj.at("lastUpdatedAt").get_to(result.lastUpdatedAt);
    json_obj.at("ttl").get_to(result.ttl);
    if (json_obj.contains("pollInterval")) {
        result.pollInterval = json_obj.at("pollInterval").get<int64_t>();
    }
    if (json_obj.contains("statusMessage")) {
        result.statusMessage = json_obj.at("statusMessage").get<std::string>();
    }
    if (json_obj.contains("_meta")) {
        result.meta = json_obj.at("_meta").get<nlohmann::json>();
    }
}

/**
 * @brief The result returned for a tasks/list request.
 */
struct ListTasksResult {
    std::vector<TaskData> tasks;            ///< The list of tasks.
    std::optional<std::string> nextCursor;  ///< Pagination position after last returned result.
    std::optional<nlohmann::json> meta;     ///< Reserved for protocol use.
};

inline void to_json(nlohmann::json& json_obj, const ListTasksResult& result) {
    json_obj = nlohmann::json{{"tasks", result.tasks}};
    if (result.nextCursor) {
        json_obj["nextCursor"] = *result.nextCursor;
    }
    if (result.meta) {
        json_obj["_meta"] = *result.meta;
    }
}

inline void from_json(const nlohmann::json& json_obj, ListTasksResult& result) {
    json_obj.at("tasks").get_to(result.tasks);
    if (json_obj.contains("nextCursor")) {
        result.nextCursor = json_obj.at("nextCursor").get<std::string>();
    }
    if (json_obj.contains("_meta")) {
        result.meta = json_obj.at("_meta").get<nlohmann::json>();
    }
}

/**
 * @brief Parameters for retrieving a task by ID.
 */
struct GetTaskRequestParams {
    std::string id;  ///< Task identifier to fetch.
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(GetTaskRequestParams, id)

/**
 * @brief A request to get a task by its ID.
 */
struct GetTaskRequest {
    std::string method = "tasks/get";  ///< The method name.
    GetTaskRequestParams params;       ///< The request parameters.
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(GetTaskRequest, method, params)

/**
 * @brief Parameters for cancelling a task.
 */
struct CancelTaskRequestParams {
    std::string id;  ///< Task identifier to cancel.
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(CancelTaskRequestParams, id)

/**
 * @brief A request to cancel a task.
 */
struct CancelTaskRequest {
    std::string method = "tasks/cancel";  ///< The method name.
    CancelTaskRequestParams params;       ///< The request parameters.
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(CancelTaskRequest, method, params)

/**
 * @brief Result of a task cancellation request.
 */
struct CancelTaskResult {
    std::optional<nlohmann::json> meta;  ///< Reserved for protocol use.
};
inline void to_json(nlohmann::json& j, const CancelTaskResult& r) {
    j = nlohmann::json::object();
    if (r.meta) {
        j["_meta"] = *r.meta;
    }
}
inline void from_json(const nlohmann::json& j, CancelTaskResult& r) {
    if (j.contains("_meta")) {
        r.meta = j.at("_meta");
    }
}

/**
 * @brief Parameters for listing tasks.
 */
struct ListTasksRequestParams {
    std::optional<std::string> cursor;  ///< Optional pagination cursor.
};
inline void to_json(nlohmann::json& j, const ListTasksRequestParams& p) {
    j = nlohmann::json::object();
    if (p.cursor) {
        j["cursor"] = *p.cursor;
    }
}
inline void from_json(const nlohmann::json& j, ListTasksRequestParams& p) {
    if (j.contains("cursor")) {
        p.cursor = j.at("cursor").get<std::string>();
    }
}

/**
 * @brief A request to list all tasks.
 */
struct ListTasksRequest {
    std::string method = "tasks/list";             ///< The method name.
    std::optional<ListTasksRequestParams> params;  ///< Optional request parameters.
};
inline void to_json(nlohmann::json& j, const ListTasksRequest& r) {
    j = nlohmann::json{{"method", r.method}};
    if (r.params) {
        j["params"] = *r.params;
    }
}
inline void from_json(const nlohmann::json& j, ListTasksRequest& r) {
    j.at("method").get_to(r.method);
    if (j.contains("params")) {
        r.params = j.at("params").get<ListTasksRequestParams>();
    }
}

/**
 * @brief Parameters for retrieving a task's payload.
 */
struct GetTaskPayloadRequestParams {
    std::string id;  ///< Task identifier whose payload should be fetched.
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(GetTaskPayloadRequestParams, id)

/**
 * @brief A request to get a task's payload.
 */
struct GetTaskPayloadRequest {
    std::string method = "tasks/getPayload";  ///< The method name.
    GetTaskPayloadRequestParams params;       ///< The request parameters.
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(GetTaskPayloadRequest, method, params)

/**
 * @brief Result containing a task's payload data.
 */
struct GetTaskPayloadResult {
    nlohmann::json payload;              ///< Payload associated with the task.
    std::optional<nlohmann::json> meta;  ///< Reserved for protocol use.
};
inline void to_json(nlohmann::json& j, const GetTaskPayloadResult& r) {
    j = nlohmann::json{{"payload", r.payload}};
    if (r.meta) {
        j["_meta"] = *r.meta;
    }
}
inline void from_json(const nlohmann::json& j, GetTaskPayloadResult& r) {
    j.at("payload").get_to(r.payload);
    if (j.contains("_meta")) {
        r.meta = j.at("_meta");
    }
}

}  // namespace mcp

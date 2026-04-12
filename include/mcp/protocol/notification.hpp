#pragma once

#include <mcp/protocol/base.hpp>
#include <nlohmann/json.hpp>

namespace mcp {

/**
 * @brief Severity threshold used for logging notifications.
 */
enum class LoggingLevel : std::uint8_t {
    Emergency,
    Alert,
    Critical,
    Error,
    Warning,
    Notice,
    Info,
    Debug
};

NLOHMANN_JSON_SERIALIZE_ENUM(LoggingLevel, {{LoggingLevel::Emergency, "emergency"},
                                            {LoggingLevel::Alert, "alert"},
                                            {LoggingLevel::Critical, "critical"},
                                            {LoggingLevel::Error, "error"},
                                            {LoggingLevel::Warning, "warning"},
                                            {LoggingLevel::Notice, "notice"},
                                            {LoggingLevel::Info, "info"},
                                            {LoggingLevel::Debug, "debug"}})

struct SetLevelRequestParams {
    LoggingLevel level;                  ///< The desired logging level.
    std::optional<nlohmann::json> meta;  ///< Reserved for protocol use.
};

inline void to_json(nlohmann::json& json_obj, const SetLevelRequestParams& params) {
    json_obj = nlohmann::json{{"level", params.level}};
    if (params.meta) {
        json_obj["_meta"] = *params.meta;
    }
}

inline void from_json(const nlohmann::json& json_obj, SetLevelRequestParams& params) {
    json_obj.at("level").get_to(params.level);
    if (json_obj.contains("_meta")) {
        params.meta = json_obj.at("_meta").get<nlohmann::json>();
    }
}

/**
 * @brief Request payload for the `logging/setLevel` method.
 */
struct SetLevelRequest {
    std::string method = "logging/setLevel";  ///< The method name.
    SetLevelRequestParams params;             ///< The request parameters.
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(SetLevelRequest, method, params)

/**
 * @brief An initialized notification.
 */
struct InitializedNotification {
    std::string method = "notifications/initialized";  ///< The method name.
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(InitializedNotification, method)

/**
 * @brief Parameters for a cancelled notification.
 */
struct CancelledNotificationParams {
    RequestId requestId;  ///< The ID of the request to cancel.
    std::optional<std::string>
        reason;  ///< An optional string describing the reason for the cancellation.
};

inline void to_json(nlohmann::json& j, const CancelledNotificationParams& params) {
    j = nlohmann::json{{"requestId", params.requestId}};
    if (params.reason) {
        j["reason"] = *params.reason;
    }
}

inline void from_json(const nlohmann::json& j, CancelledNotificationParams& params) {
    j.at("requestId").get_to(params.requestId);
    if (j.contains("reason")) {
        params.reason = j.at("reason").get<std::string>();
    }
}

/**
 * @brief A cancelled notification.
 */
struct CancelledNotification {
    std::string method = "notifications/cancelled";  ///< The method name.
    CancelledNotificationParams params;              ///< The notification parameters.
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(CancelledNotification, method, params)

/**
 * @brief Parameters for a progress notification.
 */
struct ProgressNotificationParams {
    ProgressToken progressToken;         ///< The progress token.
    double progress;                     ///< The progress value.
    std::optional<double> total;         ///< The total progress value.
    std::optional<std::string> message;  ///< An optional message.
};

inline void to_json(nlohmann::json& j, const ProgressNotificationParams& params) {
    j = nlohmann::json{{"progressToken", params.progressToken}, {"progress", params.progress}};
    if (params.total) {
        j["total"] = *params.total;
    }
    if (params.message) {
        j["message"] = *params.message;
    }
}

inline void from_json(const nlohmann::json& j, ProgressNotificationParams& params) {
    j.at("progressToken").get_to(params.progressToken);
    j.at("progress").get_to(params.progress);
    if (j.contains("total")) {
        params.total = j.at("total").get<double>();
    }
    if (j.contains("message")) {
        params.message = j.at("message").get<std::string>();
    }
}

/**
 * @brief A progress notification.
 */
struct ProgressNotification {
    std::string method = "notifications/progress";  ///< The method name.
    ProgressNotificationParams params;              ///< The notification parameters.
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ProgressNotification, method, params)

/**
 * @brief Parameters for a logging message notification.
 */
struct LoggingMessageNotificationParams {
    LoggingLevel level;                 ///< The severity of this log message.
    std::optional<std::string> logger;  ///< An optional name of the logger issuing this message.
    nlohmann::json data;  ///< The data to be logged, such as a string message or an object.
};

inline void to_json(nlohmann::json& j, const LoggingMessageNotificationParams& params) {
    j = nlohmann::json{{"level", params.level}, {"data", params.data}};
    if (params.logger) {
        j["logger"] = *params.logger;
    }
}

inline void from_json(const nlohmann::json& j, LoggingMessageNotificationParams& params) {
    j.at("level").get_to(params.level);
    j.at("data").get_to(params.data);
    if (j.contains("logger")) {
        params.logger = j.at("logger").get<std::string>();
    }
}

/**
 * @brief A logging message notification.
 */
struct LoggingMessageNotification {
    std::string method = "notifications/message";  ///< The method name.
    LoggingMessageNotificationParams params;       ///< The notification parameters.
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(LoggingMessageNotification, method, params)

/**
 * @brief A notification that the prompt list has changed.
 */
struct PromptListChangedNotification {
    std::string method = "notifications/prompts/list_changed";  ///< The method name.
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(PromptListChangedNotification, method)

/**
 * @brief A notification that the resource list has changed.
 */
struct ResourceListChangedNotification {
    std::string method = "notifications/resources/list_changed";  ///< The method name.
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ResourceListChangedNotification, method)

/**
 * @brief A notification that the tool list has changed.
 */
struct ToolListChangedNotification {
    std::string method = "notifications/tools/list_changed";  ///< The method name.
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ToolListChangedNotification, method)

/**
 * @brief A notification that the roots list has changed.
 */
struct RootsListChangedNotification {
    std::string method = "notifications/roots/list_changed";  ///< The method name.
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(RootsListChangedNotification, method)

/**
 * @brief Parameters for a resource updated notification.
 */
struct ResourceUpdatedNotificationParams {
    std::string uri;  ///< The URI of the resource that has been updated.
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ResourceUpdatedNotificationParams, uri)

/**
 * @brief A notification that a resource has been updated.
 */
struct ResourceUpdatedNotification {
    std::string method = "notifications/resources/updated";  ///< The method name.
    ResourceUpdatedNotificationParams params;                ///< The notification parameters.
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ResourceUpdatedNotification, method, params)

/**
 * @brief Parameters for task status notifications.
 */
struct TaskStatusNotificationParams {
    std::string id;                        ///< Task identifier whose status changed.
    TaskStatus status;                     ///< Updated task status.
    std::optional<TaskMetadata> metadata;  ///< Optional task metadata snapshot.
    std::optional<std::string> message;    ///< Optional human-readable status message.
};
inline void to_json(nlohmann::json& j, const TaskStatusNotificationParams& p) {
    j = nlohmann::json{{"id", p.id}, {"status", p.status}};
    if (p.metadata) {
        j["metadata"] = *p.metadata;
    }
    if (p.message) {
        j["message"] = *p.message;
    }
}
inline void from_json(const nlohmann::json& j, TaskStatusNotificationParams& p) {
    j.at("id").get_to(p.id);
    j.at("status").get_to(p.status);
    if (j.contains("metadata")) {
        p.metadata = j.at("metadata").get<TaskMetadata>();
    }
    if (j.contains("message")) {
        p.message = j.at("message").get<std::string>();
    }
}

/**
 * @brief A notification about a task's status change.
 */
struct TaskStatusNotification {
    std::string method = "notifications/tasks/status";  ///< The method name.
    TaskStatusNotificationParams params;                ///< The notification parameters.
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(TaskStatusNotification, method, params)

/**
 * @brief Parameters for elicitation completion notifications.
 */
struct ElicitationCompleteNotificationParams {
    RequestId requestId;  ///< Request identifier for the completed elicitation.
};
inline void to_json(nlohmann::json& j, const ElicitationCompleteNotificationParams& p) {
    j = nlohmann::json{{"requestId", p.requestId}};
}
inline void from_json(const nlohmann::json& j, ElicitationCompleteNotificationParams& p) {
    j.at("requestId").get_to(p.requestId);
}

/**
 * @brief A notification that an elicitation process has completed.
 */
struct ElicitationCompleteNotification {
    std::string method = "notifications/elicitation/complete";  ///< The method name.
    ElicitationCompleteNotificationParams params;               ///< The notification parameters.
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ElicitationCompleteNotification, method, params)

}  // namespace mcp

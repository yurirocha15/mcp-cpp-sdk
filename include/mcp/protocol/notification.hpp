#pragma once

#include <mcp/core/export.hpp>
#include <mcp/protocol/base.hpp>
#include <nlohmann/json.hpp>

namespace mcp {

/**
 * @brief Severity threshold used for logging notifications.
 */
enum class LoggingLevel : std::uint8_t {
    eEmergency,
    eAlert,
    eCritical,
    eError,
    eWarning,
    eNotice,
    eInfo,
    eDebug
};

NLOHMANN_JSON_SERIALIZE_ENUM(LoggingLevel, {{LoggingLevel::eEmergency, "emergency"},
                                            {LoggingLevel::eAlert, "alert"},
                                            {LoggingLevel::eCritical, "critical"},
                                            {LoggingLevel::eError, "error"},
                                            {LoggingLevel::eWarning, "warning"},
                                            {LoggingLevel::eNotice, "notice"},
                                            {LoggingLevel::eInfo, "info"},
                                            {LoggingLevel::eDebug, "debug"}})

struct SetLevelRequestParams {
    LoggingLevel level;                  ///< The desired logging level.
    std::optional<nlohmann::json> meta;  ///< Reserved for protocol use.
};

MCP_API void to_json(nlohmann::json& json_obj, const SetLevelRequestParams& params);

MCP_API void from_json(const nlohmann::json& json_obj, SetLevelRequestParams& params);

/**
 * @brief Request payload for the `logging/setLevel` method.
 */
struct SetLevelRequest {
    std::string method = "logging/setLevel";  ///< The method name.
    SetLevelRequestParams params;             ///< The request parameters.
};

MCP_API void to_json(nlohmann::json& json_obj, const SetLevelRequest& value);
MCP_API void from_json(const nlohmann::json& json_obj, SetLevelRequest& value);

/**
 * @brief An initialized notification.
 */
struct InitializedNotification {
    std::string method = "notifications/initialized";  ///< The method name.
};
MCP_API void to_json(nlohmann::json& json_obj, const InitializedNotification& value);
MCP_API void from_json(const nlohmann::json& json_obj, InitializedNotification& value);

/**
 * @brief Parameters for a cancelled notification.
 */
struct CancelledNotificationParams {
    RequestId requestId;  ///< The ID of the request to cancel.
    std::optional<std::string>
        reason;  ///< An optional string describing the reason for the cancellation.
};

MCP_API void to_json(nlohmann::json& j, const CancelledNotificationParams& params);

MCP_API void from_json(const nlohmann::json& j, CancelledNotificationParams& params);

/**
 * @brief A cancelled notification.
 */
struct CancelledNotification {
    std::string method = "notifications/cancelled";  ///< The method name.
    CancelledNotificationParams params;              ///< The notification parameters.
};
MCP_API void to_json(nlohmann::json& json_obj, const CancelledNotification& value);
MCP_API void from_json(const nlohmann::json& json_obj, CancelledNotification& value);

/**
 * @brief Parameters for a progress notification.
 */
struct ProgressNotificationParams {
    ProgressToken progressToken;         ///< The progress token.
    double progress;                     ///< The progress value.
    std::optional<double> total;         ///< The total progress value.
    std::optional<std::string> message;  ///< An optional message.
};

MCP_API void to_json(nlohmann::json& j, const ProgressNotificationParams& params);

MCP_API void from_json(const nlohmann::json& j, ProgressNotificationParams& params);

/**
 * @brief A progress notification.
 */
struct ProgressNotification {
    std::string method = "notifications/progress";  ///< The method name.
    ProgressNotificationParams params;              ///< The notification parameters.
};
MCP_API void to_json(nlohmann::json& json_obj, const ProgressNotification& value);
MCP_API void from_json(const nlohmann::json& json_obj, ProgressNotification& value);

/**
 * @brief Parameters for a logging message notification.
 */
struct LoggingMessageNotificationParams {
    LoggingLevel level;                 ///< The severity of this log message.
    std::optional<std::string> logger;  ///< An optional name of the logger issuing this message.
    nlohmann::json data;  ///< The data to be logged, such as a string message or an object.
};

MCP_API void to_json(nlohmann::json& j, const LoggingMessageNotificationParams& params);

MCP_API void from_json(const nlohmann::json& j, LoggingMessageNotificationParams& params);

/**
 * @brief A logging message notification.
 */
struct LoggingMessageNotification {
    std::string method = "notifications/message";  ///< The method name.
    LoggingMessageNotificationParams params;       ///< The notification parameters.
};
MCP_API void to_json(nlohmann::json& json_obj, const LoggingMessageNotification& value);
MCP_API void from_json(const nlohmann::json& json_obj, LoggingMessageNotification& value);

/**
 * @brief A notification that the prompt list has changed.
 */
struct PromptListChangedNotification {
    std::string method = "notifications/prompts/list_changed";  ///< The method name.
};
MCP_API void to_json(nlohmann::json& json_obj, const PromptListChangedNotification& value);
MCP_API void from_json(const nlohmann::json& json_obj, PromptListChangedNotification& value);

/**
 * @brief A notification that the resource list has changed.
 */
struct ResourceListChangedNotification {
    std::string method = "notifications/resources/list_changed";  ///< The method name.
};
MCP_API void to_json(nlohmann::json& json_obj, const ResourceListChangedNotification& value);
MCP_API void from_json(const nlohmann::json& json_obj, ResourceListChangedNotification& value);

/**
 * @brief A notification that the tool list has changed.
 */
struct ToolListChangedNotification {
    std::string method = "notifications/tools/list_changed";  ///< The method name.
};
MCP_API void to_json(nlohmann::json& json_obj, const ToolListChangedNotification& value);
MCP_API void from_json(const nlohmann::json& json_obj, ToolListChangedNotification& value);

/**
 * @brief A notification that the roots list has changed.
 */
struct RootsListChangedNotification {
    std::string method = "notifications/roots/list_changed";  ///< The method name.
};
MCP_API void to_json(nlohmann::json& json_obj, const RootsListChangedNotification& value);
MCP_API void from_json(const nlohmann::json& json_obj, RootsListChangedNotification& value);

/**
 * @brief Parameters for a resource updated notification.
 */
struct ResourceUpdatedNotificationParams {
    std::string uri;  ///< The URI of the resource that has been updated.
};
MCP_API void to_json(nlohmann::json& json_obj, const ResourceUpdatedNotificationParams& value);
MCP_API void from_json(const nlohmann::json& json_obj, ResourceUpdatedNotificationParams& value);

/**
 * @brief A notification that a resource has been updated.
 */
struct ResourceUpdatedNotification {
    std::string method = "notifications/resources/updated";  ///< The method name.
    ResourceUpdatedNotificationParams params;                ///< The notification parameters.
};
MCP_API void to_json(nlohmann::json& json_obj, const ResourceUpdatedNotification& value);
MCP_API void from_json(const nlohmann::json& json_obj, ResourceUpdatedNotification& value);

/**
 * @brief Parameters for elicitation completion notifications.
 */
struct ElicitationCompleteNotificationParams {
    /// The `elicitationId` of the URL-mode `elicitation/create` request that completed.
    std::string elicitationId;
};
MCP_API void to_json(nlohmann::json& j, const ElicitationCompleteNotificationParams& p);
MCP_API void from_json(const nlohmann::json& j, ElicitationCompleteNotificationParams& p);

/**
 * @brief A notification that an elicitation process has completed.
 */
struct ElicitationCompleteNotification {
    std::string method = "notifications/elicitation/complete";  ///< The method name.
    ElicitationCompleteNotificationParams params;               ///< The notification parameters.
};
MCP_API void to_json(nlohmann::json& json_obj, const ElicitationCompleteNotification& value);
MCP_API void from_json(const nlohmann::json& json_obj, ElicitationCompleteNotification& value);

}  // namespace mcp

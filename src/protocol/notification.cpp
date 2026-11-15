#include <mcp/protocol/notification.hpp>

#include <string>

namespace mcp {

void to_json(nlohmann::json& json_obj, const SetLevelRequestParams& params) {
    json_obj = nlohmann::json{{"level", params.level}};
    if (params.meta) {
        json_obj["_meta"] = *params.meta;
    }
}

void from_json(const nlohmann::json& json_obj, SetLevelRequestParams& params) {
    json_obj.at("level").get_to(params.level);
    if (detail::has_json_value(json_obj, "_meta")) {
        params.meta = json_obj.at("_meta").get<nlohmann::json>();
    }
}

void to_json(nlohmann::json& json_obj, const SetLevelRequest& value) {
    json_obj["method"] = value.method;
    json_obj["params"] = value.params;
}

void from_json(const nlohmann::json& json_obj, SetLevelRequest& value) {
    json_obj.at("method").get_to(value.method);
    json_obj.at("params").get_to(value.params);
}

void to_json(nlohmann::json& json_obj, const InitializedNotification& value) {
    json_obj["method"] = value.method;
}

void from_json(const nlohmann::json& json_obj, InitializedNotification& value) {
    json_obj.at("method").get_to(value.method);
}

void to_json(nlohmann::json& j, const CancelledNotificationParams& params) {
    j = nlohmann::json{{"requestId", params.requestId}};
    if (params.reason) {
        j["reason"] = *params.reason;
    }
}

void from_json(const nlohmann::json& j, CancelledNotificationParams& params) {
    j.at("requestId").get_to(params.requestId);
    if (detail::has_json_value(j, "reason")) {
        params.reason = j.at("reason").get<std::string>();
    }
}

void to_json(nlohmann::json& json_obj, const CancelledNotification& value) {
    json_obj["method"] = value.method;
    json_obj["params"] = value.params;
}

void from_json(const nlohmann::json& json_obj, CancelledNotification& value) {
    json_obj.at("method").get_to(value.method);
    json_obj.at("params").get_to(value.params);
}

void to_json(nlohmann::json& j, const ProgressNotificationParams& params) {
    j = nlohmann::json{{"progressToken", params.progressToken}, {"progress", params.progress}};
    if (params.total) {
        j["total"] = *params.total;
    }
    if (params.message) {
        j["message"] = *params.message;
    }
}

void from_json(const nlohmann::json& j, ProgressNotificationParams& params) {
    j.at("progressToken").get_to(params.progressToken);
    j.at("progress").get_to(params.progress);
    if (detail::has_json_value(j, "total")) {
        params.total = j.at("total").get<double>();
    }
    if (detail::has_json_value(j, "message")) {
        params.message = j.at("message").get<std::string>();
    }
}

void to_json(nlohmann::json& json_obj, const ProgressNotification& value) {
    json_obj["method"] = value.method;
    json_obj["params"] = value.params;
}

void from_json(const nlohmann::json& json_obj, ProgressNotification& value) {
    json_obj.at("method").get_to(value.method);
    json_obj.at("params").get_to(value.params);
}

void to_json(nlohmann::json& j, const LoggingMessageNotificationParams& params) {
    j = nlohmann::json{{"level", params.level}, {"data", params.data}};
    if (params.logger) {
        j["logger"] = *params.logger;
    }
}

void from_json(const nlohmann::json& j, LoggingMessageNotificationParams& params) {
    j.at("level").get_to(params.level);
    j.at("data").get_to(params.data);
    if (detail::has_json_value(j, "logger")) {
        params.logger = j.at("logger").get<std::string>();
    }
}

void to_json(nlohmann::json& json_obj, const LoggingMessageNotification& value) {
    json_obj["method"] = value.method;
    json_obj["params"] = value.params;
}

void from_json(const nlohmann::json& json_obj, LoggingMessageNotification& value) {
    json_obj.at("method").get_to(value.method);
    json_obj.at("params").get_to(value.params);
}

void to_json(nlohmann::json& json_obj, const PromptListChangedNotification& value) {
    json_obj["method"] = value.method;
}

void from_json(const nlohmann::json& json_obj, PromptListChangedNotification& value) {
    json_obj.at("method").get_to(value.method);
}

void to_json(nlohmann::json& json_obj, const ResourceListChangedNotification& value) {
    json_obj["method"] = value.method;
}

void from_json(const nlohmann::json& json_obj, ResourceListChangedNotification& value) {
    json_obj.at("method").get_to(value.method);
}

void to_json(nlohmann::json& json_obj, const ToolListChangedNotification& value) {
    json_obj["method"] = value.method;
}

void from_json(const nlohmann::json& json_obj, ToolListChangedNotification& value) {
    json_obj.at("method").get_to(value.method);
}

void to_json(nlohmann::json& json_obj, const RootsListChangedNotification& value) {
    json_obj["method"] = value.method;
}

void from_json(const nlohmann::json& json_obj, RootsListChangedNotification& value) {
    json_obj.at("method").get_to(value.method);
}

void to_json(nlohmann::json& json_obj, const ResourceUpdatedNotificationParams& value) {
    json_obj["uri"] = value.uri;
}

void from_json(const nlohmann::json& json_obj, ResourceUpdatedNotificationParams& value) {
    json_obj.at("uri").get_to(value.uri);
}

void to_json(nlohmann::json& json_obj, const ResourceUpdatedNotification& value) {
    json_obj["method"] = value.method;
    json_obj["params"] = value.params;
}

void from_json(const nlohmann::json& json_obj, ResourceUpdatedNotification& value) {
    json_obj.at("method").get_to(value.method);
    json_obj.at("params").get_to(value.params);
}

void to_json(nlohmann::json& j, const ElicitationCompleteNotificationParams& p) {
    j = nlohmann::json{{"elicitationId", p.elicitationId}};
}

void from_json(const nlohmann::json& j, ElicitationCompleteNotificationParams& p) {
    j.at("elicitationId").get_to(p.elicitationId);
}

void to_json(nlohmann::json& json_obj, const ElicitationCompleteNotification& value) {
    json_obj["method"] = value.method;
    json_obj["params"] = value.params;
}

void from_json(const nlohmann::json& json_obj, ElicitationCompleteNotification& value) {
    json_obj.at("method").get_to(value.method);
    json_obj.at("params").get_to(value.params);
}

}  // namespace mcp

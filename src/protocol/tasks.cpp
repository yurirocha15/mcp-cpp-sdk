#include <mcp/core/constants.hpp>
#include <mcp/protocol/tasks.hpp>

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>

namespace mcp {

void to_json(nlohmann::json& json_obj, const TaskData& task) {
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

void from_json(const nlohmann::json& json_obj, TaskData& task) {
    task = TaskData{};
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

void to_json(nlohmann::json& json_obj, const DetailedTask& detailed) {
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

void from_json(const nlohmann::json& json_obj, DetailedTask& detailed) {
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

void to_json(nlohmann::json& json_obj, const CreateTaskResult& result) {
    json_obj = result.task;
    json_obj["resultType"] = mcp::constants::tasks::g_RESULT_TYPE_TASK;
}

void from_json(const nlohmann::json& json_obj, CreateTaskResult& result) {
    result = CreateTaskResult{};
    if (json_obj.value("resultType", "") != mcp::constants::tasks::g_RESULT_TYPE_TASK) {
        throw std::invalid_argument("result is not a task");
    }
    json_obj.get_to(result.task);
}

void to_json(nlohmann::json& json_obj, const GetTaskRequestParams& value) {
    json_obj["taskId"] = value.taskId;
}

void from_json(const nlohmann::json& json_obj, GetTaskRequestParams& value) {
    json_obj.at("taskId").get_to(value.taskId);
}

void to_json(nlohmann::json& json_obj, const CancelTaskRequestParams& value) {
    json_obj["taskId"] = value.taskId;
}

void from_json(const nlohmann::json& json_obj, CancelTaskRequestParams& value) {
    json_obj.at("taskId").get_to(value.taskId);
}

void to_json(nlohmann::json& json_obj, const UpdateTaskRequestParams& value) {
    json_obj["taskId"] = value.taskId;
    json_obj["inputResponses"] = value.inputResponses;
}

void from_json(const nlohmann::json& json_obj, UpdateTaskRequestParams& value) {
    json_obj.at("taskId").get_to(value.taskId);
    json_obj.at("inputResponses").get_to(value.inputResponses);
}

void to_json(nlohmann::json& json_obj, const TaskStatusNotification& notification) {
    json_obj = nlohmann::json{{"method", notification.method}, {"params", notification.params}};
}

void from_json(const nlohmann::json& json_obj, TaskStatusNotification& notification) {
    json_obj.at("method").get_to(notification.method);
    json_obj.at("params").get_to(notification.params);
}

}  // namespace mcp

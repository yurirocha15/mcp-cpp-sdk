#include <mcp/protocol/sampling.hpp>

#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace mcp {

void to_json(nlohmann::json& json_obj, const ToolChoice& choice) {
    json_obj = nlohmann::json::object();
    if (choice.mode) {
        json_obj["mode"] = *choice.mode;
    }
}

void from_json(const nlohmann::json& json_obj, ToolChoice& choice) {
    choice = ToolChoice{};
    if (detail::has_json_value(json_obj, "mode")) {
        choice.mode = json_obj.at("mode").get<std::string>();
    }
}

void to_json(nlohmann::json& json_obj, const ModelHint& hint) {
    json_obj = nlohmann::json::object();
    if (hint.name) {
        json_obj["name"] = *hint.name;
    }
}

void from_json(const nlohmann::json& json_obj, ModelHint& hint) {
    hint = ModelHint{};
    if (detail::has_json_value(json_obj, "name")) {
        hint.name = json_obj.at("name").get<std::string>();
    }
}

void to_json(nlohmann::json& json_obj, const ModelPreferences& prefs) {
    json_obj = nlohmann::json::object();
    if (prefs.costPriority) {
        json_obj["costPriority"] = *prefs.costPriority;
    }
    if (prefs.hints) {
        json_obj["hints"] = *prefs.hints;
    }
    if (prefs.intelligencePriority) {
        json_obj["intelligencePriority"] = *prefs.intelligencePriority;
    }
    if (prefs.speedPriority) {
        json_obj["speedPriority"] = *prefs.speedPriority;
    }
}

void from_json(const nlohmann::json& json_obj, ModelPreferences& prefs) {
    prefs = ModelPreferences{};
    if (detail::has_json_value(json_obj, "costPriority")) {
        prefs.costPriority = json_obj.at("costPriority").get<double>();
    }
    if (detail::has_json_value(json_obj, "hints")) {
        prefs.hints = json_obj.at("hints").get<std::vector<ModelHint>>();
    }
    if (detail::has_json_value(json_obj, "intelligencePriority")) {
        prefs.intelligencePriority = json_obj.at("intelligencePriority").get<double>();
    }
    if (detail::has_json_value(json_obj, "speedPriority")) {
        prefs.speedPriority = json_obj.at("speedPriority").get<double>();
    }
}

void to_json(nlohmann::json& json_obj, const SamplingMessageContent& content) {
    std::visit(
        [&json_obj](auto&& arg) {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, std::vector<SamplingMessageContentBlock>>) {
                json_obj = nlohmann::json::array();
                for (const auto& block : arg) {
                    nlohmann::json block_json;
                    to_json(block_json, block);
                    json_obj.push_back(std::move(block_json));
                }
            } else {
                to_json(json_obj, arg);
            }
        },
        content);
}

void from_json(const nlohmann::json& json_obj, SamplingMessageContent& content) {
    if (json_obj.is_array()) {
        content = json_obj.get<std::vector<SamplingMessageContentBlock>>();
    } else {
        content = json_obj.get<SamplingMessageContentBlock>();
    }
}

void to_json(nlohmann::json& json_obj, const SamplingMessage& msg) {
    json_obj = nlohmann::json{{"role", msg.role}, {"content", msg.content}};
    if (msg.meta) {
        json_obj["_meta"] = *msg.meta;
    }
}

void from_json(const nlohmann::json& json_obj, SamplingMessage& msg) {
    msg = SamplingMessage{};
    json_obj.at("role").get_to(msg.role);
    json_obj.at("content").get_to(msg.content);
    if (detail::has_json_value(json_obj, "_meta")) {
        msg.meta = json_obj.at("_meta").get<nlohmann::json>();
    }
}

void to_json(nlohmann::json& json_obj, const CreateMessageRequestParams& params) {
    json_obj = nlohmann::json{{"messages", params.messages}, {"maxTokens", params.maxTokens}};
    if (params.systemPrompt) {
        json_obj["systemPrompt"] = *params.systemPrompt;
    }
    if (params.temperature) {
        json_obj["temperature"] = *params.temperature;
    }
    if (params.stopSequences) {
        json_obj["stopSequences"] = *params.stopSequences;
    }
    if (params.modelPreferences) {
        json_obj["modelPreferences"] = *params.modelPreferences;
    }
    if (params.includeContext) {
        json_obj["includeContext"] = *params.includeContext;
    }
    if (params.tools) {
        json_obj["tools"] = *params.tools;
    }
    if (params.toolChoice) {
        json_obj["toolChoice"] = *params.toolChoice;
    }
    if (params.metadata) {
        json_obj["metadata"] = *params.metadata;
    }
    if (params.meta) {
        json_obj["_meta"] = *params.meta;
    }
}

void from_json(const nlohmann::json& json_obj, CreateMessageRequestParams& params) {
    params = CreateMessageRequestParams{};
    json_obj.at("messages").get_to(params.messages);
    json_obj.at("maxTokens").get_to(params.maxTokens);
    if (detail::has_json_value(json_obj, "systemPrompt")) {
        params.systemPrompt = json_obj.at("systemPrompt").get<std::string>();
    }
    if (detail::has_json_value(json_obj, "temperature")) {
        params.temperature = json_obj.at("temperature").get<double>();
    }
    if (detail::has_json_value(json_obj, "stopSequences")) {
        params.stopSequences = json_obj.at("stopSequences").get<std::vector<std::string>>();
    }
    if (detail::has_json_value(json_obj, "modelPreferences")) {
        params.modelPreferences = json_obj.at("modelPreferences").get<ModelPreferences>();
    }
    if (detail::has_json_value(json_obj, "includeContext")) {
        params.includeContext = json_obj.at("includeContext").get<std::string>();
    }
    if (detail::has_json_value(json_obj, "tools")) {
        params.tools = json_obj.at("tools").get<std::vector<Tool>>();
    }
    if (detail::has_json_value(json_obj, "toolChoice")) {
        params.toolChoice = json_obj.at("toolChoice").get<ToolChoice>();
    }
    if (detail::has_json_value(json_obj, "metadata")) {
        params.metadata = json_obj.at("metadata").get<nlohmann::json>();
    }
    if (detail::has_json_value(json_obj, "_meta")) {
        params.meta = json_obj.at("_meta").get<nlohmann::json>();
    }
}

void to_json(nlohmann::json& json_obj, const CreateMessageRequest& value) {
    json_obj["method"] = value.method;
    json_obj["params"] = value.params;
}

void from_json(const nlohmann::json& json_obj, CreateMessageRequest& value) {
    json_obj.at("method").get_to(value.method);
    json_obj.at("params").get_to(value.params);
}

void to_json(nlohmann::json& json_obj, const CreateMessageResult& result) {
    json_obj =
        nlohmann::json{{"role", result.role}, {"content", result.content}, {"model", result.model}};
    if (result.stopReason) {
        json_obj["stopReason"] = *result.stopReason;
    }
    if (result.meta) {
        json_obj["_meta"] = *result.meta;
    }
}

void from_json(const nlohmann::json& json_obj, CreateMessageResult& result) {
    result = CreateMessageResult{};
    json_obj.at("role").get_to(result.role);
    json_obj.at("content").get_to(result.content);
    json_obj.at("model").get_to(result.model);
    if (detail::has_json_value(json_obj, "stopReason")) {
        result.stopReason = json_obj.at("stopReason").get<std::string>();
    }
    if (detail::has_json_value(json_obj, "_meta")) {
        result.meta = json_obj.at("_meta").get<nlohmann::json>();
    }
}

}  // namespace mcp

#pragma once

#include <mcp/protocol/base.hpp>
#include <mcp/protocol/content.hpp>
#include <mcp/protocol/tools.hpp>

namespace mcp {

/**
 * @brief Controls tool selection behavior for sampling requests.
 */
struct ToolChoice {
    std::optional<std::string> mode;  ///< "auto", "none", or "required".
};

/**
 * @brief Serializes ToolChoice to JSON.
 *
 * @param json_obj The JSON object to populate.
 * @param choice The ToolChoice object to serialize.
 */
inline void to_json(nlohmann::json& json_obj, const ToolChoice& choice) {
    json_obj = nlohmann::json::object();
    if (choice.mode) {
        json_obj["mode"] = *choice.mode;
    }
}

/**
 * @brief Deserializes ToolChoice from JSON.
 *
 * @param json_obj The JSON object to read from.
 * @param choice The ToolChoice object to populate.
 */
inline void from_json(const nlohmann::json& json_obj, ToolChoice& choice) {
    if (json_obj.contains("mode")) {
        choice.mode = json_obj.at("mode").get<std::string>();
    }
}

/**
 * @brief Hints to use for model selection.
 */
struct ModelHint {
    std::optional<std::string> name;  ///< A hint for a model name (substring match).
};

/**
 * @brief Serializes ModelHint to JSON.
 *
 * @param json_obj The JSON object to populate.
 * @param hint The ModelHint object to serialize.
 */
inline void to_json(nlohmann::json& json_obj, const ModelHint& hint) {
    json_obj = nlohmann::json::object();
    if (hint.name) {
        json_obj["name"] = *hint.name;
    }
}

/**
 * @brief Deserializes ModelHint from JSON.
 *
 * @param json_obj The JSON object to read from.
 * @param hint The ModelHint object to populate.
 */
inline void from_json(const nlohmann::json& json_obj, ModelHint& hint) {
    if (json_obj.contains("name")) {
        hint.name = json_obj.at("name").get<std::string>();
    }
}

/**
 * @brief The server's preferences for model selection during sampling.
 */
struct ModelPreferences {
    std::optional<double> costPriority;           ///< Cost priority (0.0 to 1.0).
    std::optional<std::vector<ModelHint>> hints;  ///< Optional hints for model selection.
    std::optional<double> intelligencePriority;   ///< Intelligence priority (0.0 to 1.0).
    std::optional<double> speedPriority;          ///< Speed priority (0.0 to 1.0).
};

/**
 * @brief Serializes ModelPreferences to JSON.
 *
 * @param json_obj The JSON object to populate.
 * @param prefs The ModelPreferences object to serialize.
 */
inline void to_json(nlohmann::json& json_obj, const ModelPreferences& prefs) {
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

/**
 * @brief Deserializes ModelPreferences from JSON.
 *
 * @param json_obj The JSON object to read from.
 * @param prefs The ModelPreferences object to populate.
 */
inline void from_json(const nlohmann::json& json_obj, ModelPreferences& prefs) {
    if (json_obj.contains("costPriority")) {
        prefs.costPriority = json_obj.at("costPriority").get<double>();
    }
    if (json_obj.contains("hints")) {
        prefs.hints = json_obj.at("hints").get<std::vector<ModelHint>>();
    }
    if (json_obj.contains("intelligencePriority")) {
        prefs.intelligencePriority = json_obj.at("intelligencePriority").get<double>();
    }
    if (json_obj.contains("speedPriority")) {
        prefs.speedPriority = json_obj.at("speedPriority").get<double>();
    }
}

using SamplingMessageContentBlock = ContentBlock;

/**
 * @brief Content for sampling messages — either a single block or an array of blocks.
 */
using SamplingMessageContent =
    std::variant<SamplingMessageContentBlock, std::vector<SamplingMessageContentBlock>>;

/**
 * @brief Serializes SamplingMessageContent variant to JSON.
 *
 * @param json_obj The JSON object to populate.
 * @param content The SamplingMessageContent variant to serialize.
 */
inline void to_json(nlohmann::json& json_obj, const SamplingMessageContent& content) {
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

/**
 * @brief Deserializes SamplingMessageContent variant from JSON.
 *
 * @param json_obj The JSON object to read from.
 * @param content The SamplingMessageContent variant to populate.
 */
inline void from_json(const nlohmann::json& json_obj, SamplingMessageContent& content) {
    if (json_obj.is_array()) {
        content = json_obj.get<std::vector<SamplingMessageContentBlock>>();
    } else {
        content = json_obj.get<SamplingMessageContentBlock>();
    }
}

/**
 * @brief Describes a message issued to or received from an LLM API.
 */
struct SamplingMessage {
    Role role;                           ///< The sender or recipient of messages.
    SamplingMessageContent content;      ///< The content of the message.
    std::optional<nlohmann::json> meta;  ///< Reserved for protocol use.
};

/**
 * @brief Serializes SamplingMessage to JSON.
 *
 * @param json_obj The JSON object to populate.
 * @param msg The SamplingMessage object to serialize.
 */
inline void to_json(nlohmann::json& json_obj, const SamplingMessage& msg) {
    json_obj = nlohmann::json{{"role", msg.role}, {"content", msg.content}};
    if (msg.meta) {
        json_obj["_meta"] = *msg.meta;
    }
}

/**
 * @brief Deserializes SamplingMessage from JSON.
 *
 * @param json_obj The JSON object to read from.
 * @param msg The SamplingMessage object to populate.
 */
inline void from_json(const nlohmann::json& json_obj, SamplingMessage& msg) {
    json_obj.at("role").get_to(msg.role);
    json_obj.at("content").get_to(msg.content);
    if (json_obj.contains("_meta")) {
        msg.meta = json_obj.at("_meta").get<nlohmann::json>();
    }
}

/**
 * @brief Parameters for a sampling/createMessage request.
 */
struct CreateMessageRequestParams {
    std::vector<SamplingMessage> messages;                  ///< The messages to send to the LLM.
    int maxTokens;                                          ///< Maximum number of tokens to sample.
    std::optional<std::string> systemPrompt;                ///< Optional system prompt.
    std::optional<double> temperature;                      ///< Optional temperature.
    std::optional<std::vector<std::string>> stopSequences;  ///< Optional stop sequences.
    std::optional<ModelPreferences> modelPreferences;       ///< Server's model selection preferences.
    std::optional<std::string> includeContext;              ///< "none", "thisServer", or "allServers".
    std::optional<std::vector<Tool>> tools;                 ///< Tools available for model use.
    std::optional<ToolChoice> toolChoice;                   ///< Controls how the model uses tools.
    std::optional<nlohmann::json> metadata;                 ///< Optional provider-specific metadata.
    std::optional<nlohmann::json> meta;                     ///< Reserved for protocol use.
};

/**
 * @brief Serializes CreateMessageRequestParams to JSON.
 *
 * @param json_obj The JSON object to populate.
 * @param params The CreateMessageRequestParams object to serialize.
 */
inline void to_json(nlohmann::json& json_obj, const CreateMessageRequestParams& params) {
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

/**
 * @brief Deserializes CreateMessageRequestParams from JSON.
 *
 * @param json_obj The JSON object to read from.
 * @param params The CreateMessageRequestParams object to populate.
 */
inline void from_json(const nlohmann::json& json_obj, CreateMessageRequestParams& params) {
    json_obj.at("messages").get_to(params.messages);
    json_obj.at("maxTokens").get_to(params.maxTokens);
    if (json_obj.contains("systemPrompt")) {
        params.systemPrompt = json_obj.at("systemPrompt").get<std::string>();
    }
    if (json_obj.contains("temperature")) {
        params.temperature = json_obj.at("temperature").get<double>();
    }
    if (json_obj.contains("stopSequences")) {
        params.stopSequences = json_obj.at("stopSequences").get<std::vector<std::string>>();
    }
    if (json_obj.contains("modelPreferences")) {
        params.modelPreferences = json_obj.at("modelPreferences").get<ModelPreferences>();
    }
    if (json_obj.contains("includeContext")) {
        params.includeContext = json_obj.at("includeContext").get<std::string>();
    }
    if (json_obj.contains("tools")) {
        params.tools = json_obj.at("tools").get<std::vector<Tool>>();
    }
    if (json_obj.contains("toolChoice")) {
        params.toolChoice = json_obj.at("toolChoice").get<ToolChoice>();
    }
    if (json_obj.contains("metadata")) {
        params.metadata = json_obj.at("metadata").get<nlohmann::json>();
    }
    if (json_obj.contains("_meta")) {
        params.meta = json_obj.at("_meta").get<nlohmann::json>();
    }
}

/**
 * @brief A request from the server to sample an LLM via the client.
 */
struct CreateMessageRequest {
    std::string method = "sampling/createMessage";  ///< The method name.
    CreateMessageRequestParams params;              ///< The request parameters.
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(CreateMessageRequest, method, params)

/**
 * @brief The result returned by the client for a sampling/createMessage request.
 */
struct CreateMessageResult {
    Role role;                              ///< The role of the message sender.
    SamplingMessageContent content;         ///< The content of the response.
    std::string model;                      ///< The name of the model that generated the message.
    std::optional<std::string> stopReason;  ///< The reason why sampling stopped.
    std::optional<nlohmann::json> meta;     ///< Reserved for protocol use.
};

/**
 * @brief Serializes CreateMessageResult to JSON.
 *
 * @param json_obj The JSON object to populate.
 * @param result The CreateMessageResult object to serialize.
 */
inline void to_json(nlohmann::json& json_obj, const CreateMessageResult& result) {
    json_obj =
        nlohmann::json{{"role", result.role}, {"content", result.content}, {"model", result.model}};
    if (result.stopReason) {
        json_obj["stopReason"] = *result.stopReason;
    }
    if (result.meta) {
        json_obj["_meta"] = *result.meta;
    }
}

/**
 * @brief Deserializes CreateMessageResult from JSON.
 *
 * @param json_obj The JSON object to read from.
 * @param result The CreateMessageResult object to populate.
 */
inline void from_json(const nlohmann::json& json_obj, CreateMessageResult& result) {
    json_obj.at("role").get_to(result.role);
    json_obj.at("content").get_to(result.content);
    json_obj.at("model").get_to(result.model);
    if (json_obj.contains("stopReason")) {
        result.stopReason = json_obj.at("stopReason").get<std::string>();
    }
    if (json_obj.contains("_meta")) {
        result.meta = json_obj.at("_meta").get<nlohmann::json>();
    }
}

}  // namespace mcp

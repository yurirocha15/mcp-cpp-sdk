#pragma once

#include <mcp/core/export.hpp>
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
MCP_API void to_json(nlohmann::json& json_obj, const ToolChoice& choice);

/**
 * @brief Deserializes ToolChoice from JSON.
 *
 * @param json_obj The JSON object to read from.
 * @param choice The ToolChoice object to populate.
 */
MCP_API void from_json(const nlohmann::json& json_obj, ToolChoice& choice);

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
MCP_API void to_json(nlohmann::json& json_obj, const ModelHint& hint);

/**
 * @brief Deserializes ModelHint from JSON.
 *
 * @param json_obj The JSON object to read from.
 * @param hint The ModelHint object to populate.
 */
MCP_API void from_json(const nlohmann::json& json_obj, ModelHint& hint);

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
MCP_API void to_json(nlohmann::json& json_obj, const ModelPreferences& prefs);

/**
 * @brief Deserializes ModelPreferences from JSON.
 *
 * @param json_obj The JSON object to read from.
 * @param prefs The ModelPreferences object to populate.
 */
MCP_API void from_json(const nlohmann::json& json_obj, ModelPreferences& prefs);

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
MCP_API void to_json(nlohmann::json& json_obj, const SamplingMessageContent& content);

/**
 * @brief Deserializes SamplingMessageContent variant from JSON.
 *
 * @param json_obj The JSON object to read from.
 * @param content The SamplingMessageContent variant to populate.
 */
MCP_API void from_json(const nlohmann::json& json_obj, SamplingMessageContent& content);

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
MCP_API void to_json(nlohmann::json& json_obj, const SamplingMessage& msg);

/**
 * @brief Deserializes SamplingMessage from JSON.
 *
 * @param json_obj The JSON object to read from.
 * @param msg The SamplingMessage object to populate.
 */
MCP_API void from_json(const nlohmann::json& json_obj, SamplingMessage& msg);

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
MCP_API void to_json(nlohmann::json& json_obj, const CreateMessageRequestParams& params);

/**
 * @brief Deserializes CreateMessageRequestParams from JSON.
 *
 * @param json_obj The JSON object to read from.
 * @param params The CreateMessageRequestParams object to populate.
 */
MCP_API void from_json(const nlohmann::json& json_obj, CreateMessageRequestParams& params);

/**
 * @brief A request from the server to sample an LLM via the client.
 */
struct CreateMessageRequest {
    std::string method = "sampling/createMessage";  ///< The method name.
    CreateMessageRequestParams params;              ///< The request parameters.
};

MCP_API void to_json(nlohmann::json& json_obj, const CreateMessageRequest& value);
MCP_API void from_json(const nlohmann::json& json_obj, CreateMessageRequest& value);

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
MCP_API void to_json(nlohmann::json& json_obj, const CreateMessageResult& result);

/**
 * @brief Deserializes CreateMessageResult from JSON.
 *
 * @param json_obj The JSON object to read from.
 * @param result The CreateMessageResult object to populate.
 */
MCP_API void from_json(const nlohmann::json& json_obj, CreateMessageResult& result);

}  // namespace mcp

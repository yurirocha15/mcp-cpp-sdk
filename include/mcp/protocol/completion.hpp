#pragma once

#include <mcp/core/export.hpp>
#include <mcp/protocol/base.hpp>

namespace mcp {

/**
 * @brief Common params for paginated requests.
 */
struct PaginatedRequestParams {
    std::optional<std::string>
        cursor;  ///< An opaque token representing the current pagination position.
    std::optional<nlohmann::json> meta;  ///< Reserved for protocol use.
};

MCP_API void to_json(nlohmann::json& json_obj, const PaginatedRequestParams& params);

MCP_API void from_json(const nlohmann::json& json_obj, PaginatedRequestParams& params);

/**
 * @brief A reference to a prompt.
 */
struct PromptReference {
    std::string type = "ref/prompt";  ///< The type of reference.
    std::string name;                 ///< The name of the prompt.
};

MCP_API void to_json(nlohmann::json& json_obj, const PromptReference& value);
MCP_API void from_json(const nlohmann::json& json_obj, PromptReference& value);

/**
 * @brief A reference to a resource template.
 */
struct ResourceTemplateReference {
    std::string type = "ref/resource";  ///< The type of reference.
    std::string uri;                    ///< The URI of the resource.
};

MCP_API void to_json(nlohmann::json& json_obj, const ResourceTemplateReference& value);
MCP_API void from_json(const nlohmann::json& json_obj, ResourceTemplateReference& value);

/**
 * @brief A reference to a resource or prompt for completion.
 */
using CompleteReference = std::variant<PromptReference, ResourceTemplateReference>;

MCP_API void to_json(nlohmann::json& json_obj, const CompleteReference& ref);

MCP_API void from_json(const nlohmann::json& json_obj, CompleteReference& ref);

/**
 * @brief The argument for completion.
 */
struct CompleteParamsArgument {
    std::string name;   ///< The name of the argument.
    std::string value;  ///< The value of the argument.
};

MCP_API void to_json(nlohmann::json& json_obj, const CompleteParamsArgument& value);
MCP_API void from_json(const nlohmann::json& json_obj, CompleteParamsArgument& value);

/**
 * @brief Context for completion.
 */
struct CompleteContext {
    std::optional<std::map<std::string, std::string>> arguments;  ///< Arguments for the context.
};

/**
 * @brief Serializes CompleteContext to JSON.
 *
 * @param json_obj The JSON object to populate.
 * @param context The CompleteContext object to serialize.
 */
MCP_API void to_json(nlohmann::json& json_obj, const CompleteContext& context);

/**
 * @brief Deserializes CompleteContext from JSON.
 *
 * @param json_obj The JSON object to read from.
 * @param context The CompleteContext object to populate.
 */
MCP_API void from_json(const nlohmann::json& json_obj, CompleteContext& context);

/**
 * @brief Parameters for completion.
 */
struct CompleteParams {
    CompleteReference ref;                   ///< The reference to the resource or prompt.
    CompleteParamsArgument argument;         ///< The argument to complete.
    std::optional<CompleteContext> context;  ///< Optional context for completion.
    std::optional<nlohmann::json> meta;      ///< Reserved for protocol use.
};

/**
 * @brief Serializes CompleteParams to JSON.
 *
 * @param json_obj The JSON object to populate.
 * @param params The CompleteParams object to serialize.
 */
MCP_API void to_json(nlohmann::json& json_obj, const CompleteParams& params);

/**
 * @brief Deserializes CompleteParams from JSON.
 *
 * @param json_obj The JSON object to read from.
 * @param params The CompleteParams object to populate.
 */
MCP_API void from_json(const nlohmann::json& json_obj, CompleteParams& params);

/**
 * @brief Details of the completion result.
 */
struct CompletionResultDetails {
    std::vector<std::string> values;  ///< The completion values.
    std::optional<int64_t> total;     ///< The total number of completions available.
    std::optional<bool> hasMore;      ///< Whether there are more completions available.
};

/**
 * @brief Serializes CompletionResultDetails to JSON.
 *
 * @param json_obj The JSON object to populate.
 * @param details The CompletionResultDetails object to serialize.
 */
MCP_API void to_json(nlohmann::json& json_obj, const CompletionResultDetails& details);

/**
 * @brief Deserializes CompletionResultDetails from JSON.
 *
 * @param json_obj The JSON object to read from.
 * @param details The CompletionResultDetails object to populate.
 */
MCP_API void from_json(const nlohmann::json& json_obj, CompletionResultDetails& details);

/**
 * @brief The result of a completion request.
 */
struct CompleteResult {
    CompletionResultDetails completion;  ///< The completion details.
    std::optional<nlohmann::json> meta;  ///< Reserved for protocol use.
};

/**
 * @brief Serializes CompleteResult to JSON.
 *
 * @param json_obj The JSON object to populate.
 * @param result The CompleteResult object to serialize.
 */
MCP_API void to_json(nlohmann::json& json_obj, const CompleteResult& result);

/**
 * @brief Deserializes CompleteResult from JSON.
 *
 * @param json_obj The JSON object to read from.
 * @param result The CompleteResult object to populate.
 */
MCP_API void from_json(const nlohmann::json& json_obj, CompleteResult& result);

}  // namespace mcp

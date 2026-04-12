#pragma once

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

inline void to_json(nlohmann::json& json_obj, const PaginatedRequestParams& params) {
    json_obj = nlohmann::json::object();
    if (params.cursor) {
        json_obj["cursor"] = *params.cursor;
    }
    if (params.meta) {
        json_obj["_meta"] = *params.meta;
    }
}

inline void from_json(const nlohmann::json& json_obj, PaginatedRequestParams& params) {
    if (json_obj.contains("cursor")) {
        params.cursor = json_obj.at("cursor").get<std::string>();
    }
    if (json_obj.contains("_meta")) {
        params.meta = json_obj.at("_meta").get<nlohmann::json>();
    }
}

/**
 * @brief A reference to a prompt.
 */
struct PromptReference {
    std::string type = "ref/prompt";  ///< The type of reference.
    std::string name;                 ///< The name of the prompt.
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(PromptReference, type, name)

/**
 * @brief A reference to a resource template.
 */
struct ResourceTemplateReference {
    std::string type = "ref/resource";  ///< The type of reference.
    std::string uri;                    ///< The URI of the resource.
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ResourceTemplateReference, type, uri)

/**
 * @brief A reference to a resource or prompt for completion.
 */
using CompleteReference = std::variant<PromptReference, ResourceTemplateReference>;

inline void to_json(nlohmann::json& json_obj, const CompleteReference& ref) {
    std::visit([&json_obj](auto&& arg) { to_json(json_obj, arg); }, ref);
}

inline void from_json(const nlohmann::json& json_obj, CompleteReference& ref) {
    if (json_obj.at("type") == "ref/prompt") {
        ref = json_obj.get<PromptReference>();
    } else {
        ref = json_obj.get<ResourceTemplateReference>();
    }
}

/**
 * @brief The argument for completion.
 */
struct CompleteParamsArgument {
    std::string name;   ///< The name of the argument.
    std::string value;  ///< The value of the argument.
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(CompleteParamsArgument, name, value)

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
inline void to_json(nlohmann::json& json_obj, const CompleteContext& context) {
    json_obj = nlohmann::json::object();
    if (context.arguments) {
        json_obj["arguments"] = *context.arguments;
    }
}

/**
 * @brief Deserializes CompleteContext from JSON.
 *
 * @param json_obj The JSON object to read from.
 * @param context The CompleteContext object to populate.
 */
inline void from_json(const nlohmann::json& json_obj, CompleteContext& context) {
    if (json_obj.contains("arguments")) {
        context.arguments = json_obj.at("arguments").get<std::map<std::string, std::string>>();
    }
}

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
inline void to_json(nlohmann::json& json_obj, const CompleteParams& params) {
    json_obj = nlohmann::json{{"ref", params.ref}, {"argument", params.argument}};
    if (params.context) {
        json_obj["context"] = *params.context;
    }
    if (params.meta) {
        json_obj["_meta"] = *params.meta;
    }
}

/**
 * @brief Deserializes CompleteParams from JSON.
 *
 * @param json_obj The JSON object to read from.
 * @param params The CompleteParams object to populate.
 */
inline void from_json(const nlohmann::json& json_obj, CompleteParams& params) {
    json_obj.at("ref").get_to(params.ref);
    json_obj.at("argument").get_to(params.argument);
    if (json_obj.contains("context") && !json_obj.at("context").is_null()) {
        params.context = json_obj.at("context").get<CompleteContext>();
    }
    if (json_obj.contains("_meta")) {
        params.meta = json_obj.at("_meta").get<nlohmann::json>();
    }
}

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
inline void to_json(nlohmann::json& json_obj, const CompletionResultDetails& details) {
    json_obj = nlohmann::json{{"values", details.values}};
    if (details.total) {
        json_obj["total"] = *details.total;
    }
    if (details.hasMore) {
        json_obj["hasMore"] = *details.hasMore;
    }
}

/**
 * @brief Deserializes CompletionResultDetails from JSON.
 *
 * @param json_obj The JSON object to read from.
 * @param details The CompletionResultDetails object to populate.
 */
inline void from_json(const nlohmann::json& json_obj, CompletionResultDetails& details) {
    json_obj.at("values").get_to(details.values);
    if (json_obj.contains("total")) {
        details.total = json_obj.at("total").get<int64_t>();
    }
    if (json_obj.contains("hasMore")) {
        details.hasMore = json_obj.at("hasMore").get<bool>();
    }
}

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
inline void to_json(nlohmann::json& json_obj, const CompleteResult& result) {
    json_obj = nlohmann::json{{"completion", result.completion}};
    if (result.meta) {
        json_obj["_meta"] = *result.meta;
    }
}

/**
 * @brief Deserializes CompleteResult from JSON.
 *
 * @param json_obj The JSON object to read from.
 * @param result The CompleteResult object to populate.
 */
inline void from_json(const nlohmann::json& json_obj, CompleteResult& result) {
    json_obj.at("completion").get_to(result.completion);
    if (json_obj.contains("_meta")) {
        result.meta = json_obj.at("_meta").get<nlohmann::json>();
    }
}

}  // namespace mcp

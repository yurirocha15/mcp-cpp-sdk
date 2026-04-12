#pragma once

#include <mcp/protocol/base.hpp>
#include <mcp/protocol/capabilities.hpp>
#include <mcp/protocol/completion.hpp>
#include <mcp/protocol/content.hpp>

namespace mcp {

/**
 * @brief Describes an argument that a prompt can accept.
 */
struct PromptArgument {
    std::string name;                        ///< The name of the argument.
    std::optional<std::string> description;  ///< A human-readable description.
    std::optional<bool> required;            ///< Whether this argument must be provided.
    std::optional<std::string> title;        ///< Human-readable title.
};

inline void to_json(nlohmann::json& json_obj, const PromptArgument& arg) {
    json_obj = nlohmann::json{{"name", arg.name}};
    if (arg.description) {
        json_obj["description"] = *arg.description;
    }
    if (arg.required) {
        json_obj["required"] = *arg.required;
    }
    if (arg.title) {
        json_obj["title"] = *arg.title;
    }
}

inline void from_json(const nlohmann::json& json_obj, PromptArgument& arg) {
    json_obj.at("name").get_to(arg.name);
    if (json_obj.contains("description")) {
        arg.description = json_obj.at("description").get<std::string>();
    }
    if (json_obj.contains("required")) {
        arg.required = json_obj.at("required").get<bool>();
    }
    if (json_obj.contains("title")) {
        arg.title = json_obj.at("title").get<std::string>();
    }
}

/**
 * @brief A prompt or prompt template that the server offers.
 */
struct Prompt {
    std::string name;                                      ///< The name of the prompt.
    std::optional<std::string> description;                ///< An optional description.
    std::optional<std::vector<PromptArgument>> arguments;  ///< A list of arguments.
    std::optional<nlohmann::json> meta;                    ///< Reserved for protocol use.
    std::optional<std::string> title;                      ///< Human-readable title.
    std::optional<std::vector<Icon>> icons;                ///< Icons for the prompt.
};

inline void to_json(nlohmann::json& json_obj, const Prompt& prompt) {
    json_obj = nlohmann::json{{"name", prompt.name}};
    if (prompt.description) {
        json_obj["description"] = *prompt.description;
    }
    if (prompt.arguments) {
        json_obj["arguments"] = *prompt.arguments;
    }
    if (prompt.meta) {
        json_obj["_meta"] = *prompt.meta;
    }
    if (prompt.title) {
        json_obj["title"] = *prompt.title;
    }
    if (prompt.icons) {
        json_obj["icons"] = *prompt.icons;
    }
}

inline void from_json(const nlohmann::json& json_obj, Prompt& prompt) {
    json_obj.at("name").get_to(prompt.name);
    if (json_obj.contains("description")) {
        prompt.description = json_obj.at("description").get<std::string>();
    }
    if (json_obj.contains("arguments")) {
        prompt.arguments = json_obj.at("arguments").get<std::vector<PromptArgument>>();
    }
    if (json_obj.contains("_meta")) {
        prompt.meta = json_obj.at("_meta").get<nlohmann::json>();
    }
    if (json_obj.contains("title")) {
        prompt.title = json_obj.at("title").get<std::string>();
    }
    if (json_obj.contains("icons")) {
        prompt.icons = json_obj.at("icons").get<std::vector<Icon>>();
    }
}

/**
 * @brief Describes a message returned as part of a prompt.
 */
struct PromptMessage {
    Role role;             ///< The sender or recipient of messages.
    ContentBlock content;  ///< The content of the message.
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(PromptMessage, role, content)

/**
 * @brief Parameters for a prompts/get request.
 */
struct GetPromptRequestParams {
    std::string name;                                             ///< The name of the prompt.
    std::optional<std::map<std::string, std::string>> arguments;  ///< Arguments to use for templating.
    std::optional<nlohmann::json> meta;                           ///< Reserved for protocol use.
};

inline void to_json(nlohmann::json& json_obj, const GetPromptRequestParams& params) {
    json_obj = nlohmann::json{{"name", params.name}};
    if (params.arguments) {
        json_obj["arguments"] = *params.arguments;
    }
    if (params.meta) {
        json_obj["_meta"] = *params.meta;
    }
}

inline void from_json(const nlohmann::json& json_obj, GetPromptRequestParams& params) {
    json_obj.at("name").get_to(params.name);
    if (json_obj.contains("arguments")) {
        params.arguments = json_obj.at("arguments").get<std::map<std::string, std::string>>();
    }
    if (json_obj.contains("_meta")) {
        params.meta = json_obj.at("_meta").get<nlohmann::json>();
    }
}

/**
 * @brief Used by the client to get a prompt provided by the server.
 */
struct GetPromptRequest {
    std::string method = "prompts/get";  ///< The method name.
    GetPromptRequestParams params;       ///< The request parameters.
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(GetPromptRequest, method, params)

/**
 * @brief The result returned by the server for a prompts/get request.
 */
struct GetPromptResult {
    std::vector<PromptMessage> messages;     ///< The messages returned by the prompt.
    std::optional<std::string> description;  ///< An optional description.
    std::optional<nlohmann::json> meta;      ///< Reserved for protocol use.
};

inline void to_json(nlohmann::json& json_obj, const GetPromptResult& result) {
    json_obj = nlohmann::json{{"messages", result.messages}};
    if (result.description) {
        json_obj["description"] = *result.description;
    }
    if (result.meta) {
        json_obj["_meta"] = *result.meta;
    }
}

inline void from_json(const nlohmann::json& json_obj, GetPromptResult& result) {
    json_obj.at("messages").get_to(result.messages);
    if (json_obj.contains("description")) {
        result.description = json_obj.at("description").get<std::string>();
    }
    if (json_obj.contains("_meta")) {
        result.meta = json_obj.at("_meta").get<nlohmann::json>();
    }
}

/**
 * @brief Sent from the client to request a list of prompts.
 */
struct ListPromptsRequest {
    std::string method = "prompts/list";           ///< The method name.
    std::optional<PaginatedRequestParams> params;  ///< The request parameters.
};

inline void to_json(nlohmann::json& json_obj, const ListPromptsRequest& req) {
    json_obj = nlohmann::json{{"method", req.method}};
    if (req.params) {
        json_obj["params"] = *req.params;
    }
}

inline void from_json(const nlohmann::json& json_obj, ListPromptsRequest& req) {
    json_obj.at("method").get_to(req.method);
    if (json_obj.contains("params")) {
        req.params = json_obj.at("params").get<PaginatedRequestParams>();
    }
}

/**
 * @brief The result returned by the server for a prompts/list request.
 */
struct ListPromptsResult {
    std::vector<Prompt> prompts;            ///< The list of prompts.
    std::optional<std::string> nextCursor;  ///< An opaque token representing the pagination position.
    std::optional<nlohmann::json> meta;     ///< Reserved for protocol use.
};

inline void to_json(nlohmann::json& json_obj, const ListPromptsResult& result) {
    json_obj = nlohmann::json{{"prompts", result.prompts}};
    if (result.nextCursor) {
        json_obj["nextCursor"] = *result.nextCursor;
    }
    if (result.meta) {
        json_obj["_meta"] = *result.meta;
    }
}

inline void from_json(const nlohmann::json& json_obj, ListPromptsResult& result) {
    json_obj.at("prompts").get_to(result.prompts);
    if (json_obj.contains("nextCursor")) {
        result.nextCursor = json_obj.at("nextCursor").get<std::string>();
    }
    if (json_obj.contains("_meta")) {
        result.meta = json_obj.at("_meta").get<nlohmann::json>();
    }
}

}  // namespace mcp

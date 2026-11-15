#pragma once

#include <mcp/core/export.hpp>
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

MCP_API void to_json(nlohmann::json& json_obj, const PromptArgument& arg);

MCP_API void from_json(const nlohmann::json& json_obj, PromptArgument& arg);

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

MCP_API void to_json(nlohmann::json& json_obj, const Prompt& prompt);

MCP_API void from_json(const nlohmann::json& json_obj, Prompt& prompt);

/**
 * @brief Describes a message returned as part of a prompt.
 */
struct PromptMessage {
    Role role;             ///< The sender or recipient of messages.
    ContentBlock content;  ///< The content of the message.
};

MCP_API void to_json(nlohmann::json& json_obj, const PromptMessage& value);
MCP_API void from_json(const nlohmann::json& json_obj, PromptMessage& value);

/**
 * @brief Parameters for a prompts/get request.
 */
struct GetPromptRequestParams {
    std::string name;                                             ///< The name of the prompt.
    std::optional<std::map<std::string, std::string>> arguments;  ///< Arguments to use for templating.
    std::optional<nlohmann::json> meta;                           ///< Reserved for protocol use.
};

MCP_API void to_json(nlohmann::json& json_obj, const GetPromptRequestParams& params);

MCP_API void from_json(const nlohmann::json& json_obj, GetPromptRequestParams& params);

/**
 * @brief Used by the client to get a prompt provided by the server.
 */
struct GetPromptRequest {
    std::string method = "prompts/get";  ///< The method name.
    GetPromptRequestParams params;       ///< The request parameters.
};

MCP_API void to_json(nlohmann::json& json_obj, const GetPromptRequest& value);
MCP_API void from_json(const nlohmann::json& json_obj, GetPromptRequest& value);

/**
 * @brief The result returned by the server for a prompts/get request.
 */
struct GetPromptResult {
    std::vector<PromptMessage> messages;     ///< The messages returned by the prompt.
    std::optional<std::string> description;  ///< An optional description.
    std::optional<nlohmann::json> meta;      ///< Reserved for protocol use.
};

MCP_API void to_json(nlohmann::json& json_obj, const GetPromptResult& result);

MCP_API void from_json(const nlohmann::json& json_obj, GetPromptResult& result);

/**
 * @brief Sent from the client to request a list of prompts.
 */
struct ListPromptsRequest {
    std::string method = "prompts/list";           ///< The method name.
    std::optional<PaginatedRequestParams> params;  ///< The request parameters.
};

MCP_API void to_json(nlohmann::json& json_obj, const ListPromptsRequest& req);

MCP_API void from_json(const nlohmann::json& json_obj, ListPromptsRequest& req);

/**
 * @brief The result returned by the server for a prompts/list request.
 */
struct ListPromptsResult {
    std::vector<Prompt> prompts;            ///< The list of prompts.
    std::optional<std::string> nextCursor;  ///< An opaque token representing the pagination position.
    std::optional<nlohmann::json> meta;     ///< Reserved for protocol use.
    std::optional<std::int64_t> ttlMs;      ///< Caching TTL hint, in milliseconds (2026-07-28).
    std::optional<CacheScope> cacheScope;   ///< Caching scope hint (2026-07-28).
};

MCP_API void to_json(nlohmann::json& json_obj, const ListPromptsResult& result);

MCP_API void from_json(const nlohmann::json& json_obj, ListPromptsResult& result);

}  // namespace mcp

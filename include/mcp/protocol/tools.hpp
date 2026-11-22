#pragma once

#include <mcp/core/export.hpp>
#include <mcp/protocol/base.hpp>
#include <mcp/protocol/capabilities.hpp>
#include <mcp/protocol/content.hpp>

namespace mcp {

/**
 * @brief Additional properties describing a Tool to clients.
 */
struct ToolAnnotations {
    std::optional<bool> destructiveHint;  ///< If true, the tool may perform destructive updates.
    std::optional<bool>
        idempotentHint;  ///< If true, calling the tool repeatedly has no additional effect.
    std::optional<bool> openWorldHint;  ///< If true, the tool may interact with an "open world".
    std::optional<bool> readOnlyHint;   ///< If true, the tool does not modify its environment.
    std::optional<std::string> title;   ///< A human-readable title for the tool.
};

/**
 * @brief Serializes ToolAnnotations to JSON.
 *
 * @param json_obj The JSON object to populate.
 * @param annotations The ToolAnnotations object to serialize.
 */
MCP_API void to_json(nlohmann::json& json_obj, const ToolAnnotations& annotations);

/**
 * @brief Deserializes ToolAnnotations from JSON.
 *
 * @param json_obj The JSON object to read from.
 * @param annotations The ToolAnnotations object to populate.
 */
MCP_API void from_json(const nlohmann::json& json_obj, ToolAnnotations& annotations);

/**
 * @brief Represents a tool definition.
 */
struct Tool {
    std::string name;                            ///< The name of the tool.
    std::optional<std::string> description;      ///< Optional description of the tool.
    nlohmann::json inputSchema;                  ///< JSON Schema object for the arguments; declares
                                                 ///< `"type": "object"`.
    std::optional<nlohmann::json> meta;          ///< Reserved for protocol use.
    std::optional<ToolAnnotations> annotations;  ///< Optional additional tool information.
    std::optional<nlohmann::json> outputSchema;  ///< Optional JSON Schema object for the output.
    std::optional<std::string> title;            ///< Human-readable title for the tool.
    std::optional<std::vector<Icon>> icons;      ///< Icons for the tool.
};

/**
 * @brief Serializes Tool to JSON.
 *
 * @param json_obj The JSON object to populate.
 * @param tool The Tool object to serialize.
 */
MCP_API void to_json(nlohmann::json& json_obj, const Tool& tool);

/**
 * @brief Deserializes Tool from JSON.
 *
 * @param json_obj The JSON object to read from.
 * @param tool The Tool object to populate.
 */
MCP_API void from_json(const nlohmann::json& json_obj, Tool& tool);

/**
 * @brief Parameters for calling a tool.
 */
struct CallToolParams {
    std::string name;                                     ///< The name of the tool to call.
    nlohmann::json arguments = nlohmann::json::object();  ///< Optional tool arguments.
    std::optional<nlohmann::json> meta;                   ///< Reserved for protocol use.
};

MCP_API void to_json(nlohmann::json& json_obj, const CallToolParams& params);

MCP_API void from_json(const nlohmann::json& json_obj, CallToolParams& params);

/**
 * @brief Result of calling a tool.
 */
struct CallToolResult {
    std::vector<ContentBlock> content;                ///< The content produced by the tool.
    std::optional<bool> isError;                      ///< Whether the tool call resulted in an error.
    std::optional<nlohmann::json> meta;               ///< Reserved for protocol use.
    std::optional<nlohmann::json> structuredContent;  ///< Optional structured content.
};

/**
 * @brief Create a successful tool result containing a text block.
 *
 * @param text Human-readable tool output.
 * @return A protocol-valid CallToolResult.
 */
MCP_API CallToolResult make_tool_text_result(std::string text);

/**
 * @brief Create a failed tool result containing a text block.
 *
 * @param message Human-readable error description.
 * @return A protocol-valid CallToolResult with isError set to true.
 */
MCP_API CallToolResult make_tool_error_result(std::string message);

/**
 * @brief Create a successful tool result containing structured output.
 *
 * The structured value may be any JSON value, as 2026-07-28 allows. A server
 * sends a value that is not an object only in response to a 2026-07-28
 * request; earlier revisions require an object, and the server refuses a result
 * that holds anything else. A JSON serialization is also emitted as text for
 * clients that do not consume structuredContent.
 *
 * @param structured_content Structured tool output.
 * @param text Optional human-readable representation. When omitted, the
 *             structured value is serialized as JSON.
 * @return A protocol-valid CallToolResult.
 */
MCP_API CallToolResult make_tool_structured_result(nlohmann::json structured_content,
                                                   std::optional<std::string> text = std::nullopt);

/**
 * @brief The text of @p result: its text blocks, joined by newlines.
 *
 * @details Blocks of any other kind, such as images, are left out, and so is structuredContent.
 * A result with no text block gives an empty string.
 */
MCP_API std::string result_text(const CallToolResult& result);

/**
 * @brief Serializes CallToolResult to JSON.
 *
 * @param json_obj The JSON object to populate.
 * @param result The CallToolResult object to serialize.
 */
MCP_API void to_json(nlohmann::json& json_obj, const CallToolResult& result);

/**
 * @brief Serializes CallToolResult to JSON, moving what it holds into the result.
 *
 * The output is that of the overload above; the result is not copied.
 *
 * @param json_obj The JSON object to populate.
 * @param result The CallToolResult object to serialize; left in an unspecified state.
 */
MCP_API void to_json(nlohmann::json& json_obj, CallToolResult&& result);

/**
 * @brief Deserializes CallToolResult from JSON.
 *
 * @param json_obj The JSON object to read from.
 * @param result The CallToolResult object to populate.
 */
MCP_API void from_json(const nlohmann::json& json_obj, CallToolResult& result);

/**
 * @brief A request to call a tool.
 */
struct CallToolRequest {
    std::string method = "tools/call";  ///< The method name.
    CallToolParams params;              ///< The request parameters.
};
MCP_API void to_json(nlohmann::json& json_obj, const CallToolRequest& value);
MCP_API void from_json(const nlohmann::json& json_obj, CallToolRequest& value);

/**
 * @brief Parameters for listing available tools.
 */
struct ListToolsRequestParams {
    std::optional<std::string> cursor;  ///< Optional pagination cursor.
};
MCP_API void to_json(nlohmann::json& j, const ListToolsRequestParams& p);
MCP_API void from_json(const nlohmann::json& j, ListToolsRequestParams& p);

/**
 * @brief A request to list all available tools.
 */
struct ListToolsRequest {
    std::string method = "tools/list";             ///< The method name.
    std::optional<ListToolsRequestParams> params;  ///< Optional request parameters.
};
MCP_API void to_json(nlohmann::json& j, const ListToolsRequest& r);
MCP_API void from_json(const nlohmann::json& j, ListToolsRequest& r);

/**
 * @brief Result containing a list of available tools.
 */
struct ListToolsResult {
    std::vector<Tool> tools;                ///< The list of tools in the current page.
    std::optional<std::string> nextCursor;  ///< Pagination cursor for the next page.
    std::optional<nlohmann::json> meta;     ///< Reserved for protocol use.
    std::optional<std::int64_t> ttlMs;      ///< Caching TTL hint, in milliseconds (2026-07-28).
    std::optional<CacheScope> cacheScope;   ///< Caching scope hint (2026-07-28).
};
MCP_API void to_json(nlohmann::json& j, const ListToolsResult& r);
MCP_API void from_json(const nlohmann::json& j, ListToolsResult& r);

}  // namespace mcp

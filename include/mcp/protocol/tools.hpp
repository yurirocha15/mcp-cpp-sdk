#pragma once

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
inline void to_json(nlohmann::json& json_obj, const ToolAnnotations& annotations) {
    json_obj = nlohmann::json::object();
    if (annotations.destructiveHint) {
        json_obj["destructiveHint"] = *annotations.destructiveHint;
    }
    if (annotations.idempotentHint) {
        json_obj["idempotentHint"] = *annotations.idempotentHint;
    }
    if (annotations.openWorldHint) {
        json_obj["openWorldHint"] = *annotations.openWorldHint;
    }
    if (annotations.readOnlyHint) {
        json_obj["readOnlyHint"] = *annotations.readOnlyHint;
    }
    if (annotations.title) {
        json_obj["title"] = *annotations.title;
    }
}

/**
 * @brief Deserializes ToolAnnotations from JSON.
 *
 * @param json_obj The JSON object to read from.
 * @param annotations The ToolAnnotations object to populate.
 */
inline void from_json(const nlohmann::json& json_obj, ToolAnnotations& annotations) {
    if (json_obj.contains("destructiveHint")) {
        annotations.destructiveHint = json_obj.at("destructiveHint").get<bool>();
    }
    if (json_obj.contains("idempotentHint")) {
        annotations.idempotentHint = json_obj.at("idempotentHint").get<bool>();
    }
    if (json_obj.contains("openWorldHint")) {
        annotations.openWorldHint = json_obj.at("openWorldHint").get<bool>();
    }
    if (json_obj.contains("readOnlyHint")) {
        annotations.readOnlyHint = json_obj.at("readOnlyHint").get<bool>();
    }
    if (json_obj.contains("title")) {
        annotations.title = json_obj.at("title").get<std::string>();
    }
}

/**
 * @brief Describes the execution support of a tool.
 */
struct ToolExecution {
    std::string taskSupport;  ///< "forbidden" | "optional" | "required"
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ToolExecution, taskSupport)

/**
 * @brief Represents a tool definition.
 */
struct Tool {
    std::string name;                            ///< The name of the tool.
    std::optional<std::string> description;      ///< Optional description of the tool.
    nlohmann::json inputSchema;                  ///< The JSON schema for the tool's input arguments.
    std::optional<nlohmann::json> meta;          ///< Reserved for protocol use.
    std::optional<ToolAnnotations> annotations;  ///< Optional additional tool information.
    std::optional<nlohmann::json> outputSchema;  ///< Optional JSON schema for the tool's output.
    std::optional<std::string> title;            ///< Human-readable title for the tool.
    std::optional<std::vector<Icon>> icons;      ///< Icons for the tool.
    std::optional<ToolExecution> execution;      ///< Tool execution requirements.
};

/**
 * @brief Serializes Tool to JSON.
 *
 * @param json_obj The JSON object to populate.
 * @param tool The Tool object to serialize.
 */
inline void to_json(nlohmann::json& json_obj, const Tool& tool) {
    json_obj = nlohmann::json{{"name", tool.name}, {"inputSchema", tool.inputSchema}};
    if (tool.description) {
        json_obj["description"] = *tool.description;
    }
    if (tool.meta) {
        json_obj["_meta"] = *tool.meta;
    }
    if (tool.annotations) {
        json_obj["annotations"] = *tool.annotations;
    }
    if (tool.outputSchema) {
        json_obj["outputSchema"] = *tool.outputSchema;
    }
    if (tool.title) {
        json_obj["title"] = *tool.title;
    }
    if (tool.icons) {
        json_obj["icons"] = *tool.icons;
    }
    if (tool.execution) {
        json_obj["execution"] = *tool.execution;
    }
}

/**
 * @brief Deserializes Tool from JSON.
 *
 * @param json_obj The JSON object to read from.
 * @param tool The Tool object to populate.
 */
inline void from_json(const nlohmann::json& json_obj, Tool& tool) {
    json_obj.at("name").get_to(tool.name);
    json_obj.at("inputSchema").get_to(tool.inputSchema);
    if (json_obj.contains("description")) {
        tool.description = json_obj.at("description").get<std::string>();
    }
    if (json_obj.contains("_meta")) {
        tool.meta = json_obj.at("_meta").get<nlohmann::json>();
    }
    if (json_obj.contains("annotations")) {
        tool.annotations = json_obj.at("annotations").get<ToolAnnotations>();
    }
    if (json_obj.contains("outputSchema")) {
        tool.outputSchema = json_obj.at("outputSchema").get<nlohmann::json>();
    }
    if (json_obj.contains("title")) {
        tool.title = json_obj.at("title").get<std::string>();
    }
    if (json_obj.contains("icons")) {
        tool.icons = json_obj.at("icons").get<std::vector<Icon>>();
    }
    if (json_obj.contains("execution")) {
        tool.execution = json_obj.at("execution").get<ToolExecution>();
    }
}

/**
 * @brief Parameters for calling a tool.
 */
struct CallToolParams {
    std::string name;                    ///< The name of the tool to call.
    nlohmann::json arguments;            ///< The arguments for the tool call.
    std::optional<nlohmann::json> meta;  ///< Reserved for protocol use.
};

inline void to_json(nlohmann::json& json_obj, const CallToolParams& params) {
    json_obj = nlohmann::json{{"name", params.name}, {"arguments", params.arguments}};
    if (params.meta) {
        json_obj["_meta"] = *params.meta;
    }
}

inline void from_json(const nlohmann::json& json_obj, CallToolParams& params) {
    json_obj.at("name").get_to(params.name);
    json_obj.at("arguments").get_to(params.arguments);
    if (json_obj.contains("_meta")) {
        params.meta = json_obj.at("_meta").get<nlohmann::json>();
    }
}

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
 * @brief Serializes CallToolResult to JSON.
 *
 * @param json_obj The JSON object to populate.
 * @param result The CallToolResult object to serialize.
 */
inline void to_json(nlohmann::json& json_obj, const CallToolResult& result) {
    json_obj = nlohmann::json{{"content", result.content}};
    if (result.isError) {
        json_obj["isError"] = *result.isError;
    }
    if (result.meta) {
        json_obj["_meta"] = *result.meta;
    }
    if (result.structuredContent) {
        json_obj["structuredContent"] = *result.structuredContent;
    }
}

/**
 * @brief Deserializes CallToolResult from JSON.
 *
 * @param json_obj The JSON object to read from.
 * @param result The CallToolResult object to populate.
 */
inline void from_json(const nlohmann::json& json_obj, CallToolResult& result) {
    json_obj.at("content").get_to(result.content);
    if (json_obj.contains("isError")) {
        result.isError = json_obj.at("isError").get<bool>();
    }
    if (json_obj.contains("_meta")) {
        result.meta = json_obj.at("_meta").get<nlohmann::json>();
    }
    if (json_obj.contains("structuredContent")) {
        result.structuredContent = json_obj.at("structuredContent").get<nlohmann::json>();
    }
}

/**
 * @brief A request to call a tool.
 */
struct CallToolRequest {
    std::string method = "tools/call";  ///< The method name.
    CallToolParams params;              ///< The request parameters.
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(CallToolRequest, method, params)

/**
 * @brief Parameters for listing available tools.
 */
struct ListToolsRequestParams {
    std::optional<std::string> cursor;  ///< Optional pagination cursor.
};
inline void to_json(nlohmann::json& j, const ListToolsRequestParams& p) {
    j = nlohmann::json::object();
    if (p.cursor) {
        j["cursor"] = *p.cursor;
    }
}
inline void from_json(const nlohmann::json& j, ListToolsRequestParams& p) {
    if (j.contains("cursor")) {
        p.cursor = j.at("cursor").get<std::string>();
    }
}

/**
 * @brief A request to list all available tools.
 */
struct ListToolsRequest {
    std::string method = "tools/list";             ///< The method name.
    std::optional<ListToolsRequestParams> params;  ///< Optional request parameters.
};
inline void to_json(nlohmann::json& j, const ListToolsRequest& r) {
    j = nlohmann::json{{"method", r.method}};
    if (r.params) {
        j["params"] = *r.params;
    }
}
inline void from_json(const nlohmann::json& j, ListToolsRequest& r) {
    j.at("method").get_to(r.method);
    if (j.contains("params")) {
        r.params = j.at("params").get<ListToolsRequestParams>();
    }
}

/**
 * @brief Result containing a list of available tools.
 */
struct ListToolsResult {
    std::vector<Tool> tools;                ///< The list of tools in the current page.
    std::optional<std::string> nextCursor;  ///< Pagination cursor for the next page.
    std::optional<nlohmann::json> meta;     ///< Reserved for protocol use.
};
inline void to_json(nlohmann::json& j, const ListToolsResult& r) {
    j = nlohmann::json{{"tools", r.tools}};
    if (r.nextCursor) {
        j["nextCursor"] = *r.nextCursor;
    }
    if (r.meta) {
        j["_meta"] = *r.meta;
    }
}
inline void from_json(const nlohmann::json& j, ListToolsResult& r) {
    j.at("tools").get_to(r.tools);
    if (j.contains("nextCursor")) {
        r.nextCursor = j.at("nextCursor").get<std::string>();
    }
    if (j.contains("_meta")) {
        r.meta = j.at("_meta");
    }
}

}  // namespace mcp

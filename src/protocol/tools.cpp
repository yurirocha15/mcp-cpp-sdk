#include <mcp/detail/json_dump.hpp>
#include <mcp/protocol/tools.hpp>

#include <string>
#include <utility>
#include <variant>
#include <vector>
#include "cache_hints.hpp"

namespace mcp {

CallToolResult make_tool_text_result(std::string text) {
    CallToolResult result;
    TextContent content;
    content.text = std::move(text);
    result.content.emplace_back(std::move(content));
    return result;
}

CallToolResult make_tool_error_result(std::string message) {
    auto result = make_tool_text_result(std::move(message));
    result.isError = true;
    return result;
}

CallToolResult make_tool_structured_result(nlohmann::json structured_content,
                                           std::optional<std::string> text) {
    auto result =
        make_tool_text_result(text ? std::move(*text) : mcp::detail::dump_json(structured_content));
    result.structuredContent = std::move(structured_content);
    return result;
}

std::string result_text(const CallToolResult& result) {
    std::string text;
    bool first = true;
    for (const auto& block : result.content) {
        if (const auto* content = std::get_if<TextContent>(&block)) {
            if (!first) {
                text += '\n';
            }
            text += content->text;
            first = false;
        }
    }
    return text;
}

void to_json(nlohmann::json& json_obj, const ToolAnnotations& annotations) {
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

void from_json(const nlohmann::json& json_obj, ToolAnnotations& annotations) {
    annotations = ToolAnnotations{};
    if (detail::has_json_value(json_obj, "destructiveHint")) {
        annotations.destructiveHint = json_obj.at("destructiveHint").get<bool>();
    }
    if (detail::has_json_value(json_obj, "idempotentHint")) {
        annotations.idempotentHint = json_obj.at("idempotentHint").get<bool>();
    }
    if (detail::has_json_value(json_obj, "openWorldHint")) {
        annotations.openWorldHint = json_obj.at("openWorldHint").get<bool>();
    }
    if (detail::has_json_value(json_obj, "readOnlyHint")) {
        annotations.readOnlyHint = json_obj.at("readOnlyHint").get<bool>();
    }
    if (detail::has_json_value(json_obj, "title")) {
        annotations.title = json_obj.at("title").get<std::string>();
    }
}

void to_json(nlohmann::json& json_obj, const Tool& tool) {
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
}

void from_json(const nlohmann::json& json_obj, Tool& tool) {
    tool = Tool{};
    json_obj.at("name").get_to(tool.name);
    json_obj.at("inputSchema").get_to(tool.inputSchema);
    if (detail::has_json_value(json_obj, "description")) {
        tool.description = json_obj.at("description").get<std::string>();
    }
    if (detail::has_json_value(json_obj, "_meta")) {
        tool.meta = json_obj.at("_meta").get<nlohmann::json>();
    }
    if (detail::has_json_value(json_obj, "annotations")) {
        tool.annotations = json_obj.at("annotations").get<ToolAnnotations>();
    }
    if (detail::has_json_value(json_obj, "outputSchema")) {
        tool.outputSchema = json_obj.at("outputSchema").get<nlohmann::json>();
    }
    if (detail::has_json_value(json_obj, "title")) {
        tool.title = json_obj.at("title").get<std::string>();
    }
    if (detail::has_json_value(json_obj, "icons")) {
        tool.icons = json_obj.at("icons").get<std::vector<Icon>>();
    }
}

void to_json(nlohmann::json& json_obj, const CallToolParams& params) {
    json_obj = nlohmann::json{{"name", params.name}, {"arguments", params.arguments}};
    if (params.meta) {
        json_obj["_meta"] = *params.meta;
    }
}

void from_json(const nlohmann::json& json_obj, CallToolParams& params) {
    params = CallToolParams{};
    json_obj.at("name").get_to(params.name);
    if (json_obj.contains("arguments") && !json_obj.at("arguments").is_object()) {
        throw std::invalid_argument("CallToolParams arguments must be a JSON object");
    }
    params.arguments =
        json_obj.contains("arguments") ? json_obj.at("arguments") : nlohmann::json::object();
    if (detail::has_json_value(json_obj, "_meta")) {
        params.meta = json_obj.at("_meta").get<nlohmann::json>();
    }
}

void to_json(nlohmann::json& json_obj, const CallToolResult& result) {
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

void to_json(nlohmann::json& json_obj, CallToolResult&& result) {
    nlohmann::json content = nlohmann::json::array();
    for (auto& block : result.content) {
        nlohmann::json serialized;
        to_json(serialized, std::move(block));
        content.push_back(std::move(serialized));
    }
    json_obj = nlohmann::json::object();
    json_obj["content"] = std::move(content);
    if (result.isError) {
        json_obj["isError"] = *result.isError;
    }
    if (result.meta) {
        json_obj["_meta"] = std::move(*result.meta);
    }
    if (result.structuredContent) {
        json_obj["structuredContent"] = std::move(*result.structuredContent);
    }
}

void from_json(const nlohmann::json& json_obj, CallToolResult& result) {
    result = CallToolResult{};
    json_obj.at("content").get_to(result.content);
    if (detail::has_json_value(json_obj, "isError")) {
        result.isError = json_obj.at("isError").get<bool>();
    }
    if (detail::has_json_value(json_obj, "_meta")) {
        result.meta = json_obj.at("_meta").get<nlohmann::json>();
    }
    if (detail::has_json_value(json_obj, "structuredContent")) {
        result.structuredContent = json_obj.at("structuredContent").get<nlohmann::json>();
    }
}

void to_json(nlohmann::json& json_obj, const CallToolRequest& value) {
    json_obj["method"] = value.method;
    json_obj["params"] = value.params;
}

void from_json(const nlohmann::json& json_obj, CallToolRequest& value) {
    json_obj.at("method").get_to(value.method);
    json_obj.at("params").get_to(value.params);
}

void to_json(nlohmann::json& j, const ListToolsRequestParams& p) {
    j = nlohmann::json::object();
    if (p.cursor) {
        j["cursor"] = *p.cursor;
    }
}

void from_json(const nlohmann::json& j, ListToolsRequestParams& p) {
    p = ListToolsRequestParams{};
    if (detail::has_json_value(j, "cursor")) {
        p.cursor = j.at("cursor").get<std::string>();
    }
}

void to_json(nlohmann::json& j, const ListToolsRequest& r) {
    j = nlohmann::json{{"method", r.method}};
    if (r.params) {
        j["params"] = *r.params;
    }
}

void from_json(const nlohmann::json& j, ListToolsRequest& r) {
    r = ListToolsRequest{};
    j.at("method").get_to(r.method);
    if (detail::has_json_value(j, "params")) {
        r.params = j.at("params").get<ListToolsRequestParams>();
    }
}

void to_json(nlohmann::json& j, const ListToolsResult& r) {
    j = nlohmann::json{{"tools", r.tools}};
    if (r.nextCursor) {
        j["nextCursor"] = *r.nextCursor;
    }
    if (r.meta) {
        j["_meta"] = *r.meta;
    }
    detail::write_cache_hints(j, r.ttlMs, r.cacheScope);
}

void from_json(const nlohmann::json& j, ListToolsResult& r) {
    r = ListToolsResult{};
    j.at("tools").get_to(r.tools);
    if (detail::has_json_value(j, "nextCursor")) {
        r.nextCursor = j.at("nextCursor").get<std::string>();
    }
    if (detail::has_json_value(j, "_meta")) {
        r.meta = j.at("_meta");
    }
    detail::read_cache_hints(j, r.ttlMs, r.cacheScope);
}

}  // namespace mcp

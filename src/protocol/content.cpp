#include <mcp/protocol/content.hpp>

#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace mcp {

namespace detail {
namespace {

void validate_content_type(std::string_view actual, std::string_view expected) {
    if (actual != expected) {
        throw std::invalid_argument("content type must be \"" + std::string(expected) + "\"");
    }
}

}  // namespace
}  // namespace detail

void to_json(nlohmann::json& json_obj, const TextResourceContents& content) {
    json_obj = nlohmann::json{{"uri", content.uri}, {"text", content.text}};
    if (content.mimeType) {
        json_obj["mimeType"] = *content.mimeType;
    }
    if (content.meta) {
        json_obj["_meta"] = *content.meta;
    }
}

void from_json(const nlohmann::json& json_obj, TextResourceContents& content) {
    json_obj.at("uri").get_to(content.uri);
    json_obj.at("text").get_to(content.text);
    if (detail::has_json_value(json_obj, "mimeType")) {
        content.mimeType = json_obj.at("mimeType").get<std::string>();
    }
    if (detail::has_json_value(json_obj, "_meta")) {
        content.meta = json_obj.at("_meta");
    }
}

void to_json(nlohmann::json& json_obj, const BlobResourceContents& content) {
    json_obj = nlohmann::json{{"uri", content.uri}, {"blob", content.blob}};
    if (content.mimeType) {
        json_obj["mimeType"] = *content.mimeType;
    }
    if (content.meta) {
        json_obj["_meta"] = *content.meta;
    }
}

void from_json(const nlohmann::json& json_obj, BlobResourceContents& content) {
    json_obj.at("uri").get_to(content.uri);
    json_obj.at("blob").get_to(content.blob);
    if (detail::has_json_value(json_obj, "mimeType")) {
        content.mimeType = json_obj.at("mimeType").get<std::string>();
    }
    if (detail::has_json_value(json_obj, "_meta")) {
        content.meta = json_obj.at("_meta");
    }
}

void to_json(nlohmann::json& json_obj, const ResourceContents& content) {
    std::visit([&json_obj](auto&& arg) { json_obj = arg; }, content);
}

void from_json(const nlohmann::json& json_obj, ResourceContents& content) {
    if (json_obj.contains("text")) {
        content = json_obj.get<TextResourceContents>();
    } else if (json_obj.contains("blob")) {
        content = json_obj.get<BlobResourceContents>();
    } else {
        throw std::invalid_argument("Unknown resource contents type");
    }
}

void to_json(nlohmann::json& json_obj, const ResourceLink& link) {
    json_obj = nlohmann::json{{"type", link.type}, {"uri", link.uri}, {"name", link.name}};
    if (link.description) {
        json_obj["description"] = *link.description;
    }
    if (link.mimeType) {
        json_obj["mimeType"] = *link.mimeType;
    }
    if (link.size) {
        json_obj["size"] = *link.size;
    }
    if (link.meta) {
        json_obj["_meta"] = *link.meta;
    }
    if (link.annotations) {
        json_obj["annotations"] = *link.annotations;
    }
    if (link.title) {
        json_obj["title"] = *link.title;
    }
    if (link.icons) {
        json_obj["icons"] = *link.icons;
    }
}

void from_json(const nlohmann::json& json_obj, ResourceLink& link) {
    json_obj.at("type").get_to(link.type);
    detail::validate_content_type(link.type, "resource_link");
    json_obj.at("uri").get_to(link.uri);
    json_obj.at("name").get_to(link.name);
    if (detail::has_json_value(json_obj, "description")) {
        link.description = json_obj.at("description").get<std::string>();
    }
    if (detail::has_json_value(json_obj, "mimeType")) {
        link.mimeType = json_obj.at("mimeType").get<std::string>();
    }
    if (detail::has_json_value(json_obj, "size")) {
        link.size = json_obj.at("size").get<int64_t>();
    }
    if (json_obj.contains("_meta")) {
        link.meta = json_obj.at("_meta").get<nlohmann::json>();
    }
    if (json_obj.contains("annotations")) {
        link.annotations = json_obj.at("annotations").get<Annotations>();
    }
    if (detail::has_json_value(json_obj, "title")) {
        link.title = json_obj.at("title").get<std::string>();
    }
    if (detail::has_json_value(json_obj, "icons")) {
        link.icons = json_obj.at("icons").get<std::vector<Icon>>();
    }
}

void to_json(nlohmann::json& json_obj, const EmbeddedResource& res) {
    json_obj = nlohmann::json{{"type", res.type}, {"resource", res.resource}};
    if (res.meta) {
        json_obj["_meta"] = *res.meta;
    }
    if (res.annotations) {
        json_obj["annotations"] = *res.annotations;
    }
}

void from_json(const nlohmann::json& json_obj, EmbeddedResource& res) {
    json_obj.at("type").get_to(res.type);
    detail::validate_content_type(res.type, "resource");
    json_obj.at("resource").get_to(res.resource);
    if (json_obj.contains("_meta")) {
        res.meta = json_obj.at("_meta").get<nlohmann::json>();
    }
    if (json_obj.contains("annotations")) {
        res.annotations = json_obj.at("annotations").get<Annotations>();
    }
}

void to_json(nlohmann::json& json_obj, const ToolUseContent& content) {
    json_obj = nlohmann::json{
        {"type", content.type}, {"id", content.id}, {"name", content.name}, {"input", content.input}};
    if (content.meta) {
        json_obj["_meta"] = *content.meta;
    }
}

void from_json(const nlohmann::json& json_obj, ToolUseContent& content) {
    json_obj.at("type").get_to(content.type);
    detail::validate_content_type(content.type, "tool_use");
    json_obj.at("id").get_to(content.id);
    json_obj.at("name").get_to(content.name);
    json_obj.at("input").get_to(content.input);
    if (json_obj.contains("_meta")) {
        content.meta = json_obj.at("_meta").get<nlohmann::json>();
    }
}

void to_json(nlohmann::json& json_obj, const ToolResultContent& content) {
    json_obj = nlohmann::json{
        {"type", content.type}, {"toolUseId", content.toolUseId}, {"content", content.content}};
    if (content.isError) {
        json_obj["isError"] = *content.isError;
    }
    if (content.structuredContent) {
        json_obj["structuredContent"] = *content.structuredContent;
    }
    if (content.meta) {
        json_obj["_meta"] = *content.meta;
    }
}

void from_json(const nlohmann::json& json_obj, ToolResultContent& content) {
    json_obj.at("type").get_to(content.type);
    detail::validate_content_type(content.type, "tool_result");
    json_obj.at("toolUseId").get_to(content.toolUseId);
    json_obj.at("content").get_to(content.content);
    if (detail::has_json_value(json_obj, "isError")) {
        content.isError = json_obj.at("isError").get<bool>();
    }
    if (detail::has_json_value(json_obj, "structuredContent")) {
        content.structuredContent = json_obj.at("structuredContent").get<nlohmann::json>();
    }
    if (detail::has_json_value(json_obj, "_meta")) {
        content.meta = json_obj.at("_meta").get<nlohmann::json>();
    }
}

void to_json(nlohmann::json& json_obj, const TextContent& content) {
    json_obj = nlohmann::json{{"type", content.type}, {"text", content.text}};
}

void to_json(nlohmann::json& json_obj, TextContent&& content) {
    json_obj = nlohmann::json::object();
    json_obj["type"] = std::move(content.type);
    json_obj["text"] = std::move(content.text);
}

void from_json(const nlohmann::json& json_obj, TextContent& content) {
    json_obj.at("type").get_to(content.type);
    detail::validate_content_type(content.type, "text");
    json_obj.at("text").get_to(content.text);
}

void to_json(nlohmann::json& json_obj, const ImageContent& content) {
    json_obj =
        nlohmann::json{{"type", content.type}, {"data", content.data}, {"mimeType", content.mimeType}};
}

void from_json(const nlohmann::json& json_obj, ImageContent& content) {
    json_obj.at("type").get_to(content.type);
    detail::validate_content_type(content.type, "image");
    json_obj.at("data").get_to(content.data);
    json_obj.at("mimeType").get_to(content.mimeType);
}

void to_json(nlohmann::json& json_obj, const AudioContent& content) {
    json_obj =
        nlohmann::json{{"type", content.type}, {"data", content.data}, {"mimeType", content.mimeType}};
}

void from_json(const nlohmann::json& json_obj, AudioContent& content) {
    json_obj.at("type").get_to(content.type);
    detail::validate_content_type(content.type, "audio");
    json_obj.at("data").get_to(content.data);
    json_obj.at("mimeType").get_to(content.mimeType);
}

void to_json(nlohmann::json& json_obj, const ContentBlock& content) {
    std::visit([&json_obj](auto&& arg) { json_obj = arg; }, content);
}

void to_json(nlohmann::json& json_obj, ContentBlock&& content) {
    if (auto* text = std::get_if<TextContent>(&content)) {
        to_json(json_obj, std::move(*text));
        return;
    }
    to_json(json_obj, static_cast<const ContentBlock&>(content));
}

void from_json(const nlohmann::json& json_obj, ContentBlock& content) {
    std::string type;
    json_obj.at("type").get_to(type);
    if (type == "text") {
        content = json_obj.get<TextContent>();
    } else if (type == "image") {
        content = json_obj.get<ImageContent>();

    } else if (type == "audio") {
        content = json_obj.get<AudioContent>();
    } else if (type == "resource_link") {
        content = json_obj.get<ResourceLink>();
    } else if (type == "resource") {
        content = json_obj.get<EmbeddedResource>();
    } else if (type == "tool_use") {
        content = json_obj.get<ToolUseContent>();
    } else if (type == "tool_result") {
        content = json_obj.get<ToolResultContent>();
    } else {
        throw std::invalid_argument("Unknown content type: " + type);
    }
}

}  // namespace mcp

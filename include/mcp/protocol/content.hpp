#pragma once

#include <mcp/protocol/base.hpp>
#include <mcp/protocol/capabilities.hpp>

namespace mcp {

/**
 * @brief Represents text content.
 */
struct TextContent {
    std::string type = "text";  ///< The type of content (always "text").
    std::string text;           ///< The text content.
};

/**
 * @brief Represents image content.
 */
struct ImageContent {
    std::string type = "image";  ///< The type of content (always "image").
    std::string data;            ///< The base64-encoded image data.
    std::string mimeType;        ///< The MIME type of the image.
};

/**
 * @brief Represents text resource contents.
 */
struct TextResourceContents {
    std::string uri;                      ///< The URI of this resource.
    std::string text;                     ///< The text of the item.
    std::optional<std::string> mimeType;  ///< The MIME type of this resource.
};

/**
 * @brief Represents blob resource contents.
 */
struct BlobResourceContents {
    std::string uri;                      ///< The URI of this resource.
    std::string blob;                     ///< The base64-encoded binary data.
    std::optional<std::string> mimeType;  ///< The MIME type of this resource.
};

/**
 * @brief Variant holding either TextResourceContents or BlobResourceContents.
 */
using ResourceContents = std::variant<TextResourceContents, BlobResourceContents>;

inline void to_json(nlohmann::json& json_obj, const TextResourceContents& content) {
    json_obj = nlohmann::json{{"uri", content.uri}, {"text", content.text}};
    if (content.mimeType) {
        json_obj["mimeType"] = *content.mimeType;
    }
}

inline void from_json(const nlohmann::json& json_obj, TextResourceContents& content) {
    json_obj.at("uri").get_to(content.uri);
    json_obj.at("text").get_to(content.text);
    if (json_obj.contains("mimeType")) {
        content.mimeType = json_obj.at("mimeType").get<std::string>();
    }
}

inline void to_json(nlohmann::json& json_obj, const BlobResourceContents& content) {
    json_obj = nlohmann::json{{"uri", content.uri}, {"blob", content.blob}};
    if (content.mimeType) {
        json_obj["mimeType"] = *content.mimeType;
    }
}

inline void from_json(const nlohmann::json& json_obj, BlobResourceContents& content) {
    json_obj.at("uri").get_to(content.uri);
    json_obj.at("blob").get_to(content.blob);
    if (json_obj.contains("mimeType")) {
        content.mimeType = json_obj.at("mimeType").get<std::string>();
    }
}

inline void to_json(nlohmann::json& json_obj, const ResourceContents& content) {
    std::visit([&json_obj](auto&& arg) { json_obj = arg; }, content);
}

inline void from_json(const nlohmann::json& json_obj, ResourceContents& content) {
    if (json_obj.contains("text")) {
        content = json_obj.get<TextResourceContents>();
    } else if (json_obj.contains("blob")) {
        content = json_obj.get<BlobResourceContents>();
    } else {
        throw std::invalid_argument("Unknown resource contents type");
    }
}

/**
 * @brief Represents audio content.
 */
struct AudioContent {
    std::string type = "audio";  ///< The type of content (always "audio").
    std::string data;            ///< The base64-encoded audio data.
    std::string mimeType;        ///< The MIME type of the audio.
};

/**
 * @brief A resource that the server is capable of reading.
 */
struct ResourceLink {
    std::string type = "resource_link";      ///< Content discriminator for resource-link blocks.
    std::string uri;                         ///< Resource URI.
    std::string name;                        ///< Human-readable resource name.
    std::optional<std::string> description;  ///< Optional resource description.
    std::optional<std::string> mimeType;     ///< Optional MIME type for the resource.
    std::optional<int64_t> size;             ///< Optional size hint in bytes.
    std::optional<nlohmann::json> meta;      ///< Reserved for protocol use.
    std::optional<Annotations> annotations;  ///< Optional display annotations.
    std::optional<std::string> title;        ///< Optional display title.
    std::optional<std::vector<Icon>> icons;  ///< Optional resource icons.
};

inline void to_json(nlohmann::json& json_obj, const ResourceLink& link) {
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

inline void from_json(const nlohmann::json& json_obj, ResourceLink& link) {
    json_obj.at("type").get_to(link.type);
    json_obj.at("uri").get_to(link.uri);
    json_obj.at("name").get_to(link.name);
    if (json_obj.contains("description")) {
        link.description = json_obj.at("description").get<std::string>();
    }
    if (json_obj.contains("mimeType")) {
        link.mimeType = json_obj.at("mimeType").get<std::string>();
    }
    if (json_obj.contains("size")) {
        link.size = json_obj.at("size").get<int64_t>();
    }
    if (json_obj.contains("_meta")) {
        link.meta = json_obj.at("_meta").get<nlohmann::json>();
    }
    if (json_obj.contains("annotations")) {
        link.annotations = json_obj.at("annotations").get<Annotations>();
    }
    if (json_obj.contains("title")) {
        link.title = json_obj.at("title").get<std::string>();
    }
    if (json_obj.contains("icons")) {
        link.icons = json_obj.at("icons").get<std::vector<Icon>>();
    }
}

/**
 * @brief The contents of a resource, embedded into a prompt or tool call result.
 */
struct EmbeddedResource {
    std::string type = "resource";           ///< Content discriminator for embedded resources.
    ResourceContents resource;               ///< Embedded resource payload.
    std::optional<nlohmann::json> meta;      ///< Reserved for protocol use.
    std::optional<Annotations> annotations;  ///< Optional display annotations.
};

inline void to_json(nlohmann::json& json_obj, const EmbeddedResource& res) {
    json_obj = nlohmann::json{{"type", res.type}, {"resource", res.resource}};
    if (res.meta) {
        json_obj["_meta"] = *res.meta;
    }
    if (res.annotations) {
        json_obj["annotations"] = *res.annotations;
    }
}

inline void from_json(const nlohmann::json& json_obj, EmbeddedResource& res) {
    json_obj.at("type").get_to(res.type);
    json_obj.at("resource").get_to(res.resource);
    if (json_obj.contains("_meta")) {
        res.meta = json_obj.at("_meta").get<nlohmann::json>();
    }
    if (json_obj.contains("annotations")) {
        res.annotations = json_obj.at("annotations").get<Annotations>();
    }
}

/**
 * @brief Content block describing a tool invocation requested by the model.
 */
struct ToolUseContent {
    std::string type = "tool_use";       ///< Content discriminator for tool-use blocks.
    std::string id;                      ///< Unique identifier for correlating tool results.
    std::string name;                    ///< Name of the tool to invoke.
    nlohmann::json input;                ///< Structured tool input payload.
    std::optional<nlohmann::json> meta;  ///< Reserved for protocol use.
};

inline void to_json(nlohmann::json& json_obj, const ToolUseContent& content) {
    json_obj = nlohmann::json{
        {"type", content.type}, {"id", content.id}, {"name", content.name}, {"input", content.input}};
    if (content.meta) {
        json_obj["_meta"] = *content.meta;
    }
}

inline void from_json(const nlohmann::json& json_obj, ToolUseContent& content) {
    json_obj.at("type").get_to(content.type);
    json_obj.at("id").get_to(content.id);
    json_obj.at("name").get_to(content.name);
    json_obj.at("input").get_to(content.input);
    if (json_obj.contains("_meta")) {
        content.meta = json_obj.at("_meta").get<nlohmann::json>();
    }
}

/**
 * @brief Content block carrying the result of a prior tool invocation.
 */
struct ToolResultContent {
    std::string type = "tool_result";  ///< Content discriminator for tool-result blocks.
    std::string toolUseId;             ///< Identifier of the matching ToolUseContent block.
    nlohmann::json content;            ///< Human-readable result content payload.
    std::optional<bool> isError;       ///< Whether the tool result represents an error.
    std::optional<nlohmann::json> structuredContent;  ///< Optional structured result payload.
    std::optional<nlohmann::json> meta;               ///< Reserved for protocol use.
};

inline void to_json(nlohmann::json& json_obj, const ToolResultContent& content) {
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

inline void from_json(const nlohmann::json& json_obj, ToolResultContent& content) {
    json_obj.at("type").get_to(content.type);
    json_obj.at("toolUseId").get_to(content.toolUseId);
    json_obj.at("content").get_to(content.content);
    if (json_obj.contains("isError")) {
        content.isError = json_obj.at("isError").get<bool>();
    }
    if (json_obj.contains("structuredContent")) {
        content.structuredContent = json_obj.at("structuredContent").get<nlohmann::json>();
    }
    if (json_obj.contains("_meta")) {
        content.meta = json_obj.at("_meta").get<nlohmann::json>();
    }
}

/**
 * @brief Serializes TextContent to JSON.
 *
 * @param json_obj The JSON object to populate.
 * @param content The TextContent object to serialize.
 */
inline void to_json(nlohmann::json& json_obj, const TextContent& content) {
    json_obj = nlohmann::json{{"type", content.type}, {"text", content.text}};
}

/**
 * @brief Deserializes TextContent from JSON.
 *
 * @param json_obj The JSON object to read from.
 * @param content The TextContent object to populate.
 */
inline void from_json(const nlohmann::json& json_obj, TextContent& content) {
    json_obj.at("type").get_to(content.type);
    json_obj.at("text").get_to(content.text);
}

/**
 * @brief Serializes ImageContent to JSON.
 *
 * @param json_obj The JSON object to populate.
 * @param content The ImageContent object to serialize.
 */
inline void to_json(nlohmann::json& json_obj, const ImageContent& content) {
    json_obj =
        nlohmann::json{{"type", content.type}, {"data", content.data}, {"mimeType", content.mimeType}};
}

/**
 * @brief Deserializes ImageContent from JSON.
 *
 * @param json_obj The JSON object to read from.
 * @param content The ImageContent object to populate.
 */
inline void from_json(const nlohmann::json& json_obj, ImageContent& content) {
    json_obj.at("type").get_to(content.type);
    json_obj.at("data").get_to(content.data);
    json_obj.at("mimeType").get_to(content.mimeType);
}

/**
 * @brief Serializes AudioContent to JSON.
 *
 * @param json_obj The JSON object to populate.
 * @param content The AudioContent object to serialize.
 */
inline void to_json(nlohmann::json& json_obj, const AudioContent& content) {
    json_obj =
        nlohmann::json{{"type", content.type}, {"data", content.data}, {"mimeType", content.mimeType}};
}

/**
 * @brief Deserializes AudioContent from JSON.
 *
 * @param json_obj The JSON object to read from.
 * @param content The AudioContent object to populate.
 */
inline void from_json(const nlohmann::json& json_obj, AudioContent& content) {
    json_obj.at("type").get_to(content.type);
    json_obj.at("data").get_to(content.data);
    json_obj.at("mimeType").get_to(content.mimeType);
}

/**
 * @brief Variant of all supported message content block types.
 */
using ContentBlock = std::variant<TextContent, ImageContent, AudioContent, ResourceLink,
                                  EmbeddedResource, ToolUseContent, ToolResultContent>;

/**
 * @brief Serializes Content variant to JSON.
 *
 * @param json_obj The JSON object to populate.
 * @param content The Content variant to serialize.
 */
inline void to_json(nlohmann::json& json_obj, const ContentBlock& content) {
    std::visit([&json_obj](auto&& arg) { json_obj = arg; }, content);
}

/**
 * @brief Deserializes Content variant from JSON.
 *
 * @param json_obj The JSON object to read from.
 * @param content The Content variant to populate.
 * @throw std::invalid_argument If the content type is unknown.
 */
inline void from_json(const nlohmann::json& json_obj, ContentBlock& content) {
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

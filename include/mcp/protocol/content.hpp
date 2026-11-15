#pragma once

#include <mcp/core/export.hpp>
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
    std::optional<nlohmann::json> meta;   ///< Reserved for protocol use (`_meta`).
};

/**
 * @brief Represents blob resource contents.
 */
struct BlobResourceContents {
    std::string uri;                      ///< The URI of this resource.
    std::string blob;                     ///< The base64-encoded binary data.
    std::optional<std::string> mimeType;  ///< The MIME type of this resource.
    std::optional<nlohmann::json> meta;   ///< Reserved for protocol use (`_meta`).
};

/**
 * @brief Variant holding either TextResourceContents or BlobResourceContents.
 */
using ResourceContents = std::variant<TextResourceContents, BlobResourceContents>;

MCP_API void to_json(nlohmann::json& json_obj, const TextResourceContents& content);

MCP_API void from_json(const nlohmann::json& json_obj, TextResourceContents& content);

MCP_API void to_json(nlohmann::json& json_obj, const BlobResourceContents& content);

MCP_API void from_json(const nlohmann::json& json_obj, BlobResourceContents& content);

MCP_API void to_json(nlohmann::json& json_obj, const ResourceContents& content);

MCP_API void from_json(const nlohmann::json& json_obj, ResourceContents& content);

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

MCP_API void to_json(nlohmann::json& json_obj, const ResourceLink& link);

MCP_API void from_json(const nlohmann::json& json_obj, ResourceLink& link);

/**
 * @brief The contents of a resource, embedded into a prompt or tool call result.
 */
struct EmbeddedResource {
    std::string type = "resource";           ///< Content discriminator for embedded resources.
    ResourceContents resource;               ///< Embedded resource payload.
    std::optional<nlohmann::json> meta;      ///< Reserved for protocol use.
    std::optional<Annotations> annotations;  ///< Optional display annotations.
};

MCP_API void to_json(nlohmann::json& json_obj, const EmbeddedResource& res);

MCP_API void from_json(const nlohmann::json& json_obj, EmbeddedResource& res);

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

MCP_API void to_json(nlohmann::json& json_obj, const ToolUseContent& content);

MCP_API void from_json(const nlohmann::json& json_obj, ToolUseContent& content);

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

MCP_API void to_json(nlohmann::json& json_obj, const ToolResultContent& content);

MCP_API void from_json(const nlohmann::json& json_obj, ToolResultContent& content);

/**
 * @brief Serializes TextContent to JSON.
 *
 * @param json_obj The JSON object to populate.
 * @param content The TextContent object to serialize.
 */
MCP_API void to_json(nlohmann::json& json_obj, const TextContent& content);

/**
 * @brief Serializes TextContent to JSON, moving its text into the result.
 *
 * @param json_obj The JSON object to populate.
 * @param content The TextContent object to serialize; left with an unspecified text.
 */
MCP_API void to_json(nlohmann::json& json_obj, TextContent&& content);

/**
 * @brief Deserializes TextContent from JSON.
 *
 * @param json_obj The JSON object to read from.
 * @param content The TextContent object to populate.
 */
MCP_API void from_json(const nlohmann::json& json_obj, TextContent& content);

/**
 * @brief Serializes ImageContent to JSON.
 *
 * @param json_obj The JSON object to populate.
 * @param content The ImageContent object to serialize.
 */
MCP_API void to_json(nlohmann::json& json_obj, const ImageContent& content);

/**
 * @brief Deserializes ImageContent from JSON.
 *
 * @param json_obj The JSON object to read from.
 * @param content The ImageContent object to populate.
 */
MCP_API void from_json(const nlohmann::json& json_obj, ImageContent& content);

/**
 * @brief Serializes AudioContent to JSON.
 *
 * @param json_obj The JSON object to populate.
 * @param content The AudioContent object to serialize.
 */
MCP_API void to_json(nlohmann::json& json_obj, const AudioContent& content);

/**
 * @brief Deserializes AudioContent from JSON.
 *
 * @param json_obj The JSON object to read from.
 * @param content The AudioContent object to populate.
 */
MCP_API void from_json(const nlohmann::json& json_obj, AudioContent& content);

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
MCP_API void to_json(nlohmann::json& json_obj, const ContentBlock& content);

/**
 * @brief Serializes a Content variant to JSON, moving what it holds into the result.
 *
 * @param json_obj The JSON object to populate.
 * @param content The Content variant to serialize; left in an unspecified state.
 */
MCP_API void to_json(nlohmann::json& json_obj, ContentBlock&& content);

/**
 * @brief Deserializes Content variant from JSON.
 *
 * @param json_obj The JSON object to read from.
 * @param content The Content variant to populate.
 * @throw std::invalid_argument If the content type is unknown.
 */
MCP_API void from_json(const nlohmann::json& json_obj, ContentBlock& content);

}  // namespace mcp

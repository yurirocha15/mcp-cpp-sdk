#pragma once

#include <array>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace mcp {

/**
 * @brief Protocol version 2024-11-05.
 */
constexpr std::string_view PROTOCOL_VERSION_2024_11_05 = "2024-11-05";

/**
 * @brief Protocol version 2025-03-26.
 */
constexpr std::string_view PROTOCOL_VERSION_2025_03_26 = "2025-03-26";

/**
 * @brief Protocol version 2025-06-18.
 */
constexpr std::string_view PROTOCOL_VERSION_2025_06_18 = "2025-06-18";

/**
 * @brief The latest supported protocol version.
 */
constexpr std::string_view LATEST_PROTOCOL_VERSION = PROTOCOL_VERSION_2025_06_18;

/**
 * @brief List of all supported protocol versions.
 */
constexpr std::array<std::string_view, 3> SUPPORTED_PROTOCOL_VERSIONS = {
    PROTOCOL_VERSION_2024_11_05, PROTOCOL_VERSION_2025_03_26, PROTOCOL_VERSION_2025_06_18};

/**
 * @brief The sender or recipient of messages and data in a conversation.
 */
enum class Role : std::uint8_t {
    User,      ///< The user.
    Assistant  ///< The assistant (LLM).
};

NLOHMANN_JSON_SERIALIZE_ENUM(Role, {{Role::User, "user"}, {Role::Assistant, "assistant"}})

/**
 * @brief Provides visual identifiers for resources, tools, prompts, and implementations.
 */
struct Icon {
    std::string source;                             ///< URI pointing to the icon resource.
    std::optional<std::string> mimeType;            ///< Optional MIME type.
    std::optional<std::vector<std::string>> sizes;  ///< Optional size specification.
    std::optional<std::string> theme;               ///< Optional theme (e.g., "light" or "dark").
};

/**
 * @brief Serializes Icon to JSON.
 *
 * @param json_obj The JSON object to populate.
 * @param icon The Icon object to serialize.
 */
inline void to_json(nlohmann::json& json_obj, const Icon& icon) {
    json_obj = nlohmann::json{{"src", icon.source}};
    if (icon.mimeType) {
        json_obj["mimeType"] = *icon.mimeType;
    }
    if (icon.sizes) {
        json_obj["sizes"] = *icon.sizes;
    }
    if (icon.theme) {
        json_obj["theme"] = *icon.theme;
    }
}

/**
 * @brief Deserializes Icon from JSON.
 *
 * @param json_obj The JSON object to read from.
 * @param icon The Icon object to populate.
 */
inline void from_json(const nlohmann::json& json_obj, Icon& icon) {
    json_obj.at("src").get_to(icon.source);
    if (json_obj.contains("mimeType")) {
        icon.mimeType = json_obj.at("mimeType").get<std::string>();
    }
    if (json_obj.contains("sizes")) {
        icon.sizes = json_obj.at("sizes").get<std::vector<std::string>>();
    }
    if (json_obj.contains("theme")) {
        icon.theme = json_obj.at("theme").get<std::string>();
    }
}

/**
 * @brief Optional annotations for the client.
 */
struct Annotations {
    std::optional<std::vector<Role>> audience;  ///< Describes who the intended customer is.
    std::optional<std::string> lastModified;    ///< ISO 8601 formatted string of last modification.
    std::optional<double> priority;             ///< Importance of data (0.0 to 1.0).
};

/**
 * @brief Serializes Annotations to JSON.
 *
 * @param json_obj The JSON object to populate.
 * @param annotations The Annotations object to serialize.
 */
inline void to_json(nlohmann::json& json_obj, const Annotations& annotations) {
    json_obj = nlohmann::json::object();
    if (annotations.audience) {
        json_obj["audience"] = *annotations.audience;
    }
    if (annotations.lastModified) {
        json_obj["lastModified"] = *annotations.lastModified;
    }
    if (annotations.priority) {
        json_obj["priority"] = *annotations.priority;
    }
}

/**
 * @brief Deserializes Annotations from JSON.
 *
 * @param json_obj The JSON object to read from.
 * @param annotations The Annotations object to populate.
 */
inline void from_json(const nlohmann::json& json_obj, Annotations& annotations) {
    if (json_obj.contains("audience")) {
        annotations.audience = json_obj.at("audience").get<std::vector<Role>>();
    }
    if (json_obj.contains("lastModified")) {
        annotations.lastModified = json_obj.at("lastModified").get<std::string>();
    }
    if (json_obj.contains("priority")) {
        annotations.priority = json_obj.at("priority").get<double>();
    }
}

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
 * @brief Describes the name and version of an MCP implementation.
 */
/**
 * @brief Describes the name and version of an MCP implementation.
 */
struct Implementation {
    std::string name;                        ///< The name of the implementation.
    std::string version;                     ///< The version of the implementation.
    std::optional<std::string> title;        ///< The title of the implementation.
    std::optional<std::string> websiteUrl;   ///< The website URL of the implementation.
    std::optional<std::vector<Icon>> icons;  ///< Icons for the implementation.
};

/**
 * @brief Serializes Implementation to JSON.
 *
 * @param json_obj The JSON object to populate.
 * @param impl The Implementation object to serialize.
 */
inline void to_json(nlohmann::json& json_obj, const Implementation& impl) {
    json_obj = nlohmann::json{{"name", impl.name}, {"version", impl.version}};
    if (impl.title) {
        json_obj["title"] = *impl.title;
    }
    if (impl.websiteUrl) {
        json_obj["websiteUrl"] = *impl.websiteUrl;
    }
    if (impl.icons) {
        json_obj["icons"] = *impl.icons;
    }
}

/**
 * @brief Deserializes Implementation from JSON.
 *
 * @param json_obj The JSON object to read from.
 * @param impl The Implementation object to populate.
 */
inline void from_json(const nlohmann::json& json_obj, Implementation& impl) {
    json_obj.at("name").get_to(impl.name);
    json_obj.at("version").get_to(impl.version);
    if (json_obj.contains("title")) {
        impl.title = json_obj.at("title").get<std::string>();
    }
    if (json_obj.contains("websiteUrl")) {
        impl.websiteUrl = json_obj.at("websiteUrl").get<std::string>();
    }
    if (json_obj.contains("icons")) {
        impl.icons = json_obj.at("icons").get<std::vector<Icon>>();
    }
}

/**
 * @brief Alias for Implementation, representing client information.
 */
using ClientInfo = Implementation;

/**
 * @brief Alias for Implementation, representing server information.
 */
using ServerInfo = Implementation;

/**
 * @brief Represents an initialization request from the client.
 */
struct InitializeRequest {
    std::string protocolVersion;  ///< The protocol version supported by the client.
    ClientInfo clientInfo;        ///< Information about the client implementation.
    nlohmann::json capabilities;  ///< The capabilities supported by the client.
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(InitializeRequest, protocolVersion, clientInfo, capabilities)

/**
 * @brief Represents the result of an initialization request.
 */
struct InitializeResult {
    std::string protocolVersion;              ///< The protocol version selected by the server.
    nlohmann::json capabilities;              ///< The capabilities supported by the server.
    ServerInfo serverInfo;                    ///< Information about the server implementation.
    std::optional<std::string> instructions;  ///< Optional instructions for the client.
};

/**
 * @brief Serializes InitializeResult to JSON.
 *
 * @param json_obj The JSON object to populate.
 * @param res The InitializeResult object to serialize.
 */
inline void to_json(nlohmann::json& json_obj, const InitializeResult& res) {
    json_obj = nlohmann::json{{"protocolVersion", res.protocolVersion},
                              {"capabilities", res.capabilities},
                              {"serverInfo", res.serverInfo}};
    if (res.instructions) {
        json_obj["instructions"] = *res.instructions;
    }
}

/**
 * @brief Deserializes InitializeResult from JSON.
 *
 * @param json_obj The JSON object to read from.
 * @param res The InitializeResult object to populate.
 */
inline void from_json(const nlohmann::json& json_obj, InitializeResult& res) {
    json_obj.at("protocolVersion").get_to(res.protocolVersion);
    json_obj.at("capabilities").get_to(res.capabilities);
    json_obj.at("serverInfo").get_to(res.serverInfo);
    if (json_obj.contains("instructions")) {
        res.instructions = json_obj.at("instructions").get<std::string>();
    }
}

// Content Primitives

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
    std::string type = "resource_link";
    std::string uri;
    std::string name;
    std::optional<std::string> description;
    std::optional<std::string> mimeType;
    std::optional<int64_t> size;
    std::optional<nlohmann::json> meta;
    std::optional<Annotations> annotations;
    std::optional<std::string> title;
    std::optional<std::vector<Icon>> icons;
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
    std::string type = "resource";
    ResourceContents resource;
    std::optional<nlohmann::json> meta;
    std::optional<Annotations> annotations;
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
 * @brief Variant holding either TextContent or ImageContent.
 */
using ContentBlock =
    std::variant<TextContent, ImageContent, AudioContent, ResourceLink, EmbeddedResource>;

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
    } else {
        throw std::invalid_argument("Unknown content type: " + type);
    }
}

// Tool Primitives

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

// Resource Primitives

/**
 * @brief Represents a resource definition.
 */
struct Resource {
    std::string uri;                         ///< The URI of the resource.
    std::string name;                        ///< The name of the resource.
    std::optional<std::string> description;  ///< Optional description of the resource.
    std::optional<std::string> mimeType;     ///< Optional MIME type of the resource.
    std::optional<nlohmann::json> meta;      ///< Reserved for protocol use.
    std::optional<Annotations> annotations;  ///< Optional annotations for the client.
    std::optional<int64_t> size;             ///< Size of the raw resource content in bytes.
    std::optional<std::string> title;        ///< Human-readable title for the resource.
    std::optional<std::vector<Icon>> icons;  ///< Icons for the resource.
};

/**
 * @brief Serializes Resource to JSON.
 *
 * @param json_obj The JSON object to populate.
 * @param resource The Resource object to serialize.
 */
inline void to_json(nlohmann::json& json_obj, const Resource& resource) {
    json_obj = nlohmann::json{{"uri", resource.uri}, {"name", resource.name}};
    if (resource.description) {
        json_obj["description"] = *resource.description;
    }
    if (resource.mimeType) {
        json_obj["mimeType"] = *resource.mimeType;
    }
    if (resource.meta) {
        json_obj["_meta"] = *resource.meta;
    }
    if (resource.annotations) {
        json_obj["annotations"] = *resource.annotations;
    }
    if (resource.size) {
        json_obj["size"] = *resource.size;
    }
    if (resource.title) {
        json_obj["title"] = *resource.title;
    }
    if (resource.icons) {
        json_obj["icons"] = *resource.icons;
    }
}

/**
 * @brief Deserializes Resource from JSON.
 *
 * @param json_obj The JSON object to read from.
 * @param resource The Resource object to populate.
 */
inline void from_json(const nlohmann::json& json_obj, Resource& resource) {
    json_obj.at("uri").get_to(resource.uri);
    json_obj.at("name").get_to(resource.name);
    if (json_obj.contains("description")) {
        resource.description = json_obj.at("description").get<std::string>();
    }
    if (json_obj.contains("mimeType")) {
        resource.mimeType = json_obj.at("mimeType").get<std::string>();
    }
    if (json_obj.contains("_meta")) {
        resource.meta = json_obj.at("_meta").get<nlohmann::json>();
    }
    if (json_obj.contains("annotations")) {
        resource.annotations = json_obj.at("annotations").get<Annotations>();
    }
    if (json_obj.contains("size")) {
        resource.size = json_obj.at("size").get<int64_t>();
    }
    if (json_obj.contains("title")) {
        resource.title = json_obj.at("title").get<std::string>();
    }
    if (json_obj.contains("icons")) {
        resource.icons = json_obj.at("icons").get<std::vector<Icon>>();
    }
}

/**
 * @brief A template description for resources available on the server.
 */
struct ResourceTemplate {
    std::string uriTemplate;                 ///< A URI template (according to RFC 6570).
    std::string name;                        ///< The name of the template.
    std::optional<std::string> description;  ///< Optional description.
    std::optional<std::string> mimeType;     ///< Optional MIME type.
    std::optional<nlohmann::json> meta;      ///< Reserved for protocol use.
    std::optional<Annotations> annotations;  ///< Optional annotations.
    std::optional<std::string> title;        ///< Human-readable title.
    std::optional<std::vector<Icon>> icons;  ///< Icons for the template.
};

inline void to_json(nlohmann::json& json_obj, const ResourceTemplate& tmpl) {
    json_obj = nlohmann::json{{"uriTemplate", tmpl.uriTemplate}, {"name", tmpl.name}};
    if (tmpl.description) {
        json_obj["description"] = *tmpl.description;
    }
    if (tmpl.mimeType) {
        json_obj["mimeType"] = *tmpl.mimeType;
    }
    if (tmpl.meta) {
        json_obj["_meta"] = *tmpl.meta;
    }
    if (tmpl.annotations) {
        json_obj["annotations"] = *tmpl.annotations;
    }
    if (tmpl.title) {
        json_obj["title"] = *tmpl.title;
    }
    if (tmpl.icons) {
        json_obj["icons"] = *tmpl.icons;
    }
}

inline void from_json(const nlohmann::json& json_obj, ResourceTemplate& tmpl) {
    json_obj.at("uriTemplate").get_to(tmpl.uriTemplate);
    json_obj.at("name").get_to(tmpl.name);
    if (json_obj.contains("description")) {
        tmpl.description = json_obj.at("description").get<std::string>();
    }
    if (json_obj.contains("mimeType")) {
        tmpl.mimeType = json_obj.at("mimeType").get<std::string>();
    }
    if (json_obj.contains("_meta")) {
        tmpl.meta = json_obj.at("_meta").get<nlohmann::json>();
    }
    if (json_obj.contains("annotations")) {
        tmpl.annotations = json_obj.at("annotations").get<Annotations>();
    }
    if (json_obj.contains("title")) {
        tmpl.title = json_obj.at("title").get<std::string>();
    }
    if (json_obj.contains("icons")) {
        tmpl.icons = json_obj.at("icons").get<std::vector<Icon>>();
    }
}

/**
 * @brief Result of reading a resource.
 */
struct ReadResourceResult {
    std::vector<ResourceContents> contents;  ///< The contents of the resource.
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ReadResourceResult, contents)

// Completion Primitives

/**
 * @brief A reference to a resource or prompt for completion.
 */
struct CompleteReference {
    std::string type;                 ///< The type of reference ("ref/prompt" or "ref/resource").
    std::optional<std::string> name;  ///< The name of the prompt or resource.
    std::optional<std::string> uri;   ///< The URI of the resource.
};

/**
 * @brief Serializes CompleteReference to JSON.
 *
 * @param json_obj The JSON object to populate.
 * @param ref The CompleteReference object to serialize.
 */
inline void to_json(nlohmann::json& json_obj, const CompleteReference& ref) {
    json_obj = nlohmann::json{{"type", ref.type}};
    if (ref.name) {
        json_obj["name"] = *ref.name;
    }
    if (ref.uri) {
        json_obj["uri"] = *ref.uri;
    }
}

/**
 * @brief Deserializes CompleteReference from JSON.
 *
 * @param json_obj The JSON object to read from.
 * @param ref The CompleteReference object to populate.
 */
inline void from_json(const nlohmann::json& json_obj, CompleteReference& ref) {
    json_obj.at("type").get_to(ref.type);
    if (json_obj.contains("name")) {
        ref.name = json_obj.at("name").get<std::string>();
    }
    if (json_obj.contains("uri")) {
        ref.uri = json_obj.at("uri").get<std::string>();
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

// Prompts Primitives

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

// Sampling & Roots Primitives

/**
 * @brief Controls tool selection behavior for sampling requests.
 */
struct ToolChoice {
    std::optional<std::string> mode;  ///< "auto", "none", or "required".
};

/**
 * @brief Serializes ToolChoice to JSON.
 *
 * @param json_obj The JSON object to populate.
 * @param choice The ToolChoice object to serialize.
 */
inline void to_json(nlohmann::json& json_obj, const ToolChoice& choice) {
    json_obj = nlohmann::json::object();
    if (choice.mode) {
        json_obj["mode"] = *choice.mode;
    }
}

/**
 * @brief Deserializes ToolChoice from JSON.
 *
 * @param json_obj The JSON object to read from.
 * @param choice The ToolChoice object to populate.
 */
inline void from_json(const nlohmann::json& json_obj, ToolChoice& choice) {
    if (json_obj.contains("mode")) {
        choice.mode = json_obj.at("mode").get<std::string>();
    }
}

/**
 * @brief A request from the assistant to call a tool.
 */
struct ToolUseContent {
    std::string type = "tool_use";       ///< The type of content (always "tool_use").
    std::string id;                      ///< A unique identifier for this tool use.
    std::string name;                    ///< The name of the tool to call.
    nlohmann::json input;                ///< The arguments to pass to the tool.
    std::optional<nlohmann::json> meta;  ///< Optional metadata about the tool use.
};

/**
 * @brief Serializes ToolUseContent to JSON.
 *
 * @param json_obj The JSON object to populate.
 * @param content The ToolUseContent object to serialize.
 */
inline void to_json(nlohmann::json& json_obj, const ToolUseContent& content) {
    json_obj = nlohmann::json{
        {"type", content.type}, {"id", content.id}, {"name", content.name}, {"input", content.input}};
    if (content.meta) {
        json_obj["_meta"] = *content.meta;
    }
}

/**
 * @brief Deserializes ToolUseContent from JSON.
 *
 * @param json_obj The JSON object to read from.
 * @param content The ToolUseContent object to populate.
 */
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
 * @brief The result of a tool use, provided by the user back to the assistant.
 *
 * Uses ContentBlock (not SamplingMessageContentBlock) for its content array,
 * avoiding circular variant definitions.
 */
struct ToolResultContent {
    std::string type = "tool_result";   ///< The type of content (always "tool_result").
    std::string toolUseId;              ///< The ID of the tool use this result corresponds to.
    std::vector<ContentBlock> content;  ///< The unstructured result content.
    std::optional<bool> isError;        ///< Whether the tool use resulted in an error.
    std::optional<nlohmann::json> structuredContent;  ///< Optional structured result object.
    std::optional<nlohmann::json> meta;               ///< Optional metadata about the tool result.
};

/**
 * @brief Serializes ToolResultContent to JSON.
 *
 * @param json_obj The JSON object to populate.
 * @param content The ToolResultContent object to serialize.
 */
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

/**
 * @brief Deserializes ToolResultContent from JSON.
 *
 * @param json_obj The JSON object to read from.
 * @param content The ToolResultContent object to populate.
 */
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
 * @brief Content block variant for sampling messages.
 *
 * Extends ContentBlock with ToolUseContent and ToolResultContent.
 */
using SamplingMessageContentBlock =
    std::variant<TextContent, ImageContent, AudioContent, ToolUseContent, ToolResultContent>;

/**
 * @brief Serializes SamplingMessageContentBlock variant to JSON.
 *
 * @param json_obj The JSON object to populate.
 * @param block The SamplingMessageContentBlock variant to serialize.
 */
inline void to_json(nlohmann::json& json_obj, const SamplingMessageContentBlock& block) {
    std::visit([&json_obj](auto&& arg) { json_obj = arg; }, block);
}

/**
 * @brief Deserializes SamplingMessageContentBlock variant from JSON.
 *
 * @param json_obj The JSON object to read from.
 * @param block The SamplingMessageContentBlock variant to populate.
 * @throw std::invalid_argument If the content type is unknown.
 */
inline void from_json(const nlohmann::json& json_obj, SamplingMessageContentBlock& block) {
    std::string type;
    json_obj.at("type").get_to(type);
    if (type == "text") {
        block = json_obj.get<TextContent>();
    } else if (type == "image") {
        block = json_obj.get<ImageContent>();
    } else if (type == "audio") {
        block = json_obj.get<AudioContent>();
    } else if (type == "tool_use") {
        block = json_obj.get<ToolUseContent>();
    } else if (type == "tool_result") {
        block = json_obj.get<ToolResultContent>();
    } else {
        throw std::invalid_argument("Unknown sampling content type: " + type);
    }
}

/**
 * @brief Content for sampling messages — either a single block or an array of blocks.
 */
using SamplingMessageContent =
    std::variant<SamplingMessageContentBlock, std::vector<SamplingMessageContentBlock>>;

/**
 * @brief Serializes SamplingMessageContent variant to JSON.
 *
 * @param json_obj The JSON object to populate.
 * @param content The SamplingMessageContent variant to serialize.
 */
inline void to_json(nlohmann::json& json_obj, const SamplingMessageContent& content) {
    std::visit(
        [&json_obj](auto&& arg) {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, std::vector<SamplingMessageContentBlock>>) {
                json_obj = nlohmann::json::array();
                for (const auto& block : arg) {
                    nlohmann::json block_json;
                    to_json(block_json, block);
                    json_obj.push_back(std::move(block_json));
                }
            } else {
                to_json(json_obj, arg);
            }
        },
        content);
}

/**
 * @brief Deserializes SamplingMessageContent variant from JSON.
 *
 * @param json_obj The JSON object to read from.
 * @param content The SamplingMessageContent variant to populate.
 */
inline void from_json(const nlohmann::json& json_obj, SamplingMessageContent& content) {
    if (json_obj.is_array()) {
        content = json_obj.get<std::vector<SamplingMessageContentBlock>>();
    } else {
        content = json_obj.get<SamplingMessageContentBlock>();
    }
}

/**
 * @brief Describes a message issued to or received from an LLM API.
 */
struct SamplingMessage {
    Role role;                           ///< The sender or recipient of messages.
    SamplingMessageContent content;      ///< The content of the message.
    std::optional<nlohmann::json> meta;  ///< Reserved for protocol use.
};

/**
 * @brief Serializes SamplingMessage to JSON.
 *
 * @param json_obj The JSON object to populate.
 * @param msg The SamplingMessage object to serialize.
 */
inline void to_json(nlohmann::json& json_obj, const SamplingMessage& msg) {
    json_obj = nlohmann::json{{"role", msg.role}, {"content", msg.content}};
    if (msg.meta) {
        json_obj["_meta"] = *msg.meta;
    }
}

/**
 * @brief Deserializes SamplingMessage from JSON.
 *
 * @param json_obj The JSON object to read from.
 * @param msg The SamplingMessage object to populate.
 */
inline void from_json(const nlohmann::json& json_obj, SamplingMessage& msg) {
    json_obj.at("role").get_to(msg.role);
    json_obj.at("content").get_to(msg.content);
    if (json_obj.contains("_meta")) {
        msg.meta = json_obj.at("_meta").get<nlohmann::json>();
    }
}

/**
 * @brief Hints to use for model selection.
 */
struct ModelHint {
    std::optional<std::string> name;  ///< A hint for a model name (substring match).
};

/**
 * @brief Serializes ModelHint to JSON.
 *
 * @param json_obj The JSON object to populate.
 * @param hint The ModelHint object to serialize.
 */
inline void to_json(nlohmann::json& json_obj, const ModelHint& hint) {
    json_obj = nlohmann::json::object();
    if (hint.name) {
        json_obj["name"] = *hint.name;
    }
}

/**
 * @brief Deserializes ModelHint from JSON.
 *
 * @param json_obj The JSON object to read from.
 * @param hint The ModelHint object to populate.
 */
inline void from_json(const nlohmann::json& json_obj, ModelHint& hint) {
    if (json_obj.contains("name")) {
        hint.name = json_obj.at("name").get<std::string>();
    }
}

/**
 * @brief The server's preferences for model selection during sampling.
 */
struct ModelPreferences {
    std::optional<double> costPriority;           ///< Cost priority (0.0 to 1.0).
    std::optional<std::vector<ModelHint>> hints;  ///< Optional hints for model selection.
    std::optional<double> intelligencePriority;   ///< Intelligence priority (0.0 to 1.0).
    std::optional<double> speedPriority;          ///< Speed priority (0.0 to 1.0).
};

/**
 * @brief Serializes ModelPreferences to JSON.
 *
 * @param json_obj The JSON object to populate.
 * @param prefs The ModelPreferences object to serialize.
 */
inline void to_json(nlohmann::json& json_obj, const ModelPreferences& prefs) {
    json_obj = nlohmann::json::object();
    if (prefs.costPriority) {
        json_obj["costPriority"] = *prefs.costPriority;
    }
    if (prefs.hints) {
        json_obj["hints"] = *prefs.hints;
    }
    if (prefs.intelligencePriority) {
        json_obj["intelligencePriority"] = *prefs.intelligencePriority;
    }
    if (prefs.speedPriority) {
        json_obj["speedPriority"] = *prefs.speedPriority;
    }
}

/**
 * @brief Deserializes ModelPreferences from JSON.
 *
 * @param json_obj The JSON object to read from.
 * @param prefs The ModelPreferences object to populate.
 */
inline void from_json(const nlohmann::json& json_obj, ModelPreferences& prefs) {
    if (json_obj.contains("costPriority")) {
        prefs.costPriority = json_obj.at("costPriority").get<double>();
    }
    if (json_obj.contains("hints")) {
        prefs.hints = json_obj.at("hints").get<std::vector<ModelHint>>();
    }
    if (json_obj.contains("intelligencePriority")) {
        prefs.intelligencePriority = json_obj.at("intelligencePriority").get<double>();
    }
    if (json_obj.contains("speedPriority")) {
        prefs.speedPriority = json_obj.at("speedPriority").get<double>();
    }
}

/**
 * @brief Parameters for a sampling/createMessage request.
 */
struct CreateMessageRequestParams {
    std::vector<SamplingMessage> messages;                  ///< The messages to send to the LLM.
    int maxTokens;                                          ///< Maximum number of tokens to sample.
    std::optional<std::string> systemPrompt;                ///< Optional system prompt.
    std::optional<double> temperature;                      ///< Optional temperature.
    std::optional<std::vector<std::string>> stopSequences;  ///< Optional stop sequences.
    std::optional<ModelPreferences> modelPreferences;       ///< Server's model selection preferences.
    std::optional<std::string> includeContext;              ///< "none", "thisServer", or "allServers".
    std::optional<std::vector<Tool>> tools;                 ///< Tools available for model use.
    std::optional<ToolChoice> toolChoice;                   ///< Controls how the model uses tools.
    std::optional<nlohmann::json> metadata;                 ///< Optional provider-specific metadata.
    std::optional<nlohmann::json> meta;                     ///< Reserved for protocol use.
};

/**
 * @brief Serializes CreateMessageRequestParams to JSON.
 *
 * @param json_obj The JSON object to populate.
 * @param params The CreateMessageRequestParams object to serialize.
 */
inline void to_json(nlohmann::json& json_obj, const CreateMessageRequestParams& params) {
    json_obj = nlohmann::json{{"messages", params.messages}, {"maxTokens", params.maxTokens}};
    if (params.systemPrompt) {
        json_obj["systemPrompt"] = *params.systemPrompt;
    }
    if (params.temperature) {
        json_obj["temperature"] = *params.temperature;
    }
    if (params.stopSequences) {
        json_obj["stopSequences"] = *params.stopSequences;
    }
    if (params.modelPreferences) {
        json_obj["modelPreferences"] = *params.modelPreferences;
    }
    if (params.includeContext) {
        json_obj["includeContext"] = *params.includeContext;
    }
    if (params.tools) {
        json_obj["tools"] = *params.tools;
    }
    if (params.toolChoice) {
        json_obj["toolChoice"] = *params.toolChoice;
    }
    if (params.metadata) {
        json_obj["metadata"] = *params.metadata;
    }
    if (params.meta) {
        json_obj["_meta"] = *params.meta;
    }
}

/**
 * @brief Deserializes CreateMessageRequestParams from JSON.
 *
 * @param json_obj The JSON object to read from.
 * @param params The CreateMessageRequestParams object to populate.
 */
inline void from_json(const nlohmann::json& json_obj, CreateMessageRequestParams& params) {
    json_obj.at("messages").get_to(params.messages);
    json_obj.at("maxTokens").get_to(params.maxTokens);
    if (json_obj.contains("systemPrompt")) {
        params.systemPrompt = json_obj.at("systemPrompt").get<std::string>();
    }
    if (json_obj.contains("temperature")) {
        params.temperature = json_obj.at("temperature").get<double>();
    }
    if (json_obj.contains("stopSequences")) {
        params.stopSequences = json_obj.at("stopSequences").get<std::vector<std::string>>();
    }
    if (json_obj.contains("modelPreferences")) {
        params.modelPreferences = json_obj.at("modelPreferences").get<ModelPreferences>();
    }
    if (json_obj.contains("includeContext")) {
        params.includeContext = json_obj.at("includeContext").get<std::string>();
    }
    if (json_obj.contains("tools")) {
        params.tools = json_obj.at("tools").get<std::vector<Tool>>();
    }
    if (json_obj.contains("toolChoice")) {
        params.toolChoice = json_obj.at("toolChoice").get<ToolChoice>();
    }
    if (json_obj.contains("metadata")) {
        params.metadata = json_obj.at("metadata").get<nlohmann::json>();
    }
    if (json_obj.contains("_meta")) {
        params.meta = json_obj.at("_meta").get<nlohmann::json>();
    }
}

/**
 * @brief A request from the server to sample an LLM via the client.
 */
struct CreateMessageRequest {
    std::string method = "sampling/createMessage";  ///< The method name.
    CreateMessageRequestParams params;              ///< The request parameters.
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(CreateMessageRequest, method, params)

/**
 * @brief The result returned by the client for a sampling/createMessage request.
 */
struct CreateMessageResult {
    Role role;                              ///< The role of the message sender.
    SamplingMessageContent content;         ///< The content of the response.
    std::string model;                      ///< The name of the model that generated the message.
    std::optional<std::string> stopReason;  ///< The reason why sampling stopped.
    std::optional<nlohmann::json> meta;     ///< Reserved for protocol use.
};

/**
 * @brief Serializes CreateMessageResult to JSON.
 *
 * @param json_obj The JSON object to populate.
 * @param result The CreateMessageResult object to serialize.
 */
inline void to_json(nlohmann::json& json_obj, const CreateMessageResult& result) {
    json_obj =
        nlohmann::json{{"role", result.role}, {"content", result.content}, {"model", result.model}};
    if (result.stopReason) {
        json_obj["stopReason"] = *result.stopReason;
    }
    if (result.meta) {
        json_obj["_meta"] = *result.meta;
    }
}

/**
 * @brief Deserializes CreateMessageResult from JSON.
 *
 * @param json_obj The JSON object to read from.
 * @param result The CreateMessageResult object to populate.
 */
inline void from_json(const nlohmann::json& json_obj, CreateMessageResult& result) {
    json_obj.at("role").get_to(result.role);
    json_obj.at("content").get_to(result.content);
    json_obj.at("model").get_to(result.model);
    if (json_obj.contains("stopReason")) {
        result.stopReason = json_obj.at("stopReason").get<std::string>();
    }
    if (json_obj.contains("_meta")) {
        result.meta = json_obj.at("_meta").get<nlohmann::json>();
    }
}

/**
 * @brief Represents a root directory or file that the server can operate on.
 */
struct Root {
    std::string uri;                     ///< The URI identifying the root (must start with file://).
    std::optional<std::string> name;     ///< An optional human-readable name.
    std::optional<nlohmann::json> meta;  ///< Reserved for protocol use.
};

/**
 * @brief Serializes Root to JSON.
 *
 * @param json_obj The JSON object to populate.
 * @param root The Root object to serialize.
 */
inline void to_json(nlohmann::json& json_obj, const Root& root) {
    json_obj = nlohmann::json{{"uri", root.uri}};
    if (root.name) {
        json_obj["name"] = *root.name;
    }
    if (root.meta) {
        json_obj["_meta"] = *root.meta;
    }
}

/**
 * @brief Deserializes Root from JSON.
 *
 * @param json_obj The JSON object to read from.
 * @param root The Root object to populate.
 */
inline void from_json(const nlohmann::json& json_obj, Root& root) {
    json_obj.at("uri").get_to(root.uri);
    if (json_obj.contains("name")) {
        root.name = json_obj.at("name").get<std::string>();
    }
    if (json_obj.contains("_meta")) {
        root.meta = json_obj.at("_meta").get<nlohmann::json>();
    }
}

/**
 * @brief Sent from the server to request a list of root URIs from the client.
 */
struct ListRootsRequest {
    std::string method = "roots/list";  ///< The method name.
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ListRootsRequest, method)

/**
 * @brief The result returned by the client for a roots/list request.
 */
struct ListRootsResult {
    std::vector<Root> roots;             ///< The list of roots.
    std::optional<nlohmann::json> meta;  ///< Reserved for protocol use.
};

/**
 * @brief Serializes ListRootsResult to JSON.
 *
 * @param json_obj The JSON object to populate.
 * @param result The ListRootsResult object to serialize.
 */
inline void to_json(nlohmann::json& json_obj, const ListRootsResult& result) {
    json_obj = nlohmann::json{{"roots", result.roots}};
    if (result.meta) {
        json_obj["_meta"] = *result.meta;
    }
}

/**
 * @brief Deserializes ListRootsResult from JSON.
 *
 * @param json_obj The JSON object to read from.
 * @param result The ListRootsResult object to populate.
 */
inline void from_json(const nlohmann::json& json_obj, ListRootsResult& result) {
    json_obj.at("roots").get_to(result.roots);
    if (json_obj.contains("_meta")) {
        result.meta = json_obj.at("_meta").get<nlohmann::json>();
    }
}

}  // namespace mcp

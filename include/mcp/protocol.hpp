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
 * @brief Represents audio content.
 */
struct AudioContent {
    std::string type = "audio";  ///< The type of content (always "audio").
    std::string data;            ///< The base64-encoded audio data.
    std::string mimeType;        ///< The MIME type of the audio.
};

/**
 * @brief Variant holding either TextContent or ImageContent.
 */
using Content = std::variant<TextContent, ImageContent, AudioContent>;

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
inline void to_json(nlohmann::json& json_obj, const Content& content) {
    std::visit([&json_obj](auto&& arg) { json_obj = arg; }, content);
}

/**
 * @brief Deserializes Content variant from JSON.
 *
 * @param json_obj The JSON object to read from.
 * @param content The Content variant to populate.
 * @throw std::invalid_argument If the content type is unknown.
 */
inline void from_json(const nlohmann::json& json_obj, Content& content) {
    std::string type;
    json_obj.at("type").get_to(type);
    if (type == "text") {
        content = json_obj.get<TextContent>();
    } else if (type == "image") {
        content = json_obj.get<ImageContent>();

    } else if (type == "audio") {
        content = json_obj.get<AudioContent>();
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
    std::vector<Content> content;                     ///< The content produced by the tool.
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

}  // namespace mcp

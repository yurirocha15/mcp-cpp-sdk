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
 * @brief Protocol version 2025-11-25.
 */
constexpr std::string_view PROTOCOL_VERSION_2025_11_25 = "2025-11-25";

/**
 * @brief The latest supported protocol version.
 */
constexpr std::string_view LATEST_PROTOCOL_VERSION = PROTOCOL_VERSION_2025_11_25;

/**
 * @brief List of all supported protocol versions.
 */
constexpr std::array<std::string_view, 4> SUPPORTED_PROTOCOL_VERSIONS = {
    PROTOCOL_VERSION_2024_11_05, PROTOCOL_VERSION_2025_03_26, PROTOCOL_VERSION_2025_06_18,
    PROTOCOL_VERSION_2025_11_25};

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
    std::optional<std::string> description;  ///< Human-readable description of this implementation.
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
    if (impl.description) {
        json_obj["description"] = *impl.description;
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
    if (json_obj.contains("description")) {
        impl.description = json_obj.at("description").get<std::string>();
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
 * @brief Capabilities a client may support.
 *
 * @details Known capabilities are defined here, but this is not a closed set:
 * any client can define its own additional capabilities.
 */
struct ClientCapabilities {
    /**
     * @brief Client support for elicitation requests.
     */
    struct ElicitationCapability {
        std::optional<nlohmann::json> form;  ///< Form-based elicitation support.
        std::optional<nlohmann::json> url;   ///< URL-based elicitation support.
    };

    /**
     * @brief Client support for roots/list and roots change notifications.
     */
    struct RootsCapability {
        std::optional<bool>
            listChanged;  ///< Whether the client supports root list change notifications.
    };

    /**
     * @brief Client support for sampling/createMessage requests.
     */
    struct SamplingCapability {
        std::optional<nlohmann::json> context;  ///< Whether the client supports context inclusion.
        std::optional<nlohmann::json> tools;    ///< Whether the client supports tool use.
    };

    /**
     * @brief Client support for task-augmented reverse-RPC request types.
     */
    struct TaskRequestsCapability {
        /**
         * @brief Task support for elicitation request creation.
         */
        struct ElicitationTaskCapability {
            std::optional<nlohmann::json> create;  ///< Support for creating elicitation tasks.
        };
        /**
         * @brief Task support for sampling request creation.
         */
        struct SamplingTaskCapability {
            std::optional<nlohmann::json> createMessage;  ///< Support for creating sampling tasks.
        };

        std::optional<ElicitationTaskCapability>
            elicitation;                                 ///< Task support for elicitation requests.
        std::optional<SamplingTaskCapability> sampling;  ///< Task support for sampling requests.
    };

    /**
     * @brief Client support for task lifecycle endpoints.
     */
    struct TasksCapability {
        std::optional<nlohmann::json> cancel;            ///< Whether the client supports tasks/cancel.
        std::optional<nlohmann::json> list;              ///< Whether the client supports tasks/list.
        std::optional<TaskRequestsCapability> requests;  ///< Which request types support tasks.
    };

    std::optional<ElicitationCapability> elicitation;  ///< Support for elicitation requests.
    std::optional<nlohmann::json> experimental;  ///< Implementation-defined experimental capabilities.
    std::optional<RootsCapability> roots;        ///< Support for roots/list requests.
    std::optional<SamplingCapability> sampling;  ///< Support for sampling/createMessage requests.
    std::optional<TasksCapability> tasks;        ///< Support for task lifecycle endpoints.
};

inline void to_json(nlohmann::json& j, const ClientCapabilities::ElicitationCapability& cap) {
    j = nlohmann::json::object();
    if (cap.form) {
        j["form"] = *cap.form;
    }
    if (cap.url) {
        j["url"] = *cap.url;
    }
}

inline void from_json(const nlohmann::json& j, ClientCapabilities::ElicitationCapability& cap) {
    if (j.contains("form")) {
        cap.form = j.at("form");
    }
    if (j.contains("url")) {
        cap.url = j.at("url");
    }
}

inline void to_json(nlohmann::json& j, const ClientCapabilities::RootsCapability& cap) {
    j = nlohmann::json::object();
    if (cap.listChanged) {
        j["listChanged"] = *cap.listChanged;
    }
}

inline void from_json(const nlohmann::json& j, ClientCapabilities::RootsCapability& cap) {
    if (j.contains("listChanged")) {
        cap.listChanged = j.at("listChanged").get<bool>();
    }
}

inline void to_json(nlohmann::json& j, const ClientCapabilities::SamplingCapability& cap) {
    j = nlohmann::json::object();
    if (cap.context) {
        j["context"] = *cap.context;
    }
    if (cap.tools) {
        j["tools"] = *cap.tools;
    }
}

inline void from_json(const nlohmann::json& j, ClientCapabilities::SamplingCapability& cap) {
    if (j.contains("context")) {
        cap.context = j.at("context");
    }
    if (j.contains("tools")) {
        cap.tools = j.at("tools");
    }
}

inline void to_json(nlohmann::json& j,
                    const ClientCapabilities::TaskRequestsCapability::ElicitationTaskCapability& cap) {
    j = nlohmann::json::object();
    if (cap.create) {
        j["create"] = *cap.create;
    }
}

inline void from_json(const nlohmann::json& j,
                      ClientCapabilities::TaskRequestsCapability::ElicitationTaskCapability& cap) {
    if (j.contains("create")) {
        cap.create = j.at("create");
    }
}

inline void to_json(nlohmann::json& j,
                    const ClientCapabilities::TaskRequestsCapability::SamplingTaskCapability& cap) {
    j = nlohmann::json::object();
    if (cap.createMessage) {
        j["createMessage"] = *cap.createMessage;
    }
}

inline void from_json(const nlohmann::json& j,
                      ClientCapabilities::TaskRequestsCapability::SamplingTaskCapability& cap) {
    if (j.contains("createMessage")) {
        cap.createMessage = j.at("createMessage");
    }
}

inline void to_json(nlohmann::json& j, const ClientCapabilities::TaskRequestsCapability& cap) {
    j = nlohmann::json::object();
    if (cap.elicitation) {
        j["elicitation"] = *cap.elicitation;
    }
    if (cap.sampling) {
        j["sampling"] = *cap.sampling;
    }
}

inline void from_json(const nlohmann::json& j, ClientCapabilities::TaskRequestsCapability& cap) {
    if (j.contains("elicitation")) {
        cap.elicitation =
            j.at("elicitation")
                .get<ClientCapabilities::TaskRequestsCapability::ElicitationTaskCapability>();
    }
    if (j.contains("sampling")) {
        cap.sampling =
            j.at("sampling").get<ClientCapabilities::TaskRequestsCapability::SamplingTaskCapability>();
    }
}

inline void to_json(nlohmann::json& j, const ClientCapabilities::TasksCapability& cap) {
    j = nlohmann::json::object();
    if (cap.cancel) {
        j["cancel"] = *cap.cancel;
    }
    if (cap.list) {
        j["list"] = *cap.list;
    }
    if (cap.requests) {
        j["requests"] = *cap.requests;
    }
}

inline void from_json(const nlohmann::json& j, ClientCapabilities::TasksCapability& cap) {
    if (j.contains("cancel")) {
        cap.cancel = j.at("cancel");
    }
    if (j.contains("list")) {
        cap.list = j.at("list");
    }
    if (j.contains("requests")) {
        cap.requests = j.at("requests").get<ClientCapabilities::TaskRequestsCapability>();
    }
}

inline void to_json(nlohmann::json& j, const ClientCapabilities& cap) {
    j = nlohmann::json::object();
    if (cap.elicitation) {
        j["elicitation"] = *cap.elicitation;
    }
    if (cap.experimental) {
        j["experimental"] = *cap.experimental;
    }
    if (cap.roots) {
        j["roots"] = *cap.roots;
    }
    if (cap.sampling) {
        j["sampling"] = *cap.sampling;
    }
    if (cap.tasks) {
        j["tasks"] = *cap.tasks;
    }
}

inline void from_json(const nlohmann::json& j, ClientCapabilities& cap) {
    if (j.contains("elicitation")) {
        cap.elicitation = j.at("elicitation").get<ClientCapabilities::ElicitationCapability>();
    }
    if (j.contains("experimental")) {
        cap.experimental = j.at("experimental");
    }
    if (j.contains("roots")) {
        cap.roots = j.at("roots").get<ClientCapabilities::RootsCapability>();
    }
    if (j.contains("sampling")) {
        cap.sampling = j.at("sampling").get<ClientCapabilities::SamplingCapability>();
    }
    if (j.contains("tasks")) {
        cap.tasks = j.at("tasks").get<ClientCapabilities::TasksCapability>();
    }
}

/**
 * @brief Capabilities that a server may support.
 *
 * @details Known capabilities are defined here, but this is not a closed set:
 * any server can define its own additional capabilities.
 */
struct ServerCapabilities {
    /**
     * @brief Server support for prompt listing change notifications.
     */
    struct PromptsCapability {
        std::optional<bool>
            listChanged;  ///< Whether the server supports prompt list change notifications.
    };

    /**
     * @brief Server support for resource listing and subscription operations.
     */
    struct ResourcesCapability {
        std::optional<bool>
            listChanged;  ///< Whether the server supports resource list change notifications.
        std::optional<bool>
            subscribe;  ///< Whether the server supports subscribing to resource updates.
    };

    /**
     * @brief Server support for task-augmented request types.
     */
    struct TaskRequestsCapability {
        /**
         * @brief Task support for tool invocation requests.
         */
        struct ToolsTaskCapability {
            std::optional<nlohmann::json> call;  ///< Support for task-augmented tool calls.
        };

        std::optional<ToolsTaskCapability> tools;  ///< Task support for tool-related requests.
    };

    /**
     * @brief Server support for task lifecycle endpoints.
     */
    struct TasksCapability {
        std::optional<nlohmann::json> cancel;            ///< Whether the server supports tasks/cancel.
        std::optional<nlohmann::json> list;              ///< Whether the server supports tasks/list.
        std::optional<TaskRequestsCapability> requests;  ///< Which request types support tasks.
    };

    /**
     * @brief Server support for tool listing change notifications.
     */
    struct ToolsCapability {
        std::optional<bool>
            listChanged;  ///< Whether the server supports tool list change notifications.
    };

    std::optional<nlohmann::json> completions;   ///< Support for completion/complete requests.
    std::optional<nlohmann::json> experimental;  ///< Implementation-defined experimental capabilities.
    std::optional<nlohmann::json> logging;       ///< Support for logging/setLevel.
    std::optional<PromptsCapability> prompts;    ///< Support for prompt endpoints.
    std::optional<ResourcesCapability> resources;  ///< Support for resource endpoints.
    std::optional<TasksCapability> tasks;          ///< Support for task lifecycle endpoints.
    std::optional<ToolsCapability> tools;          ///< Support for tool endpoints.
};

inline void to_json(nlohmann::json& j, const ServerCapabilities::PromptsCapability& cap) {
    j = nlohmann::json::object();
    if (cap.listChanged) {
        j["listChanged"] = *cap.listChanged;
    }
}

inline void from_json(const nlohmann::json& j, ServerCapabilities::PromptsCapability& cap) {
    if (j.contains("listChanged")) {
        cap.listChanged = j.at("listChanged").get<bool>();
    }
}

inline void to_json(nlohmann::json& j, const ServerCapabilities::ResourcesCapability& cap) {
    j = nlohmann::json::object();
    if (cap.listChanged) {
        j["listChanged"] = *cap.listChanged;
    }
    if (cap.subscribe) {
        j["subscribe"] = *cap.subscribe;
    }
}

inline void from_json(const nlohmann::json& j, ServerCapabilities::ResourcesCapability& cap) {
    if (j.contains("listChanged")) {
        cap.listChanged = j.at("listChanged").get<bool>();
    }
    if (j.contains("subscribe")) {
        cap.subscribe = j.at("subscribe").get<bool>();
    }
}

inline void to_json(nlohmann::json& j,
                    const ServerCapabilities::TaskRequestsCapability::ToolsTaskCapability& cap) {
    j = nlohmann::json::object();
    if (cap.call) {
        j["call"] = *cap.call;
    }
}

inline void from_json(const nlohmann::json& j,
                      ServerCapabilities::TaskRequestsCapability::ToolsTaskCapability& cap) {
    if (j.contains("call")) {
        cap.call = j.at("call");
    }
}

inline void to_json(nlohmann::json& j, const ServerCapabilities::TaskRequestsCapability& cap) {
    j = nlohmann::json::object();
    if (cap.tools) {
        j["tools"] = *cap.tools;
    }
}

inline void from_json(const nlohmann::json& j, ServerCapabilities::TaskRequestsCapability& cap) {
    if (j.contains("tools")) {
        cap.tools =
            j.at("tools").get<ServerCapabilities::TaskRequestsCapability::ToolsTaskCapability>();
    }
}

inline void to_json(nlohmann::json& j, const ServerCapabilities::TasksCapability& cap) {
    j = nlohmann::json::object();
    if (cap.cancel) {
        j["cancel"] = *cap.cancel;
    }
    if (cap.list) {
        j["list"] = *cap.list;
    }
    if (cap.requests) {
        j["requests"] = *cap.requests;
    }
}

inline void from_json(const nlohmann::json& j, ServerCapabilities::TasksCapability& cap) {
    if (j.contains("cancel")) {
        cap.cancel = j.at("cancel");
    }
    if (j.contains("list")) {
        cap.list = j.at("list");
    }
    if (j.contains("requests")) {
        cap.requests = j.at("requests").get<ServerCapabilities::TaskRequestsCapability>();
    }
}

inline void to_json(nlohmann::json& j, const ServerCapabilities::ToolsCapability& cap) {
    j = nlohmann::json::object();
    if (cap.listChanged) {
        j["listChanged"] = *cap.listChanged;
    }
}

inline void from_json(const nlohmann::json& j, ServerCapabilities::ToolsCapability& cap) {
    if (j.contains("listChanged")) {
        cap.listChanged = j.at("listChanged").get<bool>();
    }
}

inline void to_json(nlohmann::json& j, const ServerCapabilities& cap) {
    j = nlohmann::json::object();
    if (cap.completions) {
        j["completions"] = *cap.completions;
    }
    if (cap.experimental) {
        j["experimental"] = *cap.experimental;
    }
    if (cap.logging) {
        j["logging"] = *cap.logging;
    }
    if (cap.prompts) {
        j["prompts"] = *cap.prompts;
    }
    if (cap.resources) {
        j["resources"] = *cap.resources;
    }
    if (cap.tasks) {
        j["tasks"] = *cap.tasks;
    }
    if (cap.tools) {
        j["tools"] = *cap.tools;
    }
}

inline void from_json(const nlohmann::json& j, ServerCapabilities& cap) {
    if (j.contains("completions")) {
        cap.completions = j.at("completions");
    }
    if (j.contains("experimental")) {
        cap.experimental = j.at("experimental");
    }
    if (j.contains("logging")) {
        cap.logging = j.at("logging");
    }
    if (j.contains("prompts")) {
        cap.prompts = j.at("prompts").get<ServerCapabilities::PromptsCapability>();
    }
    if (j.contains("resources")) {
        cap.resources = j.at("resources").get<ServerCapabilities::ResourcesCapability>();
    }
    if (j.contains("tasks")) {
        cap.tasks = j.at("tasks").get<ServerCapabilities::TasksCapability>();
    }
    if (j.contains("tools")) {
        cap.tools = j.at("tools").get<ServerCapabilities::ToolsCapability>();
    }
}

/**
 * @brief Represents an initialization request from the client.
 */
struct InitializeRequest {
    std::string protocolVersion;      ///< The protocol version supported by the client.
    ClientInfo clientInfo;            ///< Information about the client implementation.
    ClientCapabilities capabilities;  ///< The capabilities supported by the client.
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(InitializeRequest, protocolVersion, clientInfo, capabilities)

/**
 * @brief Represents the result of an initialization request.
 */
struct InitializeResult {
    std::string protocolVersion;              ///< The protocol version selected by the server.
    ServerCapabilities capabilities;          ///< The capabilities supported by the server.
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
 * @brief Variant of all supported message content block types.
 */
using ContentBlock = std::variant<TextContent, ImageContent, AudioContent, ResourceLink,
                                  EmbeddedResource, ToolUseContent, ToolResultContent>;

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
    } else if (type == "tool_use") {
        content = json_obj.get<ToolUseContent>();
    } else if (type == "tool_result") {
        content = json_obj.get<ToolResultContent>();
    } else {
        throw std::invalid_argument("Unknown content type: " + type);
    }
}

// Tool Primitives

// MCP Protocol Constants

/**
 * @brief JSON-RPC error code for parse errors.
 */
inline constexpr int PARSE_ERROR = -32700;

/**
 * @brief JSON-RPC error code for invalid requests.
 */
inline constexpr int INVALID_REQUEST = -32600;

/**
 * @brief JSON-RPC error code for method not found.
 */
inline constexpr int METHOD_NOT_FOUND = -32601;

/**
 * @brief JSON-RPC error code for invalid parameters.
 */
inline constexpr int INVALID_PARAMS = -32602;

/**
 * @brief JSON-RPC error code for internal errors.
 */
inline constexpr int INTERNAL_ERROR = -32603;

/**
 * @brief Error code indicating URL elicitation is required.
 */
inline constexpr int URL_ELICITATION_REQUIRED_ERROR = -32042;

/**
 * @brief Describes the execution support of a tool.
 */
struct ToolExecution {
    std::string taskSupport;  ///< "forbidden" | "optional" | "required"
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ToolExecution, taskSupport)

/**
 * @brief Metadata about a related task.
 */
struct RelatedTaskMetadata {
    std::string id;                    ///< The ID of the related task.
    std::optional<std::string> title;  ///< An optional title.
};

inline void to_json(nlohmann::json& j, const RelatedTaskMetadata& t) {
    j = nlohmann::json{{"id", t.id}};
    if (t.title) {
        j["title"] = *t.title;
    }
}
inline void from_json(const nlohmann::json& j, RelatedTaskMetadata& t) {
    j.at("id").get_to(t.id);
    if (j.contains("title")) {
        t.title = j.at("title").get<std::string>();
    }
}

/**
 * @brief Definition of an elicitation form schema.
 */
struct PrimitiveSchemaDefinition {
    std::string type;                        ///< JSON Schema primitive type name.
    std::optional<std::string> title;        ///< Optional human-readable field title.
    std::optional<std::string> description;  ///< Optional human-readable field description.
};

inline void to_json(nlohmann::json& j, const PrimitiveSchemaDefinition& def) {
    j = nlohmann::json{{"type", def.type}};
    if (def.title) {
        j["title"] = *def.title;
    }
    if (def.description) {
        j["description"] = *def.description;
    }
}
inline void from_json(const nlohmann::json& j, PrimitiveSchemaDefinition& def) {
    j.at("type").get_to(def.type);
    if (j.contains("title")) {
        def.title = j.at("title").get<std::string>();
    }
    if (j.contains("description")) {
        def.description = j.at("description").get<std::string>();
    }
}

// We can just use json directly for more complex schema definitions,
// but the spec asks for schema additions. Let's provide basic types.
/// @brief Schema alias for string-valued form fields.
using StringSchema = PrimitiveSchemaDefinition;
/// @brief Schema alias for numeric form fields.
using NumberSchema = PrimitiveSchemaDefinition;
/// @brief Schema alias for boolean form fields.
using BooleanSchema = PrimitiveSchemaDefinition;
// Since they can be complex JSON Schema objects, we will map ElicitRequestFormParams::schema to json
// per the plan "can be nlohmann::json initially". The plan explicitly lists:
// PrimitiveSchemaDefinition struct (type: string, title: optional<string>, description:
// optional<string>) but for ElicitRequestFormParams it says "use schema types instead of raw json for
// schema field"

// Let's create an EnumSchema
/**
 * @brief Primitive schema with an explicit set of allowed string values.
 */
struct EnumSchema : public PrimitiveSchemaDefinition {
    std::vector<std::string> enumValues;  ///< Allowed values for the field.
};
inline void to_json(nlohmann::json& j, const EnumSchema& e) {
    to_json(j, static_cast<const PrimitiveSchemaDefinition&>(e));
    j["enum"] = e.enumValues;
}
inline void from_json(const nlohmann::json& j, EnumSchema& e) {
    from_json(j, static_cast<PrimitiveSchemaDefinition&>(e));
    if (j.contains("enum")) {
        j.at("enum").get_to(e.enumValues);
    }
}

/**
 * @brief Requests and Responses
 */

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
 * @brief A reference to a prompt.
 */
struct PromptReference {
    std::string type = "ref/prompt";  ///< The type of reference.
    std::string name;                 ///< The name of the prompt.
};

inline void to_json(nlohmann::json& json_obj, const PromptReference& ref) {
    json_obj = nlohmann::json{{"type", ref.type}, {"name", ref.name}};
}
inline void from_json(const nlohmann::json& json_obj, PromptReference& ref) {
    json_obj.at("type").get_to(ref.type);
    json_obj.at("name").get_to(ref.name);
}

/**
 * @brief A reference to a resource template.
 */
struct ResourceTemplateReference {
    std::string type = "ref/resource";  ///< The type of reference.
    std::string uri;                    ///< The URI of the resource.
};

inline void to_json(nlohmann::json& json_obj, const ResourceTemplateReference& ref) {
    json_obj = nlohmann::json{{"type", ref.type}, {"uri", ref.uri}};
}
inline void from_json(const nlohmann::json& json_obj, ResourceTemplateReference& ref) {
    json_obj.at("type").get_to(ref.type);
    json_obj.at("uri").get_to(ref.uri);
}

/**
 * @brief A reference to a resource or prompt for completion.
 */
using CompleteReference = std::variant<PromptReference, ResourceTemplateReference>;

inline void to_json(nlohmann::json& json_obj, const CompleteReference& ref) {
    std::visit([&json_obj](auto&& arg) { to_json(json_obj, arg); }, ref);
}

inline void from_json(const nlohmann::json& json_obj, CompleteReference& ref) {
    if (json_obj.at("type") == "ref/prompt") {
        ref = json_obj.get<PromptReference>();
    } else {
        ref = json_obj.get<ResourceTemplateReference>();
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

using SamplingMessageContentBlock = ContentBlock;

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

// Task Primitives

/**
 * @brief The status of a task.
 */
enum class TaskStatus : std::uint8_t {
    Cancelled,      ///< Task has been cancelled.
    Completed,      ///< Task has completed successfully.
    Failed,         ///< Task has failed.
    InputRequired,  ///< Task requires additional input.
    Working         ///< Task is currently in progress.
};

NLOHMANN_JSON_SERIALIZE_ENUM(TaskStatus, {{TaskStatus::Cancelled, "cancelled"},
                                          {TaskStatus::Completed, "completed"},
                                          {TaskStatus::Failed, "failed"},
                                          {TaskStatus::InputRequired, "input_required"},
                                          {TaskStatus::Working, "working"}})

/**
 * @brief Metadata for augmenting a request with task execution.
 */
struct TaskMetadata {
    std::optional<int64_t> ttl;  ///< Requested retention duration from creation in milliseconds.
    std::optional<std::vector<RelatedTaskMetadata>> relatedTasks;  ///< Related tasks.
};

inline void to_json(nlohmann::json& json_obj, const TaskMetadata& meta) {
    json_obj = nlohmann::json::object();
    if (meta.ttl) {
        json_obj["ttl"] = *meta.ttl;
    }
    if (meta.relatedTasks) {
        json_obj["relatedTasks"] = *meta.relatedTasks;
    }
}

inline void from_json(const nlohmann::json& json_obj, TaskMetadata& meta) {
    if (json_obj.contains("ttl")) {
        meta.ttl = json_obj.at("ttl").get<int64_t>();
    }
    if (json_obj.contains("relatedTasks")) {
        meta.relatedTasks = json_obj.at("relatedTasks").get<std::vector<RelatedTaskMetadata>>();
    }
}

/**
 * @brief Data associated with a task.
 *
 * Named TaskData to avoid conflict with mcp::Task (boost::asio::awaitable) in core.hpp.
 */
struct TaskData {
    std::string taskId;                        ///< The task identifier.
    TaskStatus status;                         ///< Current task state.
    std::string createdAt;                     ///< ISO 8601 timestamp when the task was created.
    std::string lastUpdatedAt;                 ///< ISO 8601 timestamp when the task was last updated.
    int64_t ttl;                               ///< Retention duration from creation in milliseconds.
    std::optional<int64_t> pollInterval;       ///< Suggested polling interval in milliseconds.
    std::optional<std::string> statusMessage;  ///< Human-readable message describing current state.
};

inline void to_json(nlohmann::json& json_obj, const TaskData& task) {
    json_obj = nlohmann::json{{"taskId", task.taskId},
                              {"status", task.status},
                              {"createdAt", task.createdAt},
                              {"lastUpdatedAt", task.lastUpdatedAt},
                              {"ttl", task.ttl}};
    if (task.pollInterval) {
        json_obj["pollInterval"] = *task.pollInterval;
    }
    if (task.statusMessage) {
        json_obj["statusMessage"] = *task.statusMessage;
    }
}

inline void from_json(const nlohmann::json& json_obj, TaskData& task) {
    json_obj.at("taskId").get_to(task.taskId);
    json_obj.at("status").get_to(task.status);
    json_obj.at("createdAt").get_to(task.createdAt);
    json_obj.at("lastUpdatedAt").get_to(task.lastUpdatedAt);
    json_obj.at("ttl").get_to(task.ttl);
    if (json_obj.contains("pollInterval")) {
        task.pollInterval = json_obj.at("pollInterval").get<int64_t>();
    }
    if (json_obj.contains("statusMessage")) {
        task.statusMessage = json_obj.at("statusMessage").get<std::string>();
    }
}

/**
 * @brief The result returned for a task-augmented request.
 */
struct CreateTaskResult {
    TaskData task;                       ///< The task data.
    std::optional<nlohmann::json> meta;  ///< Reserved for protocol use.
};

inline void to_json(nlohmann::json& json_obj, const CreateTaskResult& result) {
    json_obj = nlohmann::json{{"task", result.task}};
    if (result.meta) {
        json_obj["_meta"] = *result.meta;
    }
}

inline void from_json(const nlohmann::json& json_obj, CreateTaskResult& result) {
    json_obj.at("task").get_to(result.task);
    if (json_obj.contains("_meta")) {
        result.meta = json_obj.at("_meta").get<nlohmann::json>();
    }
}

/**
 * @brief The result returned for a tasks/get request.
 *
 * Merges Result (_meta) and Task fields per schema allOf.
 */
struct GetTaskResult {
    std::string taskId;                        ///< The task identifier.
    TaskStatus status;                         ///< Current task state.
    std::string createdAt;                     ///< ISO 8601 timestamp when the task was created.
    std::string lastUpdatedAt;                 ///< ISO 8601 timestamp when the task was last updated.
    int64_t ttl;                               ///< Retention duration from creation in milliseconds.
    std::optional<int64_t> pollInterval;       ///< Suggested polling interval in milliseconds.
    std::optional<std::string> statusMessage;  ///< Human-readable message describing current state.
    std::optional<nlohmann::json> meta;        ///< Reserved for protocol use.
};

inline void to_json(nlohmann::json& json_obj, const GetTaskResult& result) {
    json_obj = nlohmann::json{{"taskId", result.taskId},
                              {"status", result.status},
                              {"createdAt", result.createdAt},
                              {"lastUpdatedAt", result.lastUpdatedAt},
                              {"ttl", result.ttl}};
    if (result.pollInterval) {
        json_obj["pollInterval"] = *result.pollInterval;
    }
    if (result.statusMessage) {
        json_obj["statusMessage"] = *result.statusMessage;
    }
    if (result.meta) {
        json_obj["_meta"] = *result.meta;
    }
}

inline void from_json(const nlohmann::json& json_obj, GetTaskResult& result) {
    json_obj.at("taskId").get_to(result.taskId);
    json_obj.at("status").get_to(result.status);
    json_obj.at("createdAt").get_to(result.createdAt);
    json_obj.at("lastUpdatedAt").get_to(result.lastUpdatedAt);
    json_obj.at("ttl").get_to(result.ttl);
    if (json_obj.contains("pollInterval")) {
        result.pollInterval = json_obj.at("pollInterval").get<int64_t>();
    }
    if (json_obj.contains("statusMessage")) {
        result.statusMessage = json_obj.at("statusMessage").get<std::string>();
    }
    if (json_obj.contains("_meta")) {
        result.meta = json_obj.at("_meta").get<nlohmann::json>();
    }
}

/**
 * @brief The result returned for a tasks/list request.
 */
struct ListTasksResult {
    std::vector<TaskData> tasks;            ///< The list of tasks.
    std::optional<std::string> nextCursor;  ///< Pagination position after last returned result.
    std::optional<nlohmann::json> meta;     ///< Reserved for protocol use.
};

inline void to_json(nlohmann::json& json_obj, const ListTasksResult& result) {
    json_obj = nlohmann::json{{"tasks", result.tasks}};
    if (result.nextCursor) {
        json_obj["nextCursor"] = *result.nextCursor;
    }
    if (result.meta) {
        json_obj["_meta"] = *result.meta;
    }
}

inline void from_json(const nlohmann::json& json_obj, ListTasksResult& result) {
    json_obj.at("tasks").get_to(result.tasks);
    if (json_obj.contains("nextCursor")) {
        result.nextCursor = json_obj.at("nextCursor").get<std::string>();
    }
    if (json_obj.contains("_meta")) {
        result.meta = json_obj.at("_meta").get<nlohmann::json>();
    }
}

// Elicitation Primitives

/**
 * @brief Parameters for a URL-mode elicitation request.
 */
struct ElicitRequestURLParams {
    std::string mode = "url";            ///< The elicitation mode (always "url").
    std::string message;                 ///< The message to present to the user.
    std::string url;                     ///< The URL that the user should navigate to.
    std::string elicitationId;           ///< Unique ID for this elicitation.
    std::optional<TaskMetadata> task;    ///< Optional task augmentation metadata.
    std::optional<nlohmann::json> meta;  ///< Reserved for protocol use.
};

inline void to_json(nlohmann::json& json_obj, const ElicitRequestURLParams& params) {
    json_obj = nlohmann::json{{"mode", params.mode},
                              {"message", params.message},
                              {"url", params.url},
                              {"elicitationId", params.elicitationId}};
    if (params.task) {
        json_obj["task"] = *params.task;
    }
    if (params.meta) {
        json_obj["_meta"] = *params.meta;
    }
}

inline void from_json(const nlohmann::json& json_obj, ElicitRequestURLParams& params) {
    json_obj.at("mode").get_to(params.mode);
    json_obj.at("message").get_to(params.message);
    json_obj.at("url").get_to(params.url);
    json_obj.at("elicitationId").get_to(params.elicitationId);
    if (json_obj.contains("task")) {
        params.task = json_obj.at("task").get<TaskMetadata>();
    }
    if (json_obj.contains("_meta")) {
        params.meta = json_obj.at("_meta").get<nlohmann::json>();
    }
}

/**
 * @brief Parameters for a form-mode elicitation request.
 */
struct ElicitRequestFormParams {
    std::string mode = "form";           ///< The elicitation mode (always "form").
    std::string message;                 ///< The message describing what info is requested.
    nlohmann::json requestedSchema;      ///< Restricted JSON Schema for the form.
    std::optional<TaskMetadata> task;    ///< Optional task augmentation metadata.
    std::optional<nlohmann::json> meta;  ///< Reserved for protocol use.
};

inline void to_json(nlohmann::json& json_obj, const ElicitRequestFormParams& params) {
    json_obj = nlohmann::json{{"mode", params.mode},
                              {"message", params.message},
                              {"requestedSchema", params.requestedSchema}};
    if (params.task) {
        json_obj["task"] = *params.task;
    }
    if (params.meta) {
        json_obj["_meta"] = *params.meta;
    }
}

inline void from_json(const nlohmann::json& json_obj, ElicitRequestFormParams& params) {
    json_obj.at("mode").get_to(params.mode);
    json_obj.at("message").get_to(params.message);
    json_obj.at("requestedSchema").get_to(params.requestedSchema);
    if (json_obj.contains("task")) {
        params.task = json_obj.at("task").get<TaskMetadata>();
    }
    if (json_obj.contains("_meta")) {
        params.meta = json_obj.at("_meta").get<nlohmann::json>();
    }
}

/**
 * @brief Parameters for an elicitation request — either URL or form mode.
 */
using ElicitRequestParams = std::variant<ElicitRequestURLParams, ElicitRequestFormParams>;

inline void to_json(nlohmann::json& json_obj, const ElicitRequestParams& params) {
    std::visit([&json_obj](auto&& arg) { json_obj = arg; }, params);
}

inline void from_json(const nlohmann::json& json_obj, ElicitRequestParams& params) {
    std::string mode;
    json_obj.at("mode").get_to(mode);
    if (mode == "url") {
        params = json_obj.get<ElicitRequestURLParams>();
    } else if (mode == "form") {
        params = json_obj.get<ElicitRequestFormParams>();
    } else {
        throw std::invalid_argument("Unknown elicitation mode: " + mode);
    }
}

/**
 * @brief A request from the server to elicit additional information from the user.
 */
struct ElicitRequest {
    std::string method = "elicitation/create";  ///< The method name.
    ElicitRequestParams params;                 ///< The request parameters.
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ElicitRequest, method, params)

/**
 * @brief The user action in response to an elicitation.
 */
enum class ElicitAction : std::uint8_t {
    Accept,   ///< User submitted the form/confirmed the action.
    Decline,  ///< User explicitly declined the action.
    Cancel    ///< User dismissed without making an explicit choice.
};

NLOHMANN_JSON_SERIALIZE_ENUM(ElicitAction, {{ElicitAction::Accept, "accept"},
                                            {ElicitAction::Decline, "decline"},
                                            {ElicitAction::Cancel, "cancel"}})

/**
 * @brief The result returned by the client for an elicitation/create request.
 */
struct ElicitResult {
    ElicitAction action;                    ///< The user action.
    std::optional<nlohmann::json> content;  ///< Submitted form data (only when action is accept).
    std::optional<nlohmann::json> meta;     ///< Reserved for protocol use.
};

inline void to_json(nlohmann::json& json_obj, const ElicitResult& result) {
    json_obj = nlohmann::json{{"action", result.action}};
    if (result.content) {
        json_obj["content"] = *result.content;
    }
    if (result.meta) {
        json_obj["_meta"] = *result.meta;
    }
}

inline void from_json(const nlohmann::json& json_obj, ElicitResult& result) {
    json_obj.at("action").get_to(result.action);
    if (json_obj.contains("content")) {
        result.content = json_obj.at("content").get<nlohmann::json>();
    }
    if (json_obj.contains("_meta")) {
        result.meta = json_obj.at("_meta").get<nlohmann::json>();
    }
}

// JSON-RPC Base Types

/// @brief JSON-RPC request identifier represented as either a string or integer.
using RequestId = std::variant<std::string, int64_t>;
/// @brief Token used to correlate progress updates with a request or task.
using ProgressToken = RequestId;

}  // namespace mcp

#ifndef DOXYGEN_SHOULD_SKIP_THIS
template <>
struct nlohmann::adl_serializer<mcp::RequestId> {
    static void to_json(nlohmann::json& json_obj, const mcp::RequestId& id) {
        std::visit([&json_obj](auto&& val) { json_obj = val; }, id);
    }

    static void from_json(const nlohmann::json& json_obj, mcp::RequestId& id) {
        if (json_obj.is_string()) {
            id = json_obj.get<std::string>();
        } else if (json_obj.is_number_integer()) {
            id = json_obj.get<int64_t>();
        } else {
            throw std::invalid_argument("RequestId must be a string or integer");
        }
    }
};
#endif

namespace mcp {

/**
 * @brief Standard JSON-RPC error payload.
 */
struct Error {
    int code;                            ///< The error type that occurred.
    std::string message;                 ///< A short description of the error.
    std::optional<nlohmann::json> data;  ///< Additional information about the error.
};

inline void to_json(nlohmann::json& json_obj, const Error& error) {
    json_obj = nlohmann::json{{"code", error.code}, {"message", error.message}};
    if (error.data) {
        json_obj["data"] = *error.data;
    }
}

inline void from_json(const nlohmann::json& json_obj, Error& error) {
    json_obj.at("code").get_to(error.code);
    json_obj.at("message").get_to(error.message);
    if (json_obj.contains("data")) {
        error.data = json_obj.at("data");
    }
}

/**
 * @brief A JSON-RPC request message.
 */
struct JSONRPCRequest {
    RequestId id;                          ///< Unique request identifier.
    std::string jsonrpc = "2.0";           ///< JSON-RPC version (always "2.0").
    std::string method;                    ///< The method to invoke.
    std::optional<nlohmann::json> params;  ///< Optional parameters object.
};

inline void to_json(nlohmann::json& json_obj, const JSONRPCRequest& req) {
    json_obj = nlohmann::json::object();
    json_obj["id"] = req.id;
    json_obj["jsonrpc"] = req.jsonrpc;
    json_obj["method"] = req.method;
    if (req.params) {
        json_obj["params"] = *req.params;
    }
}

inline void from_json(const nlohmann::json& json_obj, JSONRPCRequest& req) {
    req.id = json_obj.at("id").get<RequestId>();
    json_obj.at("jsonrpc").get_to(req.jsonrpc);
    json_obj.at("method").get_to(req.method);
    if (json_obj.contains("params")) {
        req.params = json_obj.at("params");
    }
}

/**
 * @brief A JSON-RPC notification message.
 */
struct JSONRPCNotification {
    std::string jsonrpc = "2.0";           ///< JSON-RPC version (always "2.0").
    std::string method;                    ///< The notification method.
    std::optional<nlohmann::json> params;  ///< Optional parameters object.
};

inline void to_json(nlohmann::json& json_obj, const JSONRPCNotification& notif) {
    json_obj = nlohmann::json{{"jsonrpc", notif.jsonrpc}, {"method", notif.method}};
    if (notif.params) {
        json_obj["params"] = *notif.params;
    }
}

inline void from_json(const nlohmann::json& json_obj, JSONRPCNotification& notif) {
    json_obj.at("jsonrpc").get_to(notif.jsonrpc);
    json_obj.at("method").get_to(notif.method);
    if (json_obj.contains("params")) {
        notif.params = json_obj.at("params");
    }
}

/**
 * @brief A successful JSON-RPC response message.
 */
struct JSONRPCResultResponse {
    RequestId id;                 ///< The request ID this responds to.
    std::string jsonrpc = "2.0";  ///< JSON-RPC version (always "2.0").
    nlohmann::json result;        ///< The result payload.
};

inline void to_json(nlohmann::json& json_obj, const JSONRPCResultResponse& resp) {
    json_obj = nlohmann::json::object();
    json_obj["id"] = resp.id;
    json_obj["jsonrpc"] = resp.jsonrpc;
    json_obj["result"] = resp.result;
}

inline void from_json(const nlohmann::json& json_obj, JSONRPCResultResponse& resp) {
    resp.id = json_obj.at("id").get<RequestId>();
    json_obj.at("jsonrpc").get_to(resp.jsonrpc);
    json_obj.at("result").get_to(resp.result);
}

/**
 * @brief An error JSON-RPC response message.
 */
struct JSONRPCErrorResponse {
    Error error;                  ///< The error object.
    std::string jsonrpc = "2.0";  ///< JSON-RPC version (always "2.0").
    std::optional<RequestId> id;  ///< The request ID (may be absent for parse errors).
};

inline void to_json(nlohmann::json& json_obj, const JSONRPCErrorResponse& resp) {
    json_obj = nlohmann::json::object();
    json_obj["error"] = resp.error;
    json_obj["jsonrpc"] = resp.jsonrpc;
    if (resp.id) {
        json_obj["id"] = *resp.id;
    }
}

inline void from_json(const nlohmann::json& json_obj, JSONRPCErrorResponse& resp) {
    json_obj.at("error").get_to(resp.error);
    json_obj.at("jsonrpc").get_to(resp.jsonrpc);
    if (json_obj.contains("id")) {
        resp.id = json_obj.at("id").get<RequestId>();
    }
}

/// @brief Variant of all JSON-RPC response message forms.
using JSONRPCResponse = std::variant<JSONRPCResultResponse, JSONRPCErrorResponse>;

inline void to_json(nlohmann::json& json_obj, const JSONRPCResponse& resp) {
    std::visit([&json_obj](auto&& arg) { json_obj = arg; }, resp);
}

inline void from_json(const nlohmann::json& json_obj, JSONRPCResponse& resp) {
    if (json_obj.contains("error")) {
        resp = json_obj.get<JSONRPCErrorResponse>();
    } else {
        resp = json_obj.get<JSONRPCResultResponse>();
    }
}

/// @brief Variant of all JSON-RPC message forms handled by the SDK.
using JSONRPCMessage =
    std::variant<JSONRPCRequest, JSONRPCNotification, JSONRPCResultResponse, JSONRPCErrorResponse>;

inline void to_json(nlohmann::json& json_obj, const JSONRPCMessage& msg) {
    std::visit([&json_obj](auto&& arg) { json_obj = arg; }, msg);
}

inline void from_json(const nlohmann::json& json_obj, JSONRPCMessage& msg) {
    if (json_obj.contains("error")) {
        msg = json_obj.get<JSONRPCErrorResponse>();
    } else if (json_obj.contains("result")) {
        msg = json_obj.get<JSONRPCResultResponse>();
    } else if (json_obj.contains("id")) {
        msg = json_obj.get<JSONRPCRequest>();
    } else {
        msg = json_obj.get<JSONRPCNotification>();
    }
}

/**
 * @brief Severity threshold used for logging notifications.
 */
enum class LoggingLevel : std::uint8_t {
    Emergency,
    Alert,
    Critical,
    Error,
    Warning,
    Notice,
    Info,
    Debug
};

NLOHMANN_JSON_SERIALIZE_ENUM(LoggingLevel, {{LoggingLevel::Emergency, "emergency"},
                                            {LoggingLevel::Alert, "alert"},
                                            {LoggingLevel::Critical, "critical"},
                                            {LoggingLevel::Error, "error"},
                                            {LoggingLevel::Warning, "warning"},
                                            {LoggingLevel::Notice, "notice"},
                                            {LoggingLevel::Info, "info"},
                                            {LoggingLevel::Debug, "debug"}})

struct SetLevelRequestParams {
    LoggingLevel level;                  ///< The desired logging level.
    std::optional<nlohmann::json> meta;  ///< Reserved for protocol use.
};

inline void to_json(nlohmann::json& json_obj, const SetLevelRequestParams& params) {
    json_obj = nlohmann::json{{"level", params.level}};
    if (params.meta) {
        json_obj["_meta"] = *params.meta;
    }
}

inline void from_json(const nlohmann::json& json_obj, SetLevelRequestParams& params) {
    json_obj.at("level").get_to(params.level);
    if (json_obj.contains("_meta")) {
        params.meta = json_obj.at("_meta").get<nlohmann::json>();
    }
}

/**
 * @brief Request payload for the `logging/setLevel` method.
 */
struct SetLevelRequest {
    std::string method = "logging/setLevel";  ///< The method name.
    SetLevelRequestParams params;             ///< The request parameters.
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(SetLevelRequest, method, params)

/**
 * @brief A ping request.
 */
struct PingRequest {
    std::string method = "ping";  ///< The method name.
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(PingRequest, method)

/**
 * @brief Parameters for a cancelled notification.
 */
struct CancelledNotificationParams {
    RequestId requestId;  ///< The ID of the request to cancel.
    std::optional<std::string>
        reason;  ///< An optional string describing the reason for the cancellation.
};

inline void to_json(nlohmann::json& j, const CancelledNotificationParams& params) {
    j = nlohmann::json{{"requestId", params.requestId}};
    if (params.reason) {
        j["reason"] = *params.reason;
    }
}

inline void from_json(const nlohmann::json& j, CancelledNotificationParams& params) {
    j.at("requestId").get_to(params.requestId);
    if (j.contains("reason")) {
        params.reason = j.at("reason").get<std::string>();
    }
}

/**
 * @brief A cancelled notification.
 */
struct CancelledNotification {
    std::string method = "notifications/cancelled";  ///< The method name.
    CancelledNotificationParams params;              ///< The notification parameters.
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(CancelledNotification, method, params)

/**
 * @brief Parameters for a progress notification.
 */
struct ProgressNotificationParams {
    ProgressToken progressToken;         ///< The progress token.
    double progress;                     ///< The progress value.
    std::optional<double> total;         ///< The total progress value.
    std::optional<std::string> message;  ///< An optional message.
};

inline void to_json(nlohmann::json& j, const ProgressNotificationParams& params) {
    j = nlohmann::json{{"progressToken", params.progressToken}, {"progress", params.progress}};
    if (params.total) {
        j["total"] = *params.total;
    }
    if (params.message) {
        j["message"] = *params.message;
    }
}

inline void from_json(const nlohmann::json& j, ProgressNotificationParams& params) {
    j.at("progressToken").get_to(params.progressToken);
    j.at("progress").get_to(params.progress);
    if (j.contains("total")) {
        params.total = j.at("total").get<double>();
    }
    if (j.contains("message")) {
        params.message = j.at("message").get<std::string>();
    }
}

/**
 * @brief A progress notification.
 */
struct ProgressNotification {
    std::string method = "notifications/progress";  ///< The method name.
    ProgressNotificationParams params;              ///< The notification parameters.
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ProgressNotification, method, params)

/**
 * @brief Parameters for a logging message notification.
 */
struct LoggingMessageNotificationParams {
    LoggingLevel level;                 ///< The severity of this log message.
    std::optional<std::string> logger;  ///< An optional name of the logger issuing this message.
    nlohmann::json data;  ///< The data to be logged, such as a string message or an object.
};

inline void to_json(nlohmann::json& j, const LoggingMessageNotificationParams& params) {
    j = nlohmann::json{{"level", params.level}, {"data", params.data}};
    if (params.logger) {
        j["logger"] = *params.logger;
    }
}

inline void from_json(const nlohmann::json& j, LoggingMessageNotificationParams& params) {
    j.at("level").get_to(params.level);
    j.at("data").get_to(params.data);
    if (j.contains("logger")) {
        params.logger = j.at("logger").get<std::string>();
    }
}

/**
 * @brief A logging message notification.
 */
struct LoggingMessageNotification {
    std::string method = "notifications/message";  ///< The method name.
    LoggingMessageNotificationParams params;       ///< The notification parameters.
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(LoggingMessageNotification, method, params)

/**
 * @brief An initialized notification.
 */
struct InitializedNotification {
    std::string method = "notifications/initialized";  ///< The method name.
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(InitializedNotification, method)

/**
 * @brief A notification that the prompt list has changed.
 */
struct PromptListChangedNotification {
    std::string method = "notifications/prompts/list_changed";  ///< The method name.
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(PromptListChangedNotification, method)

/**
 * @brief A notification that the resource list has changed.
 */
struct ResourceListChangedNotification {
    std::string method = "notifications/resources/list_changed";  ///< The method name.
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ResourceListChangedNotification, method)

/**
 * @brief A notification that the tool list has changed.
 */
struct ToolListChangedNotification {
    std::string method = "notifications/tools/list_changed";  ///< The method name.
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ToolListChangedNotification, method)

/**
 * @brief A notification that the roots list has changed.
 */
struct RootsListChangedNotification {
    std::string method = "notifications/roots/list_changed";  ///< The method name.
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(RootsListChangedNotification, method)

/**
 * @brief Parameters for a resource updated notification.
 */
struct ResourceUpdatedNotificationParams {
    std::string uri;  ///< The URI of the resource that has been updated.
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ResourceUpdatedNotificationParams, uri)

/**
 * @brief A notification that a resource has been updated.
 */
struct ResourceUpdatedNotification {
    std::string method = "notifications/resources/updated";  ///< The method name.
    ResourceUpdatedNotificationParams params;                ///< The notification parameters.
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ResourceUpdatedNotification, method, params)

/**
 * @brief Parameters for subscribing to a resource.
 */
struct ResourceSubscribeParams {
    std::string uri;  ///< The URI of the resource to subscribe to.
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ResourceSubscribeParams, uri)

/**
 * @brief A request to subscribe to resource updates.
 */
struct SubscribeRequest {
    std::string method = "resources/subscribe";  ///< The method name.
    ResourceSubscribeParams params;              ///< The request parameters.
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(SubscribeRequest, method, params)

/**
 * @brief Parameters for unsubscribing from a resource.
 */
struct ResourceUnsubscribeParams {
    std::string uri;  ///< The URI of the resource to unsubscribe from.
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ResourceUnsubscribeParams, uri)

/**
 * @brief A request to unsubscribe from resource updates.
 */
struct UnsubscribeRequest {
    std::string method = "resources/unsubscribe";  ///< The method name.
    ResourceUnsubscribeParams params;              ///< The request parameters.
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(UnsubscribeRequest, method, params)

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

/**
 * @brief Parameters for listing available resources.
 */
struct ListResourcesRequestParams {
    std::optional<std::string> cursor;  ///< Optional pagination cursor.
};
inline void to_json(nlohmann::json& j, const ListResourcesRequestParams& p) {
    j = nlohmann::json::object();
    if (p.cursor) {
        j["cursor"] = *p.cursor;
    }
}
inline void from_json(const nlohmann::json& j, ListResourcesRequestParams& p) {
    if (j.contains("cursor")) {
        p.cursor = j.at("cursor").get<std::string>();
    }
}

/**
 * @brief A request to list all available resources.
 */
struct ListResourcesRequest {
    std::string method = "resources/list";             ///< The method name.
    std::optional<ListResourcesRequestParams> params;  ///< Optional request parameters.
};
inline void to_json(nlohmann::json& j, const ListResourcesRequest& r) {
    j = nlohmann::json{{"method", r.method}};
    if (r.params) {
        j["params"] = *r.params;
    }
}
inline void from_json(const nlohmann::json& j, ListResourcesRequest& r) {
    j.at("method").get_to(r.method);
    if (j.contains("params")) {
        r.params = j.at("params").get<ListResourcesRequestParams>();
    }
}

/**
 * @brief Result containing a list of available resources.
 */
struct ListResourcesResult {
    std::vector<Resource> resources;        ///< The list of resources in the current page.
    std::optional<std::string> nextCursor;  ///< Pagination cursor for the next page.
    std::optional<nlohmann::json> meta;     ///< Reserved for protocol use.
};
inline void to_json(nlohmann::json& j, const ListResourcesResult& r) {
    j = nlohmann::json{{"resources", r.resources}};
    if (r.nextCursor) {
        j["nextCursor"] = *r.nextCursor;
    }
    if (r.meta) {
        j["_meta"] = *r.meta;
    }
}
inline void from_json(const nlohmann::json& j, ListResourcesResult& r) {
    j.at("resources").get_to(r.resources);
    if (j.contains("nextCursor")) {
        r.nextCursor = j.at("nextCursor").get<std::string>();
    }
    if (j.contains("_meta")) {
        r.meta = j.at("_meta");
    }
}

/**
 * @brief Parameters for listing resource templates.
 */
struct ListResourceTemplatesRequestParams {
    std::optional<std::string> cursor;  ///< Optional pagination cursor.
};
inline void to_json(nlohmann::json& j, const ListResourceTemplatesRequestParams& p) {
    j = nlohmann::json::object();
    if (p.cursor) {
        j["cursor"] = *p.cursor;
    }
}
inline void from_json(const nlohmann::json& j, ListResourceTemplatesRequestParams& p) {
    if (j.contains("cursor")) {
        p.cursor = j.at("cursor").get<std::string>();
    }
}

/**
 * @brief A request to list all available resource templates.
 */
struct ListResourceTemplatesRequest {
    std::string method = "resources/templates/list";           ///< The method name.
    std::optional<ListResourceTemplatesRequestParams> params;  ///< Optional request parameters.
};
inline void to_json(nlohmann::json& j, const ListResourceTemplatesRequest& r) {
    j = nlohmann::json{{"method", r.method}};
    if (r.params) {
        j["params"] = *r.params;
    }
}
inline void from_json(const nlohmann::json& j, ListResourceTemplatesRequest& r) {
    j.at("method").get_to(r.method);
    if (j.contains("params")) {
        r.params = j.at("params").get<ListResourceTemplatesRequestParams>();
    }
}

/**
 * @brief Result containing a list of available resource templates.
 */
struct ListResourceTemplatesResult {
    std::vector<ResourceTemplate> resourceTemplates;  ///< The list of resource templates in the page.
    std::optional<std::string> nextCursor;            ///< Pagination cursor for the next page.
    std::optional<nlohmann::json> meta;               ///< Reserved for protocol use.
};
inline void to_json(nlohmann::json& j, const ListResourceTemplatesResult& r) {
    j = nlohmann::json{{"resourceTemplates", r.resourceTemplates}};
    if (r.nextCursor) {
        j["nextCursor"] = *r.nextCursor;
    }
    if (r.meta) {
        j["_meta"] = *r.meta;
    }
}
inline void from_json(const nlohmann::json& j, ListResourceTemplatesResult& r) {
    j.at("resourceTemplates").get_to(r.resourceTemplates);
    if (j.contains("nextCursor")) {
        r.nextCursor = j.at("nextCursor").get<std::string>();
    }
    if (j.contains("_meta")) {
        r.meta = j.at("_meta");
    }
}

/**
 * @brief Parameters for reading a resource.
 */
struct ReadResourceRequestParams {
    std::string uri;  ///< URI of the resource to read.
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ReadResourceRequestParams, uri)

/**
 * @brief A request to read the contents of a resource.
 */
struct ReadResourceRequest {
    std::string method = "resources/read";  ///< The method name.
    ReadResourceRequestParams params;       ///< The request parameters.
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ReadResourceRequest, method, params)

// Task Requests & Notifications

/**
 * @brief Parameters for retrieving a task by ID.
 */
struct GetTaskRequestParams {
    std::string id;  ///< Task identifier to fetch.
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(GetTaskRequestParams, id)

/**
 * @brief A request to get a task by its ID.
 */
struct GetTaskRequest {
    std::string method = "tasks/get";  ///< The method name.
    GetTaskRequestParams params;       ///< The request parameters.
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(GetTaskRequest, method, params)

/**
 * @brief Parameters for cancelling a task.
 */
struct CancelTaskRequestParams {
    std::string id;  ///< Task identifier to cancel.
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(CancelTaskRequestParams, id)

/**
 * @brief A request to cancel a task.
 */
struct CancelTaskRequest {
    std::string method = "tasks/cancel";  ///< The method name.
    CancelTaskRequestParams params;       ///< The request parameters.
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(CancelTaskRequest, method, params)

/**
 * @brief Result of a task cancellation request.
 */
struct CancelTaskResult {
    std::optional<nlohmann::json> meta;  ///< Reserved for protocol use.
};
inline void to_json(nlohmann::json& j, const CancelTaskResult& r) {
    j = nlohmann::json::object();
    if (r.meta) {
        j["_meta"] = *r.meta;
    }
}
inline void from_json(const nlohmann::json& j, CancelTaskResult& r) {
    if (j.contains("_meta")) {
        r.meta = j.at("_meta");
    }
}

/**
 * @brief Parameters for listing tasks.
 */
struct ListTasksRequestParams {
    std::optional<std::string> cursor;  ///< Optional pagination cursor.
};
inline void to_json(nlohmann::json& j, const ListTasksRequestParams& p) {
    j = nlohmann::json::object();
    if (p.cursor) {
        j["cursor"] = *p.cursor;
    }
}
inline void from_json(const nlohmann::json& j, ListTasksRequestParams& p) {
    if (j.contains("cursor")) {
        p.cursor = j.at("cursor").get<std::string>();
    }
}

/**
 * @brief A request to list all tasks.
 */
struct ListTasksRequest {
    std::string method = "tasks/list";             ///< The method name.
    std::optional<ListTasksRequestParams> params;  ///< Optional request parameters.
};
inline void to_json(nlohmann::json& j, const ListTasksRequest& r) {
    j = nlohmann::json{{"method", r.method}};
    if (r.params) {
        j["params"] = *r.params;
    }
}
inline void from_json(const nlohmann::json& j, ListTasksRequest& r) {
    j.at("method").get_to(r.method);
    if (j.contains("params")) {
        r.params = j.at("params").get<ListTasksRequestParams>();
    }
}

/**
 * @brief Parameters for retrieving a task's payload.
 */
struct GetTaskPayloadRequestParams {
    std::string id;  ///< Task identifier whose payload should be fetched.
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(GetTaskPayloadRequestParams, id)

/**
 * @brief A request to get a task's payload.
 */
struct GetTaskPayloadRequest {
    std::string method = "tasks/getPayload";  ///< The method name.
    GetTaskPayloadRequestParams params;       ///< The request parameters.
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(GetTaskPayloadRequest, method, params)

/**
 * @brief Result containing a task's payload data.
 */
struct GetTaskPayloadResult {
    nlohmann::json payload;              ///< Payload associated with the task.
    std::optional<nlohmann::json> meta;  ///< Reserved for protocol use.
};
inline void to_json(nlohmann::json& j, const GetTaskPayloadResult& r) {
    j = nlohmann::json{{"payload", r.payload}};
    if (r.meta) {
        j["_meta"] = *r.meta;
    }
}
inline void from_json(const nlohmann::json& j, GetTaskPayloadResult& r) {
    j.at("payload").get_to(r.payload);
    if (j.contains("_meta")) {
        r.meta = j.at("_meta");
    }
}

/**
 * @brief Parameters for task status notifications.
 */
struct TaskStatusNotificationParams {
    std::string id;                        ///< Task identifier whose status changed.
    TaskStatus status;                     ///< Updated task status.
    std::optional<TaskMetadata> metadata;  ///< Optional task metadata snapshot.
    std::optional<std::string> message;    ///< Optional human-readable status message.
};
inline void to_json(nlohmann::json& j, const TaskStatusNotificationParams& p) {
    j = nlohmann::json{{"id", p.id}, {"status", p.status}};
    if (p.metadata) {
        j["metadata"] = *p.metadata;
    }
    if (p.message) {
        j["message"] = *p.message;
    }
}
inline void from_json(const nlohmann::json& j, TaskStatusNotificationParams& p) {
    j.at("id").get_to(p.id);
    j.at("status").get_to(p.status);
    if (j.contains("metadata")) {
        p.metadata = j.at("metadata").get<TaskMetadata>();
    }
    if (j.contains("message")) {
        p.message = j.at("message").get<std::string>();
    }
}

/**
 * @brief A notification about a task's status change.
 */
struct TaskStatusNotification {
    std::string method = "notifications/tasks/status";  ///< The method name.
    TaskStatusNotificationParams params;                ///< The notification parameters.
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(TaskStatusNotification, method, params)

/**
 * @brief Parameters for elicitation completion notifications.
 */
struct ElicitationCompleteNotificationParams {
    RequestId requestId;  ///< Request identifier for the completed elicitation.
};
inline void to_json(nlohmann::json& j, const ElicitationCompleteNotificationParams& p) {
    j = nlohmann::json{{"requestId", p.requestId}};
}
inline void from_json(const nlohmann::json& j, ElicitationCompleteNotificationParams& p) {
    j.at("requestId").get_to(p.requestId);
}

/**
 * @brief A notification that an elicitation process has completed.
 */
struct ElicitationCompleteNotification {
    std::string method = "notifications/elicitation/complete";  ///< The method name.
    ElicitationCompleteNotificationParams params;               ///< The notification parameters.
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ElicitationCompleteNotification, method, params)

}  // namespace mcp

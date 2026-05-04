#pragma once

#include <mcp/protocol/base.hpp>

#include <algorithm>
#include <array>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mcp {

/**
 * @brief Protocol version 2024-11-05.
 */
constexpr std::string_view g_PROTOCOL_VERSION_2024_11_05 = "2024-11-05";

/**
 * @brief Protocol version 2025-03-26.
 */
constexpr std::string_view g_PROTOCOL_VERSION_2025_03_26 = "2025-03-26";

/**
 * @brief Protocol version 2025-06-18.
 */
constexpr std::string_view g_PROTOCOL_VERSION_2025_06_18 = "2025-06-18";

/**
 * @brief Protocol version 2025-11-25.
 */
constexpr std::string_view g_PROTOCOL_VERSION_2025_11_25 = "2025-11-25";

/**
 * @brief The latest supported protocol version.
 */
constexpr std::string_view g_LATEST_PROTOCOL_VERSION = g_PROTOCOL_VERSION_2025_11_25;

/**
 * @brief List of all supported protocol versions.
 */
constexpr std::array<std::string_view, 4> g_SUPPORTED_PROTOCOL_VERSIONS = {
    g_PROTOCOL_VERSION_2024_11_05, g_PROTOCOL_VERSION_2025_03_26, g_PROTOCOL_VERSION_2025_06_18,
    g_PROTOCOL_VERSION_2025_11_25};

/**
 * @brief Check whether a protocol version is supported by this SDK.
 *
 * @param version The protocol version to check.
 * @return true if the version is present in g_SUPPORTED_PROTOCOL_VERSIONS.
 */
[[nodiscard]] constexpr bool is_supported_protocol_version(std::string_view version) {
    return std::find(g_SUPPORTED_PROTOCOL_VERSIONS.begin(), g_SUPPORTED_PROTOCOL_VERSIONS.end(),
                     version) != g_SUPPORTED_PROTOCOL_VERSIONS.end();
}

/**
 * @brief Negotiate the protocol version to use for a session.
 *
 * @details If the requested version is supported, it is selected. Otherwise,
 * the latest supported version is used as a fallback.
 *
 * @param requested_version The version requested by the peer.
 * @return The negotiated protocol version.
 */
[[nodiscard]] constexpr std::string_view negotiate_protocol_version(
    std::string_view requested_version) {
    return is_supported_protocol_version(requested_version) ? requested_version
                                                            : g_LATEST_PROTOCOL_VERSION;
}

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
 * @brief A ping request.
 */
struct PingRequest {
    std::string method = "ping";  ///< The method name.
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(PingRequest, method)

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

}  // namespace mcp

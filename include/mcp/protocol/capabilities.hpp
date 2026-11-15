#pragma once

#include <mcp/core/constants.hpp>
#include <mcp/protocol/base.hpp>

#include <algorithm>
#include <array>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mcp {

/**
 * @brief Check whether a protocol version is listed in
 * mcp::constants::protocol::g_SUPPORTED_PROTOCOL_VERSIONS, in either era.
 *
 * @details initialize negotiation does not use it; see is_legacy_protocol_version().
 *
 * @param version The protocol version to check.
 * @return true if the version is present in mcp::constants::protocol::g_SUPPORTED_PROTOCOL_VERSIONS.
 */
[[nodiscard]] constexpr bool is_supported_protocol_version(std::string_view version) {
    return std::find(mcp::constants::protocol::g_SUPPORTED_PROTOCOL_VERSIONS.begin(),
                     mcp::constants::protocol::g_SUPPORTED_PROTOCOL_VERSIONS.end(),
                     version) != mcp::constants::protocol::g_SUPPORTED_PROTOCOL_VERSIONS.end();
}

/**
 * @brief Check whether a protocol version starts a session with initialize.
 *
 * @param version The protocol version to check.
 * @return true if the version is present in mcp::constants::protocol::g_LEGACY_PROTOCOL_VERSIONS.
 */
[[nodiscard]] constexpr bool is_legacy_protocol_version(std::string_view version) {
    return std::find(mcp::constants::protocol::g_LEGACY_PROTOCOL_VERSIONS.begin(),
                     mcp::constants::protocol::g_LEGACY_PROTOCOL_VERSIONS.end(),
                     version) != mcp::constants::protocol::g_LEGACY_PROTOCOL_VERSIONS.end();
}

/**
 * @brief Check whether a protocol version is served per request, without initialize.
 *
 * @param version The protocol version to check.
 * @return true if the version is present in mcp::constants::protocol::g_MODERN_PROTOCOL_VERSIONS.
 */
[[nodiscard]] constexpr bool is_modern_protocol_version(std::string_view version) {
    return std::find(mcp::constants::protocol::g_MODERN_PROTOCOL_VERSIONS.begin(),
                     mcp::constants::protocol::g_MODERN_PROTOCOL_VERSIONS.end(),
                     version) != mcp::constants::protocol::g_MODERN_PROTOCOL_VERSIONS.end();
}

/**
 * @brief Negotiate the protocol version of an initialize request.
 *
 * @details A requested version that starts a session with initialize is selected. Any other
 * version, including one served per request such as 2026-07-28, falls back to
 * mcp::constants::protocol::g_LATEST_LEGACY_PROTOCOL_VERSION.
 *
 * @param requested_version The version requested by the peer.
 * @return The negotiated protocol version.
 */
[[nodiscard]] constexpr std::string_view negotiate_protocol_version(
    std::string_view requested_version) {
    return is_legacy_protocol_version(requested_version)
               ? requested_version
               : mcp::constants::protocol::g_LATEST_LEGACY_PROTOCOL_VERSION;
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
    if (detail::has_json_value(json_obj, "mimeType")) {
        icon.mimeType = json_obj.at("mimeType").get<std::string>();
    }
    if (detail::has_json_value(json_obj, "sizes")) {
        icon.sizes = json_obj.at("sizes").get<std::vector<std::string>>();
    }
    if (detail::has_json_value(json_obj, "theme")) {
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
    if (detail::has_json_value(json_obj, "audience")) {
        annotations.audience = json_obj.at("audience").get<std::vector<Role>>();
    }
    if (detail::has_json_value(json_obj, "lastModified")) {
        annotations.lastModified = json_obj.at("lastModified").get<std::string>();
    }
    if (detail::has_json_value(json_obj, "priority")) {
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
    if (detail::has_json_value(json_obj, "title")) {
        impl.title = json_obj.at("title").get<std::string>();
    }
    if (detail::has_json_value(json_obj, "description")) {
        impl.description = json_obj.at("description").get<std::string>();
    }
    if (detail::has_json_value(json_obj, "websiteUrl")) {
        impl.websiteUrl = json_obj.at("websiteUrl").get<std::string>();
    }
    if (detail::has_json_value(json_obj, "icons")) {
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

    std::optional<ElicitationCapability> elicitation;  ///< Support for elicitation requests.
    std::optional<nlohmann::json> experimental;  ///< Implementation-defined experimental capabilities.
    std::optional<RootsCapability> roots;        ///< Support for roots/list requests.
    std::optional<SamplingCapability> sampling;  ///< Support for sampling/createMessage requests.
    std::optional<std::map<std::string, nlohmann::json>>
        extensions;  ///< Extension capability negotiation, keyed by extension name.
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
    if (detail::has_json_value(j, "listChanged")) {
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
    if (cap.extensions) {
        j["extensions"] = *cap.extensions;
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
    if (detail::has_json_value(j, "extensions")) {
        cap.extensions = j.at("extensions").get<std::map<std::string, nlohmann::json>>();
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
    std::optional<ToolsCapability> tools;          ///< Support for tool endpoints.
    std::optional<std::map<std::string, nlohmann::json>>
        extensions;  ///< Extension capability negotiation, keyed by extension name.
};

inline void to_json(nlohmann::json& j, const ServerCapabilities::PromptsCapability& cap) {
    j = nlohmann::json::object();
    if (cap.listChanged) {
        j["listChanged"] = *cap.listChanged;
    }
}

inline void from_json(const nlohmann::json& j, ServerCapabilities::PromptsCapability& cap) {
    if (detail::has_json_value(j, "listChanged")) {
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
    if (detail::has_json_value(j, "listChanged")) {
        cap.listChanged = j.at("listChanged").get<bool>();
    }
    if (detail::has_json_value(j, "subscribe")) {
        cap.subscribe = j.at("subscribe").get<bool>();
    }
}

inline void to_json(nlohmann::json& j, const ServerCapabilities::ToolsCapability& cap) {
    j = nlohmann::json::object();
    if (cap.listChanged) {
        j["listChanged"] = *cap.listChanged;
    }
}

inline void from_json(const nlohmann::json& j, ServerCapabilities::ToolsCapability& cap) {
    if (detail::has_json_value(j, "listChanged")) {
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
    if (cap.tools) {
        j["tools"] = *cap.tools;
    }
    if (cap.extensions) {
        j["extensions"] = *cap.extensions;
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
    if (j.contains("tools")) {
        cap.tools = j.at("tools").get<ServerCapabilities::ToolsCapability>();
    }
    if (detail::has_json_value(j, "extensions")) {
        cap.extensions = j.at("extensions").get<std::map<std::string, nlohmann::json>>();
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

/**
 * @brief Represents a server/discover request from the client.
 *
 * @details Carries no body parameters beyond the standard `_meta` field. Per the
 * 2026-07-28 spec, `_meta` may include `io.modelcontextprotocol/protocolVersion`,
 * `io.modelcontextprotocol/clientInfo`, and `io.modelcontextprotocol/clientCapabilities`;
 * this type accepts and preserves the raw `_meta` object without interpreting its
 * contents.
 */
struct DiscoverRequest {
    std::optional<nlohmann::json> meta;  ///< Reserved for protocol use; contents not interpreted yet.
};

inline void to_json(nlohmann::json& json_obj, const DiscoverRequest& req) {
    json_obj = nlohmann::json::object();
    if (req.meta) {
        json_obj["_meta"] = *req.meta;
    }
}

inline void from_json(const nlohmann::json& json_obj, DiscoverRequest& req) {
    if (json_obj.contains("_meta")) {
        req.meta = json_obj.at("_meta");
    }
}

/**
 * @brief Caching scope hint for a cacheable result, per server/utilities/caching.
 */
enum class CacheScope : std::uint8_t {
    ePublic,   ///< The response does not contain user-specific data and may be shared.
    ePrivate,  ///< The response is scoped to the caller's authorization context.
};

NLOHMANN_JSON_SERIALIZE_ENUM(CacheScope,
                             {{CacheScope::ePublic, "public"}, {CacheScope::ePrivate, "private"}})

namespace detail {

/// Reads the `ttlMs` and `cacheScope` caching hints of a result. A `cacheScope` this SDK does not
/// recognise is left unset: the enum's JSON conversion would read it as "public", the scope that
/// lets a cache share the result across callers.
inline void read_cache_hints(const nlohmann::json& json_obj, std::optional<std::int64_t>& ttl_ms,
                             std::optional<CacheScope>& cache_scope) {
    if (has_json_value(json_obj, "ttlMs")) {
        ttl_ms = json_obj.at("ttlMs").get<std::int64_t>();
    }
    if (const auto scope = json_obj.find("cacheScope"); scope != json_obj.end() && scope->is_string()) {
        if (*scope == "public") {
            cache_scope = CacheScope::ePublic;
        } else if (*scope == "private") {
            cache_scope = CacheScope::ePrivate;
        }
    }
}

/// Writes the `ttlMs` and `cacheScope` caching hints of a result, each only when it is set.
inline void write_cache_hints(nlohmann::json& json_obj, const std::optional<std::int64_t>& ttl_ms,
                              const std::optional<CacheScope>& cache_scope) {
    if (ttl_ms) {
        json_obj["ttlMs"] = *ttl_ms;
    }
    if (cache_scope) {
        json_obj["cacheScope"] = *cache_scope;
    }
}

}  // namespace detail

/**
 * @brief Represents the result of a server/discover request.
 *
 * @details `resultType` is always "complete" for this result.
 */
struct DiscoverResult {
    std::string resultType = "complete";         ///< Always "complete" for this result.
    std::vector<std::string> supportedVersions;  ///< Protocol versions the server supports.
    ServerCapabilities capabilities;             ///< The capabilities supported by the server.
    std::optional<ServerInfo> serverInfo;     ///< Server identity, carried under `_meta` on the wire.
    std::optional<std::string> instructions;  ///< Optional instructions for the client.
    std::optional<std::int64_t> ttlMs;        ///< Optional caching TTL hint, in milliseconds.
    std::optional<CacheScope> cacheScope;     ///< Optional caching scope hint.
};

/**
 * @brief Serializes DiscoverResult to JSON.
 *
 * @param json_obj The JSON object to populate.
 * @param res The DiscoverResult object to serialize.
 */
inline void to_json(nlohmann::json& json_obj, const DiscoverResult& res) {
    json_obj = nlohmann::json{
        {"resultType", res.resultType},
        {"supportedVersions", res.supportedVersions},
        {"capabilities", res.capabilities},
    };
    if (res.serverInfo) {
        json_obj["_meta"] = {{"io.modelcontextprotocol/serverInfo", *res.serverInfo}};
    }
    if (res.instructions) {
        json_obj["instructions"] = *res.instructions;
    }
    detail::write_cache_hints(json_obj, res.ttlMs, res.cacheScope);
}

/**
 * @brief Deserializes DiscoverResult from JSON.
 *
 * @param json_obj The JSON object to read from.
 * @param res The DiscoverResult object to populate.
 */
inline void from_json(const nlohmann::json& json_obj, DiscoverResult& res) {
    json_obj.at("resultType").get_to(res.resultType);
    json_obj.at("supportedVersions").get_to(res.supportedVersions);
    json_obj.at("capabilities").get_to(res.capabilities);
    if (json_obj.contains("_meta") &&
        detail::has_json_value(json_obj.at("_meta"), "io.modelcontextprotocol/serverInfo")) {
        res.serverInfo =
            json_obj.at("_meta").at("io.modelcontextprotocol/serverInfo").get<ServerInfo>();
    }
    if (detail::has_json_value(json_obj, "instructions")) {
        res.instructions = json_obj.at("instructions").get<std::string>();
    }
    detail::read_cache_hints(json_obj, res.ttlMs, res.cacheScope);
}

}  // namespace mcp

#pragma once

#include <mcp/core/constants.hpp>
#include <mcp/core/export.hpp>
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
    return std::ranges::find(mcp::constants::protocol::g_SUPPORTED_PROTOCOL_VERSIONS, version) !=
           mcp::constants::protocol::g_SUPPORTED_PROTOCOL_VERSIONS.end();
}

/**
 * @brief Check whether a protocol version starts a session with initialize.
 *
 * @param version The protocol version to check.
 * @return true if the version is present in mcp::constants::protocol::g_LEGACY_PROTOCOL_VERSIONS.
 */
[[nodiscard]] constexpr bool is_legacy_protocol_version(std::string_view version) {
    return std::ranges::find(mcp::constants::protocol::g_LEGACY_PROTOCOL_VERSIONS, version) !=
           mcp::constants::protocol::g_LEGACY_PROTOCOL_VERSIONS.end();
}

/**
 * @brief Check whether a protocol version is served per request, without initialize.
 *
 * @param version The protocol version to check.
 * @return true if the version is present in mcp::constants::protocol::g_MODERN_PROTOCOL_VERSIONS.
 */
[[nodiscard]] constexpr bool is_modern_protocol_version(std::string_view version) {
    return std::ranges::find(mcp::constants::protocol::g_MODERN_PROTOCOL_VERSIONS, version) !=
           mcp::constants::protocol::g_MODERN_PROTOCOL_VERSIONS.end();
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

    bool operator==(const Icon&) const = default;
};

/**
 * @brief Serializes Icon to JSON.
 *
 * @param json_obj The JSON object to populate.
 * @param icon The Icon object to serialize.
 */
MCP_API void to_json(nlohmann::json& json_obj, const Icon& icon);

/**
 * @brief Deserializes Icon from JSON.
 *
 * @param json_obj The JSON object to read from.
 * @param icon The Icon object to populate.
 */
MCP_API void from_json(const nlohmann::json& json_obj, Icon& icon);

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
MCP_API void to_json(nlohmann::json& json_obj, const Annotations& annotations);

/**
 * @brief Deserializes Annotations from JSON.
 *
 * @param json_obj The JSON object to read from.
 * @param annotations The Annotations object to populate.
 */
MCP_API void from_json(const nlohmann::json& json_obj, Annotations& annotations);

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

    bool operator==(const Implementation&) const = default;
};

/**
 * @brief Serializes Implementation to JSON.
 *
 * @param json_obj The JSON object to populate.
 * @param impl The Implementation object to serialize.
 */
MCP_API void to_json(nlohmann::json& json_obj, const Implementation& impl);

/**
 * @brief Deserializes Implementation from JSON.
 *
 * @param json_obj The JSON object to read from.
 * @param impl The Implementation object to populate.
 */
MCP_API void from_json(const nlohmann::json& json_obj, Implementation& impl);

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

MCP_API void to_json(nlohmann::json& j, const ClientCapabilities::ElicitationCapability& cap);

MCP_API void from_json(const nlohmann::json& j, ClientCapabilities::ElicitationCapability& cap);

MCP_API void to_json(nlohmann::json& j, const ClientCapabilities::RootsCapability& cap);

MCP_API void from_json(const nlohmann::json& j, ClientCapabilities::RootsCapability& cap);

MCP_API void to_json(nlohmann::json& j, const ClientCapabilities::SamplingCapability& cap);

MCP_API void from_json(const nlohmann::json& j, ClientCapabilities::SamplingCapability& cap);

MCP_API void to_json(nlohmann::json& j, const ClientCapabilities& cap);

MCP_API void from_json(const nlohmann::json& j, ClientCapabilities& cap);

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

        bool operator==(const PromptsCapability&) const = default;
    };

    /**
     * @brief Server support for resource listing and subscription operations.
     */
    struct ResourcesCapability {
        std::optional<bool>
            listChanged;  ///< Whether the server supports resource list change notifications.
        std::optional<bool>
            subscribe;  ///< Whether the server supports subscribing to resource updates.

        bool operator==(const ResourcesCapability&) const = default;
    };

    /**
     * @brief Server support for tool listing change notifications.
     */
    struct ToolsCapability {
        std::optional<bool>
            listChanged;  ///< Whether the server supports tool list change notifications.

        bool operator==(const ToolsCapability&) const = default;
    };

    std::optional<nlohmann::json> completions;   ///< Support for completion/complete requests.
    std::optional<nlohmann::json> experimental;  ///< Implementation-defined experimental capabilities.
    std::optional<nlohmann::json> logging;       ///< Support for logging/setLevel.
    std::optional<PromptsCapability> prompts;    ///< Support for prompt endpoints.
    std::optional<ResourcesCapability> resources;  ///< Support for resource endpoints.
    std::optional<ToolsCapability> tools;          ///< Support for tool endpoints.
    std::optional<std::map<std::string, nlohmann::json>>
        extensions;  ///< Extension capability negotiation, keyed by extension name.

    bool operator==(const ServerCapabilities&) const = default;
};

MCP_API void to_json(nlohmann::json& j, const ServerCapabilities::PromptsCapability& cap);

MCP_API void from_json(const nlohmann::json& j, ServerCapabilities::PromptsCapability& cap);

MCP_API void to_json(nlohmann::json& j, const ServerCapabilities::ResourcesCapability& cap);

MCP_API void from_json(const nlohmann::json& j, ServerCapabilities::ResourcesCapability& cap);

MCP_API void to_json(nlohmann::json& j, const ServerCapabilities::ToolsCapability& cap);

MCP_API void from_json(const nlohmann::json& j, ServerCapabilities::ToolsCapability& cap);

MCP_API void to_json(nlohmann::json& j, const ServerCapabilities& cap);

MCP_API void from_json(const nlohmann::json& j, ServerCapabilities& cap);

/**
 * @brief A ping request.
 */
struct PingRequest {
    std::string method{mcp::constants::methods::g_PING};  ///< The method name.
};
MCP_API void to_json(nlohmann::json& json_obj, const PingRequest& value);
MCP_API void from_json(const nlohmann::json& json_obj, PingRequest& value);

/**
 * @brief Represents an initialization request from the client.
 */
struct InitializeRequest {
    std::string protocolVersion;      ///< The protocol version supported by the client.
    ClientInfo clientInfo;            ///< Information about the client implementation.
    ClientCapabilities capabilities;  ///< The capabilities supported by the client.
};

MCP_API void to_json(nlohmann::json& json_obj, const InitializeRequest& value);
MCP_API void from_json(const nlohmann::json& json_obj, InitializeRequest& value);

/**
 * @brief Represents the result of an initialization request.
 */
struct InitializeResult {
    std::string protocolVersion;              ///< The protocol version selected by the server.
    ServerCapabilities capabilities;          ///< The capabilities supported by the server.
    ServerInfo serverInfo;                    ///< Information about the server implementation.
    std::optional<std::string> instructions;  ///< Optional instructions for the client.

    bool operator==(const InitializeResult&) const = default;
};

/**
 * @brief Serializes InitializeResult to JSON.
 *
 * @param json_obj The JSON object to populate.
 * @param res The InitializeResult object to serialize.
 */
MCP_API void to_json(nlohmann::json& json_obj, const InitializeResult& res);

/**
 * @brief Deserializes InitializeResult from JSON.
 *
 * @param json_obj The JSON object to read from.
 * @param res The InitializeResult object to populate.
 */
MCP_API void from_json(const nlohmann::json& json_obj, InitializeResult& res);

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

MCP_API void to_json(nlohmann::json& json_obj, const DiscoverRequest& req);

MCP_API void from_json(const nlohmann::json& json_obj, DiscoverRequest& req);

/**
 * @brief Caching scope hint for a cacheable result, per server/utilities/caching.
 */
enum class CacheScope : std::uint8_t {
    ePublic,   ///< The response does not contain user-specific data and may be shared.
    ePrivate,  ///< The response is scoped to the caller's authorization context.
};

NLOHMANN_JSON_SERIALIZE_ENUM(CacheScope,
                             {{CacheScope::ePublic, "public"}, {CacheScope::ePrivate, "private"}})

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
MCP_API void to_json(nlohmann::json& json_obj, const DiscoverResult& res);

/**
 * @brief Deserializes DiscoverResult from JSON.
 *
 * @param json_obj The JSON object to read from.
 * @param res The DiscoverResult object to populate.
 */
MCP_API void from_json(const nlohmann::json& json_obj, DiscoverResult& res);

}  // namespace mcp

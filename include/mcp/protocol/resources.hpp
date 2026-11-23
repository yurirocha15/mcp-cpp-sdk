#pragma once

#include <mcp/core/constants.hpp>
#include <mcp/core/export.hpp>
#include <mcp/protocol/base.hpp>
#include <mcp/protocol/capabilities.hpp>
#include <mcp/protocol/content.hpp>

namespace mcp {

// GUIDE: Move the following from protocol.hpp to here:
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
MCP_API void to_json(nlohmann::json& json_obj, const Resource& resource);

/**
 * @brief Deserializes Resource from JSON.
 *
 * @param json_obj The JSON object to read from.
 * @param resource The Resource object to populate.
 */
MCP_API void from_json(const nlohmann::json& json_obj, Resource& resource);

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

MCP_API void to_json(nlohmann::json& json_obj, const ResourceTemplate& tmpl);

MCP_API void from_json(const nlohmann::json& json_obj, ResourceTemplate& tmpl);

/**
 * @brief Result of reading a resource.
 */
struct ReadResourceResult {
    std::vector<ResourceContents> contents;  ///< The contents of the resource.
    std::optional<std::int64_t> ttlMs;       ///< Caching TTL hint, in milliseconds (2026-07-28).
    std::optional<CacheScope> cacheScope;    ///< Caching scope hint (2026-07-28).
};

MCP_API void to_json(nlohmann::json& j, const ReadResourceResult& r);

MCP_API void from_json(const nlohmann::json& j, ReadResourceResult& r);

/**
 * @brief Parameters for listing available resources.
 */
struct ListResourcesRequestParams {
    std::optional<std::string> cursor;  ///< Optional pagination cursor.
};
MCP_API void to_json(nlohmann::json& j, const ListResourcesRequestParams& p);
MCP_API void from_json(const nlohmann::json& j, ListResourcesRequestParams& p);

/**
 * @brief A request to list all available resources.
 */
struct ListResourcesRequest {
    std::string method{mcp::constants::methods::g_RESOURCES_LIST};  ///< The method name.
    std::optional<ListResourcesRequestParams> params;               ///< Optional request parameters.
};
MCP_API void to_json(nlohmann::json& j, const ListResourcesRequest& r);
MCP_API void from_json(const nlohmann::json& j, ListResourcesRequest& r);

/**
 * @brief Result containing a list of available resources.
 */
struct ListResourcesResult {
    std::vector<Resource> resources;        ///< The list of resources in the current page.
    std::optional<std::string> nextCursor;  ///< Pagination cursor for the next page.
    std::optional<nlohmann::json> meta;     ///< Reserved for protocol use.
    std::optional<std::int64_t> ttlMs;      ///< Caching TTL hint, in milliseconds (2026-07-28).
    std::optional<CacheScope> cacheScope;   ///< Caching scope hint (2026-07-28).
};
MCP_API void to_json(nlohmann::json& j, const ListResourcesResult& r);
MCP_API void from_json(const nlohmann::json& j, ListResourcesResult& r);

/**
 * @brief Parameters for listing resource templates.
 */
struct ListResourceTemplatesRequestParams {
    std::optional<std::string> cursor;  ///< Optional pagination cursor.
};
MCP_API void to_json(nlohmann::json& j, const ListResourceTemplatesRequestParams& p);
MCP_API void from_json(const nlohmann::json& j, ListResourceTemplatesRequestParams& p);

/**
 * @brief A request to list all available resource templates.
 */
struct ListResourceTemplatesRequest {
    std::string method{mcp::constants::methods::g_RESOURCES_TEMPLATES_LIST};  ///< The method name.
    std::optional<ListResourceTemplatesRequestParams> params;  ///< Optional request parameters.
};
MCP_API void to_json(nlohmann::json& j, const ListResourceTemplatesRequest& r);
MCP_API void from_json(const nlohmann::json& j, ListResourceTemplatesRequest& r);

/**
 * @brief Result containing a list of available resource templates.
 */
struct ListResourceTemplatesResult {
    std::vector<ResourceTemplate> resourceTemplates;  ///< The list of resource templates in the page.
    std::optional<std::string> nextCursor;            ///< Pagination cursor for the next page.
    std::optional<nlohmann::json> meta;               ///< Reserved for protocol use.
    std::optional<std::int64_t> ttlMs;     ///< Caching TTL hint, in milliseconds (2026-07-28).
    std::optional<CacheScope> cacheScope;  ///< Caching scope hint (2026-07-28).
};
MCP_API void to_json(nlohmann::json& j, const ListResourceTemplatesResult& r);
MCP_API void from_json(const nlohmann::json& j, ListResourceTemplatesResult& r);

/**
 * @brief Parameters for reading a resource.
 */
struct ReadResourceRequestParams {
    std::string uri;  ///< URI of the resource to read.
};
MCP_API void to_json(nlohmann::json& json_obj, const ReadResourceRequestParams& value);
MCP_API void from_json(const nlohmann::json& json_obj, ReadResourceRequestParams& value);

/**
 * @brief A request to read the contents of a resource.
 */
struct ReadResourceRequest {
    std::string method{mcp::constants::methods::g_RESOURCES_READ};  ///< The method name.
    ReadResourceRequestParams params;                               ///< The request parameters.
};
MCP_API void to_json(nlohmann::json& json_obj, const ReadResourceRequest& value);
MCP_API void from_json(const nlohmann::json& json_obj, ReadResourceRequest& value);

/**
 * @brief Parameters for subscribing to a resource.
 */
struct ResourceSubscribeParams {
    std::string uri;  ///< The URI of the resource to subscribe to.
};
MCP_API void to_json(nlohmann::json& json_obj, const ResourceSubscribeParams& value);
MCP_API void from_json(const nlohmann::json& json_obj, ResourceSubscribeParams& value);

/**
 * @brief A request to subscribe to resource updates.
 */
struct SubscribeRequest {
    std::string method{mcp::constants::methods::g_RESOURCES_SUBSCRIBE};  ///< The method name.
    ResourceSubscribeParams params;                                      ///< The request parameters.
};
MCP_API void to_json(nlohmann::json& json_obj, const SubscribeRequest& value);
MCP_API void from_json(const nlohmann::json& json_obj, SubscribeRequest& value);

/**
 * @brief Parameters for unsubscribing from a resource.
 */
struct ResourceUnsubscribeParams {
    std::string uri;  ///< The URI of the resource to unsubscribe from.
};
MCP_API void to_json(nlohmann::json& json_obj, const ResourceUnsubscribeParams& value);
MCP_API void from_json(const nlohmann::json& json_obj, ResourceUnsubscribeParams& value);

/**
 * @brief A request to unsubscribe from resource updates.
 */
struct UnsubscribeRequest {
    std::string method{mcp::constants::methods::g_RESOURCES_UNSUBSCRIBE};  ///< The method name.
    ResourceUnsubscribeParams params;                                      ///< The request parameters.
};
MCP_API void to_json(nlohmann::json& json_obj, const UnsubscribeRequest& value);
MCP_API void from_json(const nlohmann::json& json_obj, UnsubscribeRequest& value);

}  // namespace mcp

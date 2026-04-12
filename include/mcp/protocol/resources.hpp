#pragma once

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

}  // namespace mcp

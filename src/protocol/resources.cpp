#include <mcp/protocol/resources.hpp>

#include <string>
#include <vector>
#include "cache_hints.hpp"

namespace mcp {

void to_json(nlohmann::json& json_obj, const Resource& resource) {
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

void from_json(const nlohmann::json& json_obj, Resource& resource) {
    resource = Resource{};
    json_obj.at("uri").get_to(resource.uri);
    json_obj.at("name").get_to(resource.name);
    if (detail::has_json_value(json_obj, "description")) {
        resource.description = json_obj.at("description").get<std::string>();
    }
    if (detail::has_json_value(json_obj, "mimeType")) {
        resource.mimeType = json_obj.at("mimeType").get<std::string>();
    }
    if (detail::has_json_value(json_obj, "_meta")) {
        resource.meta = json_obj.at("_meta").get<nlohmann::json>();
    }
    if (detail::has_json_value(json_obj, "annotations")) {
        resource.annotations = json_obj.at("annotations").get<Annotations>();
    }
    if (detail::has_json_value(json_obj, "size")) {
        resource.size = json_obj.at("size").get<int64_t>();
    }
    if (detail::has_json_value(json_obj, "title")) {
        resource.title = json_obj.at("title").get<std::string>();
    }
    if (detail::has_json_value(json_obj, "icons")) {
        resource.icons = json_obj.at("icons").get<std::vector<Icon>>();
    }
}

void to_json(nlohmann::json& json_obj, const ResourceTemplate& tmpl) {
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

void from_json(const nlohmann::json& json_obj, ResourceTemplate& tmpl) {
    tmpl = ResourceTemplate{};
    json_obj.at("uriTemplate").get_to(tmpl.uriTemplate);
    json_obj.at("name").get_to(tmpl.name);
    if (detail::has_json_value(json_obj, "description")) {
        tmpl.description = json_obj.at("description").get<std::string>();
    }
    if (detail::has_json_value(json_obj, "mimeType")) {
        tmpl.mimeType = json_obj.at("mimeType").get<std::string>();
    }
    if (detail::has_json_value(json_obj, "_meta")) {
        tmpl.meta = json_obj.at("_meta").get<nlohmann::json>();
    }
    if (detail::has_json_value(json_obj, "annotations")) {
        tmpl.annotations = json_obj.at("annotations").get<Annotations>();
    }
    if (detail::has_json_value(json_obj, "title")) {
        tmpl.title = json_obj.at("title").get<std::string>();
    }
    if (detail::has_json_value(json_obj, "icons")) {
        tmpl.icons = json_obj.at("icons").get<std::vector<Icon>>();
    }
}

void to_json(nlohmann::json& j, const ReadResourceResult& r) {
    j = nlohmann::json{{"contents", r.contents}};
    detail::write_cache_hints(j, r.ttlMs, r.cacheScope);
}

void from_json(const nlohmann::json& j, ReadResourceResult& r) {
    r = ReadResourceResult{};
    j.at("contents").get_to(r.contents);
    detail::read_cache_hints(j, r.ttlMs, r.cacheScope);
}

void to_json(nlohmann::json& j, const ListResourcesRequestParams& p) {
    j = nlohmann::json::object();
    if (p.cursor) {
        j["cursor"] = *p.cursor;
    }
}

void from_json(const nlohmann::json& j, ListResourcesRequestParams& p) {
    p = ListResourcesRequestParams{};
    if (detail::has_json_value(j, "cursor")) {
        p.cursor = j.at("cursor").get<std::string>();
    }
}

void to_json(nlohmann::json& j, const ListResourcesRequest& r) {
    j = nlohmann::json{{"method", r.method}};
    if (r.params) {
        j["params"] = *r.params;
    }
}

void from_json(const nlohmann::json& j, ListResourcesRequest& r) {
    r = ListResourcesRequest{};
    j.at("method").get_to(r.method);
    if (j.contains("params")) {
        r.params = j.at("params").get<ListResourcesRequestParams>();
    }
}

void to_json(nlohmann::json& j, const ListResourcesResult& r) {
    j = nlohmann::json{{"resources", r.resources}};
    if (r.nextCursor) {
        j["nextCursor"] = *r.nextCursor;
    }
    if (r.meta) {
        j["_meta"] = *r.meta;
    }
    detail::write_cache_hints(j, r.ttlMs, r.cacheScope);
}

void from_json(const nlohmann::json& j, ListResourcesResult& r) {
    r = ListResourcesResult{};
    j.at("resources").get_to(r.resources);
    if (detail::has_json_value(j, "nextCursor")) {
        r.nextCursor = j.at("nextCursor").get<std::string>();
    }
    if (j.contains("_meta")) {
        r.meta = j.at("_meta");
    }
    detail::read_cache_hints(j, r.ttlMs, r.cacheScope);
}

void to_json(nlohmann::json& j, const ListResourceTemplatesRequestParams& p) {
    j = nlohmann::json::object();
    if (p.cursor) {
        j["cursor"] = *p.cursor;
    }
}

void from_json(const nlohmann::json& j, ListResourceTemplatesRequestParams& p) {
    p = ListResourceTemplatesRequestParams{};
    if (detail::has_json_value(j, "cursor")) {
        p.cursor = j.at("cursor").get<std::string>();
    }
}

void to_json(nlohmann::json& j, const ListResourceTemplatesRequest& r) {
    j = nlohmann::json{{"method", r.method}};
    if (r.params) {
        j["params"] = *r.params;
    }
}

void from_json(const nlohmann::json& j, ListResourceTemplatesRequest& r) {
    r = ListResourceTemplatesRequest{};
    j.at("method").get_to(r.method);
    if (j.contains("params")) {
        r.params = j.at("params").get<ListResourceTemplatesRequestParams>();
    }
}

void to_json(nlohmann::json& j, const ListResourceTemplatesResult& r) {
    j = nlohmann::json{{"resourceTemplates", r.resourceTemplates}};
    if (r.nextCursor) {
        j["nextCursor"] = *r.nextCursor;
    }
    if (r.meta) {
        j["_meta"] = *r.meta;
    }
    detail::write_cache_hints(j, r.ttlMs, r.cacheScope);
}

void from_json(const nlohmann::json& j, ListResourceTemplatesResult& r) {
    r = ListResourceTemplatesResult{};
    j.at("resourceTemplates").get_to(r.resourceTemplates);
    if (detail::has_json_value(j, "nextCursor")) {
        r.nextCursor = j.at("nextCursor").get<std::string>();
    }
    if (j.contains("_meta")) {
        r.meta = j.at("_meta");
    }
    detail::read_cache_hints(j, r.ttlMs, r.cacheScope);
}

void to_json(nlohmann::json& json_obj, const ReadResourceRequestParams& value) {
    json_obj["uri"] = value.uri;
}

void from_json(const nlohmann::json& json_obj, ReadResourceRequestParams& value) {
    json_obj.at("uri").get_to(value.uri);
}

void to_json(nlohmann::json& json_obj, const ReadResourceRequest& value) {
    json_obj["method"] = value.method;
    json_obj["params"] = value.params;
}

void from_json(const nlohmann::json& json_obj, ReadResourceRequest& value) {
    json_obj.at("method").get_to(value.method);
    json_obj.at("params").get_to(value.params);
}

void to_json(nlohmann::json& json_obj, const ResourceSubscribeParams& value) {
    json_obj["uri"] = value.uri;
}

void from_json(const nlohmann::json& json_obj, ResourceSubscribeParams& value) {
    json_obj.at("uri").get_to(value.uri);
}

void to_json(nlohmann::json& json_obj, const SubscribeRequest& value) {
    json_obj["method"] = value.method;
    json_obj["params"] = value.params;
}

void from_json(const nlohmann::json& json_obj, SubscribeRequest& value) {
    json_obj.at("method").get_to(value.method);
    json_obj.at("params").get_to(value.params);
}

void to_json(nlohmann::json& json_obj, const ResourceUnsubscribeParams& value) {
    json_obj["uri"] = value.uri;
}

void from_json(const nlohmann::json& json_obj, ResourceUnsubscribeParams& value) {
    json_obj.at("uri").get_to(value.uri);
}

void to_json(nlohmann::json& json_obj, const UnsubscribeRequest& value) {
    json_obj["method"] = value.method;
    json_obj["params"] = value.params;
}

void from_json(const nlohmann::json& json_obj, UnsubscribeRequest& value) {
    json_obj.at("method").get_to(value.method);
    json_obj.at("params").get_to(value.params);
}

}  // namespace mcp

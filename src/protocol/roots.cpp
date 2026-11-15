#include <mcp/protocol/base.hpp>
#include <mcp/protocol/roots.hpp>

#include <string>

namespace mcp {

void to_json(nlohmann::json& json_obj, const Root& root) {
    json_obj = nlohmann::json{{"uri", root.uri}};
    if (root.name) {
        json_obj["name"] = *root.name;
    }
    if (root.meta) {
        json_obj["_meta"] = *root.meta;
    }
}

void from_json(const nlohmann::json& json_obj, Root& root) {
    root = Root{};
    json_obj.at("uri").get_to(root.uri);
    if (detail::has_json_value(json_obj, "name")) {
        root.name = json_obj.at("name").get<std::string>();
    }
    if (json_obj.contains("_meta")) {
        root.meta = json_obj.at("_meta").get<nlohmann::json>();
    }
}

void to_json(nlohmann::json& json_obj, const ListRootsRequest& value) {
    json_obj["method"] = value.method;
}

void from_json(const nlohmann::json& json_obj, ListRootsRequest& value) {
    json_obj.at("method").get_to(value.method);
}

void to_json(nlohmann::json& json_obj, const ListRootsResult& result) {
    json_obj = nlohmann::json{{"roots", result.roots}};
    if (result.meta) {
        json_obj["_meta"] = *result.meta;
    }
}

void from_json(const nlohmann::json& json_obj, ListRootsResult& result) {
    result = ListRootsResult{};
    json_obj.at("roots").get_to(result.roots);
    if (json_obj.contains("_meta")) {
        result.meta = json_obj.at("_meta").get<nlohmann::json>();
    }
}

}  // namespace mcp

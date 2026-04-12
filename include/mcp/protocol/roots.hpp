#pragma once

#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace mcp {

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

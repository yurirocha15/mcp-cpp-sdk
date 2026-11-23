#pragma once

#include <mcp/core/constants.hpp>
#include <mcp/core/export.hpp>
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
MCP_API void to_json(nlohmann::json& json_obj, const Root& root);

/**
 * @brief Deserializes Root from JSON.
 *
 * @param json_obj The JSON object to read from.
 * @param root The Root object to populate.
 */
MCP_API void from_json(const nlohmann::json& json_obj, Root& root);

/**
 * @brief Sent from the server to request a list of root URIs from the client.
 */
struct ListRootsRequest {
    std::string method{mcp::constants::methods::g_ROOTS_LIST};  ///< The method name.
};

MCP_API void to_json(nlohmann::json& json_obj, const ListRootsRequest& value);
MCP_API void from_json(const nlohmann::json& json_obj, ListRootsRequest& value);

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
MCP_API void to_json(nlohmann::json& json_obj, const ListRootsResult& result);

/**
 * @brief Deserializes ListRootsResult from JSON.
 *
 * @param json_obj The JSON object to read from.
 * @param result The ListRootsResult object to populate.
 */
MCP_API void from_json(const nlohmann::json& json_obj, ListRootsResult& result);

}  // namespace mcp

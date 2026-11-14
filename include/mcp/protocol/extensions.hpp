#pragma once

#include <mcp/protocol/capabilities.hpp>
#include <mcp/protocol/request_meta.hpp>

#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace mcp {

/**
 * @file
 * @brief Declaring and reading protocol extensions.
 *
 * @details Revision 2026-07-28 lets a client and a server negotiate optional extensions under
 * the `extensions` member of their capabilities, keyed by the extension's identifier (for example
 * `io.modelcontextprotocol/tasks`). An extension is supported when its key is present, and its
 * value holds the extension's settings, which is an empty object for an extension without any.
 *
 * A server declares its extensions in the capabilities it gives Server and reports through
 * `server/discover`; a client declares its own in the capabilities of each request, which a
 * server handler reads through Context::request_meta(). These helpers read and write that member
 * for either side.
 */

/**
 * @brief Declares support for extension @p id.
 *
 * @param capabilities The capabilities to change.
 * @param id The extension's identifier.
 * @param settings The extension's settings; an empty object when it has none. Replaces the
 *                 settings of an earlier declaration of @p id.
 */
inline void declare_extension(ClientCapabilities& capabilities, std::string_view id,
                              nlohmann::json settings = nlohmann::json::object()) {
    if (!capabilities.extensions) {
        capabilities.extensions.emplace();
    }
    (*capabilities.extensions)[std::string(id)] = std::move(settings);
}

/// @copydoc declare_extension(ClientCapabilities&, std::string_view, nlohmann::json)
inline void declare_extension(ServerCapabilities& capabilities, std::string_view id,
                              nlohmann::json settings = nlohmann::json::object()) {
    if (!capabilities.extensions) {
        capabilities.extensions.emplace();
    }
    (*capabilities.extensions)[std::string(id)] = std::move(settings);
}

namespace detail {

inline const nlohmann::json* find_extension(
    const std::optional<std::map<std::string, nlohmann::json>>& extensions, std::string_view id) {
    if (!extensions) {
        return nullptr;
    }
    const auto found = extensions->find(std::string(id));
    return found == extensions->end() ? nullptr : &found->second;
}

}  // namespace detail

/**
 * @brief The settings the client declared for extension @p id.
 *
 * @return The settings, which are an empty object when the client declared the extension without
 *         any, or nullopt when it did not declare it.
 */
[[nodiscard]] inline std::optional<nlohmann::json> extension_settings(
    const ClientCapabilities& capabilities, std::string_view id) {
    const auto* found = detail::find_extension(capabilities.extensions, id);
    if (found == nullptr) {
        return std::nullopt;
    }
    if (!found->is_object()) {
        return nlohmann::json::object();
    }
    // In place: with nlohmann_json before 3.12 a const json converts to std::optional<json> two
    // ways.
    return std::optional<nlohmann::json>(std::in_place, *found);
}

/// The settings the server declared for extension @p id, as extension_settings() reads a client's.
[[nodiscard]] inline std::optional<nlohmann::json> extension_settings(
    const ServerCapabilities& capabilities, std::string_view id) {
    const auto* found = detail::find_extension(capabilities.extensions, id);
    if (found == nullptr) {
        return std::nullopt;
    }
    if (!found->is_object()) {
        return nlohmann::json::object();
    }
    // In place: with nlohmann_json before 3.12 a const json converts to std::optional<json> two
    // ways.
    return std::optional<nlohmann::json>(std::in_place, *found);
}

/**
 * @brief The settings the client declared for extension @p id on the request @p meta describes.
 *
 * @param meta The request's per-request fields, as Context::request_meta() returns them; null for
 *             a request of an earlier revision, whose client declared no extensions.
 * @param id The extension identifier.
 */
[[nodiscard]] inline std::optional<nlohmann::json> extension_settings(const RequestMetadata* meta,
                                                                      std::string_view id) {
    if (meta == nullptr) {
        return std::nullopt;
    }
    return extension_settings(meta->clientCapabilities, id);
}

/// Whether the client declared extension @p id.
[[nodiscard]] inline bool declares_extension(const ClientCapabilities& capabilities,
                                             std::string_view id) {
    return detail::find_extension(capabilities.extensions, id) != nullptr;
}

/// Whether the server declared extension @p id.
[[nodiscard]] inline bool declares_extension(const ServerCapabilities& capabilities,
                                             std::string_view id) {
    return detail::find_extension(capabilities.extensions, id) != nullptr;
}

/// Whether the client declared extension @p id on the request @p meta describes; false for null.
[[nodiscard]] inline bool declares_extension(const RequestMetadata* meta, std::string_view id) {
    return meta != nullptr && declares_extension(meta->clientCapabilities, id);
}

}  // namespace mcp

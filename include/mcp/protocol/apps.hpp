#pragma once

#include <mcp/core/constants.hpp>
#include <mcp/core/context.hpp>
#include <mcp/protocol/capabilities.hpp>
#include <mcp/protocol/extensions.hpp>
#include <mcp/protocol/request_meta.hpp>
#include <mcp/protocol/resources.hpp>
#include <mcp/protocol/tools.hpp>

#include <algorithm>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/**
 * @file apps.hpp
 * @brief Wire types and capability helpers of the MCP Apps extension (`io.modelcontextprotocol/ui`).
 *
 * @details MCP Apps lets a server attach an interactive HTML view to a tool. The view is a
 * predeclared `ui://` resource served with the `text/html;profile=mcp-app` MIME type; the tool
 * names it in `_meta.ui.resourceUri`. Rendering the view and the host-to-view messages are the
 * host's concern and are not part of this SDK. The types here follow the 2026-01-26 revision of the
 * extension, github.com/modelcontextprotocol/ext-apps, specification/2026-01-26/apps.mdx.
 */

namespace mcp::apps {

/// Origins a view may reach, each list mapping to one Content Security Policy directive.
struct ResourceCsp {
    std::optional<std::vector<std::string>> connectDomains;   ///< `connect-src`.
    std::optional<std::vector<std::string>> resourceDomains;  ///< Static resources of the view.
    std::optional<std::vector<std::string>> frameDomains;     ///< `frame-src`.
    std::optional<std::vector<std::string>> baseUriDomains;   ///< `base-uri`.
};

/// Browser capabilities a view asks the host to allow. Each member is `{}` on the wire.
struct ResourcePermissions {
    bool camera{false};          ///< Camera access.
    bool microphone{false};      ///< Microphone access.
    bool geolocation{false};     ///< Geolocation access.
    bool clipboardWrite{false};  ///< Clipboard write access.
};

/// The `_meta.ui` object of a view resource or of the content a read returns.
struct ResourceMeta {
    std::optional<ResourceCsp> csp;                  ///< Origins the view needs.
    std::optional<ResourcePermissions> permissions;  ///< Browser capabilities the view asks for.
    std::optional<std::string> domain;               ///< Dedicated sandbox origin; host-defined format.
    std::optional<bool> prefersBorder;               ///< Whether the host should draw a border.
};

/// Who may call a tool that links a view.
enum class Visibility {
    eModel,  ///< Visible to and callable by the model.
    eApp,    ///< Callable by the view, from the server that serves it.
};

/// The `_meta.ui` object of a tool.
struct ToolMeta {
    std::optional<std::string> resourceUri;             ///< View that renders the tool's result.
    std::optional<std::vector<Visibility>> visibility;  ///< Defaults to model and app when absent.
};

namespace detail {

/// Writes @p domains under @p key when it is set.
inline void put_domains(nlohmann::json& object, const char* key,
                        const std::optional<std::vector<std::string>>& domains) {
    if (domains) {
        object[key] = *domains;
    }
}

/// Reads the string array under @p key into @p domains; a missing or non-array member leaves it unset.
inline void get_domains(const nlohmann::json& object, const char* key,
                        std::optional<std::vector<std::string>>& domains) {
    const auto found = object.find(key);
    if (found != object.end() && found->is_array()) {
        domains = found->get<std::vector<std::string>>();
    }
}

/// Writes an empty object under @p key when @p granted is true.
inline void put_permission(nlohmann::json& object, const char* key, bool granted) {
    if (granted) {
        object[key] = nlohmann::json::object();
    }
}

}  // namespace detail

/// Writes the directives that are set; an unset list is omitted.
inline void to_json(nlohmann::json& json_obj, const ResourceCsp& csp) {
    json_obj = nlohmann::json::object();
    detail::put_domains(json_obj, "connectDomains", csp.connectDomains);
    detail::put_domains(json_obj, "resourceDomains", csp.resourceDomains);
    detail::put_domains(json_obj, "frameDomains", csp.frameDomains);
    detail::put_domains(json_obj, "baseUriDomains", csp.baseUriDomains);
}

/// Reads the domain lists; one that is missing or not an array stays unset.
inline void from_json(const nlohmann::json& json_obj, ResourceCsp& csp) {
    csp = ResourceCsp{};
    if (!json_obj.is_object()) {
        return;
    }
    detail::get_domains(json_obj, "connectDomains", csp.connectDomains);
    detail::get_domains(json_obj, "resourceDomains", csp.resourceDomains);
    detail::get_domains(json_obj, "frameDomains", csp.frameDomains);
    detail::get_domains(json_obj, "baseUriDomains", csp.baseUriDomains);
}

/// Writes `{}` under the name of each permission that is granted.
inline void to_json(nlohmann::json& json_obj, const ResourcePermissions& permissions) {
    json_obj = nlohmann::json::object();
    detail::put_permission(json_obj, "camera", permissions.camera);
    detail::put_permission(json_obj, "microphone", permissions.microphone);
    detail::put_permission(json_obj, "geolocation", permissions.geolocation);
    detail::put_permission(json_obj, "clipboardWrite", permissions.clipboardWrite);
}

/// Grants each permission whose name is present.
inline void from_json(const nlohmann::json& json_obj, ResourcePermissions& permissions) {
    permissions = ResourcePermissions{};
    if (!json_obj.is_object()) {
        return;
    }
    permissions.camera = json_obj.contains("camera");
    permissions.microphone = json_obj.contains("microphone");
    permissions.geolocation = json_obj.contains("geolocation");
    permissions.clipboardWrite = json_obj.contains("clipboardWrite");
}

/// Serializes the members of `_meta.ui`; use make_meta() to wrap it under `ui`.
inline void to_json(nlohmann::json& json_obj, const ResourceMeta& meta) {
    json_obj = nlohmann::json::object();
    if (meta.csp) {
        json_obj["csp"] = *meta.csp;
    }
    if (meta.permissions) {
        json_obj["permissions"] = *meta.permissions;
    }
    if (meta.domain) {
        json_obj["domain"] = *meta.domain;
    }
    if (meta.prefersBorder) {
        json_obj["prefersBorder"] = *meta.prefersBorder;
    }
}

/// Reads the members of `_meta.ui`; a member of the wrong type stays unset.
inline void from_json(const nlohmann::json& json_obj, ResourceMeta& meta) {
    meta = ResourceMeta{};
    if (!json_obj.is_object()) {
        return;
    }
    if (const auto csp = json_obj.find("csp"); csp != json_obj.end() && csp->is_object()) {
        meta.csp = csp->get<ResourceCsp>();
    }
    if (const auto permissions = json_obj.find("permissions");
        permissions != json_obj.end() && permissions->is_object()) {
        meta.permissions = permissions->get<ResourcePermissions>();
    }
    if (const auto domain = json_obj.find("domain"); domain != json_obj.end() && domain->is_string()) {
        meta.domain = domain->get<std::string>();
    }
    if (const auto border = json_obj.find("prefersBorder");
        border != json_obj.end() && border->is_boolean()) {
        meta.prefersBorder = border->get<bool>();
    }
}

/// Writes `"model"` or `"app"`.
inline void to_json(nlohmann::json& json_obj, const Visibility& visibility) {
    json_obj = visibility == Visibility::eModel ? "model" : "app";
}

/// @throws std::invalid_argument For a string other than `"model"` or `"app"`.
inline void from_json(const nlohmann::json& json_obj, Visibility& visibility) {
    const auto& text = json_obj.get_ref<const std::string&>();
    if (text == "model") {
        visibility = Visibility::eModel;
    } else if (text == "app") {
        visibility = Visibility::eApp;
    } else {
        throw std::invalid_argument("Unknown MCP Apps visibility: " + text);
    }
}

/// Serializes the members of `_meta.ui` of a tool.
inline void to_json(nlohmann::json& json_obj, const ToolMeta& meta) {
    json_obj = nlohmann::json::object();
    if (meta.resourceUri) {
        json_obj["resourceUri"] = *meta.resourceUri;
    }
    if (meta.visibility) {
        json_obj["visibility"] = *meta.visibility;
    }
}

/// Reads the members of `_meta.ui` of a tool; a member of the wrong type stays unset.
inline void from_json(const nlohmann::json& json_obj, ToolMeta& meta) {
    meta = ToolMeta{};
    if (!json_obj.is_object()) {
        return;
    }
    if (const auto uri = json_obj.find("resourceUri"); uri != json_obj.end() && uri->is_string()) {
        meta.resourceUri = uri->get<std::string>();
    }
    if (const auto visibility = json_obj.find("visibility");
        visibility != json_obj.end() && visibility->is_array()) {
        // An entry this version does not know is ignored, so a later revision's audience does not
        // make the whole tool unreadable.
        std::vector<Visibility> known;
        for (const auto& entry : *visibility) {
            if (entry.is_string() && (entry == std::string("model") || entry == std::string("app"))) {
                known.push_back(entry.get<Visibility>());
            }
        }
        meta.visibility = std::move(known);
    }
}

/// @return Whether @p uri names a view resource: it starts with `ui://`.
[[nodiscard]] inline bool is_view_uri(std::string_view uri) {
    return uri.size() > mcp::constants::apps::g_URI_PREFIX.size() &&
           uri.substr(0, mcp::constants::apps::g_URI_PREFIX.size()) ==
               mcp::constants::apps::g_URI_PREFIX;
}

/// @return `{"ui": meta}`, the `_meta` object that carries @p meta on a resource or a tool.
template <typename Meta>
[[nodiscard]] inline nlohmann::json make_meta(const Meta& meta) {
    return nlohmann::json{{"ui", nlohmann::json(meta)}};
}

/// @return The `ui` object of a `_meta` value, or nothing when it has none.
[[nodiscard]] inline const nlohmann::json* find_ui_meta(const std::optional<nlohmann::json>& meta) {
    if (!meta || !meta->is_object()) {
        return nullptr;
    }
    const auto ui = meta->find("ui");
    return ui != meta->end() && ui->is_object() ? &*ui : nullptr;
}

/**
 * @brief The view a tool links to and who may call it.
 *
 * @details Reads `_meta.ui`; a tool that carries only the deprecated flat `_meta["ui/resourceUri"]`
 * reads as if it had `_meta.ui.resourceUri`. `_meta.ui.resourceUri` wins when both are present.
 *
 * @return The tool's metadata, or nothing when the tool links no view and sets no visibility.
 */
[[nodiscard]] inline std::optional<ToolMeta> tool_meta(const Tool& tool) {
    std::optional<ToolMeta> result;
    if (const auto* ui = find_ui_meta(tool.meta)) {
        result = ui->get<ToolMeta>();
    }
    if (!result || !result->resourceUri) {
        if (tool.meta && tool.meta->is_object()) {
            const auto legacy =
                tool.meta->find(std::string(mcp::constants::apps::g_LEGACY_RESOURCE_URI_KEY));
            if (legacy != tool.meta->end() && legacy->is_string()) {
                if (!result) {
                    result = ToolMeta{};
                }
                result->resourceUri = legacy->get<std::string>();
            }
        }
    }
    return result;
}

/// @return The `ui://` URI of the view a tool links to, or nothing when it links none.
[[nodiscard]] inline std::optional<std::string> tool_view_uri(const Tool& tool) {
    const auto meta = tool_meta(tool);
    if (meta && meta->resourceUri && is_view_uri(*meta->resourceUri)) {
        return meta->resourceUri;
    }
    return std::nullopt;
}

/// @return Whether the model may see and call @p tool; the default when no visibility is set.
[[nodiscard]] inline bool visible_to_model(const Tool& tool) {
    const auto meta = tool_meta(tool);
    if (!meta || !meta->visibility) {
        return true;
    }
    return std::find(meta->visibility->begin(), meta->visibility->end(), Visibility::eModel) !=
           meta->visibility->end();
}

/// @return Whether a view may call @p tool; the default when no visibility is set.
[[nodiscard]] inline bool visible_to_app(const Tool& tool) {
    const auto meta = tool_meta(tool);
    if (!meta || !meta->visibility) {
        return true;
    }
    return std::find(meta->visibility->begin(), meta->visibility->end(), Visibility::eApp) !=
           meta->visibility->end();
}

/// @return The view metadata of a `_meta` value (`_meta.ui` of a resource or of read content).
[[nodiscard]] inline std::optional<ResourceMeta> resource_meta(
    const std::optional<nlohmann::json>& meta) {
    if (const auto* ui = find_ui_meta(meta)) {
        return ui->get<ResourceMeta>();
    }
    return std::nullopt;
}

/// The settings object of the extension on the client side.
[[nodiscard]] inline nlohmann::json client_settings() {
    return nlohmann::json{
        {"mimeTypes",
         nlohmann::json::array({std::string(mcp::constants::apps::g_RESOURCE_MIME_TYPE)})}};
}

/// Declares MCP Apps support in @p capabilities, as a host does. Replaces an earlier declaration.
inline void declare_support(ClientCapabilities& capabilities) {
    declare_extension(capabilities, mcp::constants::apps::g_EXTENSION_ID, client_settings());
}

/**
 * @brief Whether a client declared MCP Apps support for HTML views.
 *
 * @return true when `extensions["io.modelcontextprotocol/ui"].mimeTypes` lists
 *         `text/html;profile=mcp-app`.
 */
[[nodiscard]] inline bool supports_views(const ClientCapabilities& capabilities) {
    const auto settings = extension_settings(capabilities, mcp::constants::apps::g_EXTENSION_ID);
    if (!settings) {
        return false;
    }
    const auto types = settings->find("mimeTypes");
    if (types == settings->end() || !types->is_array()) {
        return false;
    }
    return std::any_of(types->begin(), types->end(), [](const nlohmann::json& type) {
        return type.is_string() &&
               type.get_ref<const std::string&>() == mcp::constants::apps::g_RESOURCE_MIME_TYPE;
    });
}

/**
 * @brief Whether the client of a request declared MCP Apps support.
 *
 * @details Reads the capabilities the client sent with a 2026-07-28 request. A request of an
 * earlier revision carries none, so this is false for it.
 */
[[nodiscard]] inline bool supports_views(const Context& context) {
    const auto* meta = context.request_meta();
    return meta != nullptr && supports_views(meta->clientCapabilities);
}

}  // namespace mcp::apps

#include <mcp/protocol/apps.hpp>

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace mcp::apps {

namespace detail {
namespace {

/// Writes @p domains under @p key when it is set.
void put_domains(nlohmann::json& object, const char* key,
                 const std::optional<std::vector<std::string>>& domains) {
    if (domains) {
        object[key] = *domains;
    }
}

/// Reads the string array under @p key into @p domains; a missing or non-array member leaves it unset.
void get_domains(const nlohmann::json& object, const char* key,
                 std::optional<std::vector<std::string>>& domains) {
    const auto found = object.find(key);
    if (found != object.end() && found->is_array()) {
        domains = found->get<std::vector<std::string>>();
    }
}

/// Writes an empty object under @p key when @p granted is true.
void put_permission(nlohmann::json& object, const char* key, bool granted) {
    if (granted) {
        object[key] = nlohmann::json::object();
    }
}

}  // namespace
}  // namespace detail

void to_json(nlohmann::json& json_obj, const ResourceCsp& csp) {
    json_obj = nlohmann::json::object();
    detail::put_domains(json_obj, "connectDomains", csp.connectDomains);
    detail::put_domains(json_obj, "resourceDomains", csp.resourceDomains);
    detail::put_domains(json_obj, "frameDomains", csp.frameDomains);
    detail::put_domains(json_obj, "baseUriDomains", csp.baseUriDomains);
}

void from_json(const nlohmann::json& json_obj, ResourceCsp& csp) {
    csp = ResourceCsp{};
    if (!json_obj.is_object()) {
        return;
    }
    detail::get_domains(json_obj, "connectDomains", csp.connectDomains);
    detail::get_domains(json_obj, "resourceDomains", csp.resourceDomains);
    detail::get_domains(json_obj, "frameDomains", csp.frameDomains);
    detail::get_domains(json_obj, "baseUriDomains", csp.baseUriDomains);
}

void to_json(nlohmann::json& json_obj, const ResourcePermissions& permissions) {
    json_obj = nlohmann::json::object();
    detail::put_permission(json_obj, "camera", permissions.camera);
    detail::put_permission(json_obj, "microphone", permissions.microphone);
    detail::put_permission(json_obj, "geolocation", permissions.geolocation);
    detail::put_permission(json_obj, "clipboardWrite", permissions.clipboardWrite);
}

void from_json(const nlohmann::json& json_obj, ResourcePermissions& permissions) {
    permissions = ResourcePermissions{};
    if (!json_obj.is_object()) {
        return;
    }
    permissions.camera = json_obj.contains("camera");
    permissions.microphone = json_obj.contains("microphone");
    permissions.geolocation = json_obj.contains("geolocation");
    permissions.clipboardWrite = json_obj.contains("clipboardWrite");
}

void to_json(nlohmann::json& json_obj, const ResourceMeta& meta) {
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

void from_json(const nlohmann::json& json_obj, ResourceMeta& meta) {
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

void to_json(nlohmann::json& json_obj, const Visibility& visibility) {
    json_obj = visibility == Visibility::eModel ? "model" : "app";
}

void from_json(const nlohmann::json& json_obj, Visibility& visibility) {
    const auto& text = json_obj.get_ref<const std::string&>();
    if (text == "model") {
        visibility = Visibility::eModel;
    } else if (text == "app") {
        visibility = Visibility::eApp;
    } else {
        throw std::invalid_argument("Unknown MCP Apps visibility: " + text);
    }
}

void to_json(nlohmann::json& json_obj, const ToolMeta& meta) {
    json_obj = nlohmann::json::object();
    if (meta.resourceUri) {
        json_obj["resourceUri"] = *meta.resourceUri;
    }
    if (meta.visibility) {
        json_obj["visibility"] = *meta.visibility;
    }
}

void from_json(const nlohmann::json& json_obj, ToolMeta& meta) {
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

}  // namespace mcp::apps

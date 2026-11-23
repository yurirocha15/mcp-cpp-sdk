#include <mcp/protocol/capabilities.hpp>

#include <map>
#include <optional>
#include <string>
#include <vector>
#include "cache_hints.hpp"

namespace mcp {

void to_json(nlohmann::json& json_obj, const Icon& icon) {
    json_obj = nlohmann::json{{"src", icon.source}};
    if (icon.mimeType) {
        json_obj["mimeType"] = *icon.mimeType;
    }
    if (icon.sizes) {
        json_obj["sizes"] = *icon.sizes;
    }
    if (icon.theme) {
        json_obj["theme"] = *icon.theme;
    }
}

void from_json(const nlohmann::json& json_obj, Icon& icon) {
    icon = Icon{};
    json_obj.at("src").get_to(icon.source);
    if (detail::has_json_value(json_obj, "mimeType")) {
        icon.mimeType = json_obj.at("mimeType").get<std::string>();
    }
    if (detail::has_json_value(json_obj, "sizes")) {
        icon.sizes = json_obj.at("sizes").get<std::vector<std::string>>();
    }
    if (detail::has_json_value(json_obj, "theme")) {
        icon.theme = json_obj.at("theme").get<std::string>();
    }
}

void to_json(nlohmann::json& json_obj, const Annotations& annotations) {
    json_obj = nlohmann::json::object();
    if (annotations.audience) {
        json_obj["audience"] = *annotations.audience;
    }
    if (annotations.lastModified) {
        json_obj["lastModified"] = *annotations.lastModified;
    }
    if (annotations.priority) {
        json_obj["priority"] = *annotations.priority;
    }
}

void from_json(const nlohmann::json& json_obj, Annotations& annotations) {
    annotations = Annotations{};
    if (detail::has_json_value(json_obj, "audience")) {
        annotations.audience = json_obj.at("audience").get<std::vector<Role>>();
    }
    if (detail::has_json_value(json_obj, "lastModified")) {
        annotations.lastModified = json_obj.at("lastModified").get<std::string>();
    }
    if (detail::has_json_value(json_obj, "priority")) {
        annotations.priority = json_obj.at("priority").get<double>();
    }
}

void to_json(nlohmann::json& json_obj, const Implementation& impl) {
    json_obj = nlohmann::json{{"name", impl.name}, {"version", impl.version}};
    if (impl.title) {
        json_obj["title"] = *impl.title;
    }
    if (impl.description) {
        json_obj["description"] = *impl.description;
    }
    if (impl.websiteUrl) {
        json_obj["websiteUrl"] = *impl.websiteUrl;
    }
    if (impl.icons) {
        json_obj["icons"] = *impl.icons;
    }
}

void from_json(const nlohmann::json& json_obj, Implementation& impl) {
    impl = Implementation{};
    json_obj.at("name").get_to(impl.name);
    json_obj.at("version").get_to(impl.version);
    if (detail::has_json_value(json_obj, "title")) {
        impl.title = json_obj.at("title").get<std::string>();
    }
    if (detail::has_json_value(json_obj, "description")) {
        impl.description = json_obj.at("description").get<std::string>();
    }
    if (detail::has_json_value(json_obj, "websiteUrl")) {
        impl.websiteUrl = json_obj.at("websiteUrl").get<std::string>();
    }
    if (detail::has_json_value(json_obj, "icons")) {
        impl.icons = json_obj.at("icons").get<std::vector<Icon>>();
    }
}

void to_json(nlohmann::json& j, const ClientCapabilities::ElicitationCapability& cap) {
    j = nlohmann::json::object();
    if (cap.form) {
        j["form"] = *cap.form;
    }
    if (cap.url) {
        j["url"] = *cap.url;
    }
}

void from_json(const nlohmann::json& j, ClientCapabilities::ElicitationCapability& cap) {
    cap = ClientCapabilities::ElicitationCapability{};
    if (detail::has_json_value(j, "form")) {
        cap.form = j.at("form");
    }
    if (detail::has_json_value(j, "url")) {
        cap.url = j.at("url");
    }
}

void to_json(nlohmann::json& j, const ClientCapabilities::RootsCapability& cap) {
    j = nlohmann::json::object();
    if (cap.listChanged) {
        j["listChanged"] = *cap.listChanged;
    }
}

void from_json(const nlohmann::json& j, ClientCapabilities::RootsCapability& cap) {
    cap = ClientCapabilities::RootsCapability{};
    if (detail::has_json_value(j, "listChanged")) {
        cap.listChanged = j.at("listChanged").get<bool>();
    }
}

void to_json(nlohmann::json& j, const ClientCapabilities::SamplingCapability& cap) {
    j = nlohmann::json::object();
    if (cap.context) {
        j["context"] = *cap.context;
    }
    if (cap.tools) {
        j["tools"] = *cap.tools;
    }
}

void from_json(const nlohmann::json& j, ClientCapabilities::SamplingCapability& cap) {
    cap = ClientCapabilities::SamplingCapability{};
    if (detail::has_json_value(j, "context")) {
        cap.context = j.at("context");
    }
    if (detail::has_json_value(j, "tools")) {
        cap.tools = j.at("tools");
    }
}

void to_json(nlohmann::json& j, const ClientCapabilities& cap) {
    j = nlohmann::json::object();
    if (cap.elicitation) {
        j["elicitation"] = *cap.elicitation;
    }
    if (cap.experimental) {
        j["experimental"] = *cap.experimental;
    }
    if (cap.roots) {
        j["roots"] = *cap.roots;
    }
    if (cap.sampling) {
        j["sampling"] = *cap.sampling;
    }
    if (cap.extensions) {
        j["extensions"] = *cap.extensions;
    }
}

void from_json(const nlohmann::json& j, ClientCapabilities& cap) {
    cap = ClientCapabilities{};
    if (detail::has_json_value(j, "elicitation")) {
        cap.elicitation = j.at("elicitation").get<ClientCapabilities::ElicitationCapability>();
    }
    if (detail::has_json_value(j, "experimental")) {
        cap.experimental = j.at("experimental");
    }
    if (detail::has_json_value(j, "roots")) {
        cap.roots = j.at("roots").get<ClientCapabilities::RootsCapability>();
    }
    if (detail::has_json_value(j, "sampling")) {
        cap.sampling = j.at("sampling").get<ClientCapabilities::SamplingCapability>();
    }
    if (detail::has_json_value(j, "extensions")) {
        cap.extensions = j.at("extensions").get<std::map<std::string, nlohmann::json>>();
    }
}

void to_json(nlohmann::json& j, const ServerCapabilities::PromptsCapability& cap) {
    j = nlohmann::json::object();
    if (cap.listChanged) {
        j["listChanged"] = *cap.listChanged;
    }
}

void from_json(const nlohmann::json& j, ServerCapabilities::PromptsCapability& cap) {
    cap = ServerCapabilities::PromptsCapability{};
    if (detail::has_json_value(j, "listChanged")) {
        cap.listChanged = j.at("listChanged").get<bool>();
    }
}

void to_json(nlohmann::json& j, const ServerCapabilities::ResourcesCapability& cap) {
    j = nlohmann::json::object();
    if (cap.listChanged) {
        j["listChanged"] = *cap.listChanged;
    }
    if (cap.subscribe) {
        j["subscribe"] = *cap.subscribe;
    }
}

void from_json(const nlohmann::json& j, ServerCapabilities::ResourcesCapability& cap) {
    cap = ServerCapabilities::ResourcesCapability{};
    if (detail::has_json_value(j, "listChanged")) {
        cap.listChanged = j.at("listChanged").get<bool>();
    }
    if (detail::has_json_value(j, "subscribe")) {
        cap.subscribe = j.at("subscribe").get<bool>();
    }
}

void to_json(nlohmann::json& j, const ServerCapabilities::ToolsCapability& cap) {
    j = nlohmann::json::object();
    if (cap.listChanged) {
        j["listChanged"] = *cap.listChanged;
    }
}

void from_json(const nlohmann::json& j, ServerCapabilities::ToolsCapability& cap) {
    cap = ServerCapabilities::ToolsCapability{};
    if (detail::has_json_value(j, "listChanged")) {
        cap.listChanged = j.at("listChanged").get<bool>();
    }
}

void to_json(nlohmann::json& j, const ServerCapabilities& cap) {
    j = nlohmann::json::object();
    if (cap.completions) {
        j["completions"] = *cap.completions;
    }
    if (cap.experimental) {
        j["experimental"] = *cap.experimental;
    }
    if (cap.logging) {
        j["logging"] = *cap.logging;
    }
    if (cap.prompts) {
        j["prompts"] = *cap.prompts;
    }
    if (cap.resources) {
        j["resources"] = *cap.resources;
    }
    if (cap.tools) {
        j["tools"] = *cap.tools;
    }
    if (cap.extensions) {
        j["extensions"] = *cap.extensions;
    }
}

void from_json(const nlohmann::json& j, ServerCapabilities& cap) {
    cap = ServerCapabilities{};
    if (detail::has_json_value(j, "completions")) {
        cap.completions = j.at("completions");
    }
    if (detail::has_json_value(j, "experimental")) {
        cap.experimental = j.at("experimental");
    }
    if (detail::has_json_value(j, "logging")) {
        cap.logging = j.at("logging");
    }
    if (detail::has_json_value(j, "prompts")) {
        cap.prompts = j.at("prompts").get<ServerCapabilities::PromptsCapability>();
    }
    if (detail::has_json_value(j, "resources")) {
        cap.resources = j.at("resources").get<ServerCapabilities::ResourcesCapability>();
    }
    if (detail::has_json_value(j, "tools")) {
        cap.tools = j.at("tools").get<ServerCapabilities::ToolsCapability>();
    }
    if (detail::has_json_value(j, "extensions")) {
        cap.extensions = j.at("extensions").get<std::map<std::string, nlohmann::json>>();
    }
}

void to_json(nlohmann::json& json_obj, const PingRequest& value) { json_obj["method"] = value.method; }

void from_json(const nlohmann::json& json_obj, PingRequest& value) {
    json_obj.at("method").get_to(value.method);
}

void to_json(nlohmann::json& json_obj, const InitializeRequest& value) {
    json_obj["protocolVersion"] = value.protocolVersion;
    json_obj["clientInfo"] = value.clientInfo;
    json_obj["capabilities"] = value.capabilities;
}

void from_json(const nlohmann::json& json_obj, InitializeRequest& value) {
    json_obj.at("protocolVersion").get_to(value.protocolVersion);
    json_obj.at("clientInfo").get_to(value.clientInfo);
    json_obj.at("capabilities").get_to(value.capabilities);
}

void to_json(nlohmann::json& json_obj, const InitializeResult& res) {
    json_obj = nlohmann::json{{"protocolVersion", res.protocolVersion},
                              {"capabilities", res.capabilities},
                              {"serverInfo", res.serverInfo}};
    if (res.instructions) {
        json_obj["instructions"] = *res.instructions;
    }
}

void from_json(const nlohmann::json& json_obj, InitializeResult& res) {
    res = InitializeResult{};
    json_obj.at("protocolVersion").get_to(res.protocolVersion);
    json_obj.at("capabilities").get_to(res.capabilities);
    json_obj.at("serverInfo").get_to(res.serverInfo);
    if (detail::has_json_value(json_obj, "instructions")) {
        res.instructions = json_obj.at("instructions").get<std::string>();
    }
}

void to_json(nlohmann::json& json_obj, const DiscoverRequest& req) {
    json_obj = nlohmann::json::object();
    if (req.meta) {
        json_obj["_meta"] = *req.meta;
    }
}

void from_json(const nlohmann::json& json_obj, DiscoverRequest& req) {
    req = DiscoverRequest{};
    if (detail::has_json_value(json_obj, "_meta")) {
        req.meta = json_obj.at("_meta");
    }
}

void to_json(nlohmann::json& json_obj, const DiscoverResult& res) {
    json_obj = nlohmann::json{
        {"resultType", res.resultType},
        {"supportedVersions", res.supportedVersions},
        {"capabilities", res.capabilities},
    };
    if (res.serverInfo) {
        json_obj["_meta"] = {{"io.modelcontextprotocol/serverInfo", *res.serverInfo}};
    }
    if (res.instructions) {
        json_obj["instructions"] = *res.instructions;
    }
    detail::write_cache_hints(json_obj, res.ttlMs, res.cacheScope);
}

void from_json(const nlohmann::json& json_obj, DiscoverResult& res) {
    res = DiscoverResult{};
    json_obj.at("resultType").get_to(res.resultType);
    json_obj.at("supportedVersions").get_to(res.supportedVersions);
    json_obj.at("capabilities").get_to(res.capabilities);
    if (detail::has_json_value(json_obj, "_meta") &&
        detail::has_json_value(json_obj.at("_meta"), "io.modelcontextprotocol/serverInfo")) {
        res.serverInfo =
            json_obj.at("_meta").at("io.modelcontextprotocol/serverInfo").get<ServerInfo>();
    }
    if (detail::has_json_value(json_obj, "instructions")) {
        res.instructions = json_obj.at("instructions").get<std::string>();
    }
    detail::read_cache_hints(json_obj, res.ttlMs, res.cacheScope);
}

}  // namespace mcp

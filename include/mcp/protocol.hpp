#pragma once

#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace mcp {

constexpr std::string_view PROTOCOL_VERSION_2024_11_05 = "2024-11-05";
constexpr std::string_view PROTOCOL_VERSION_2025_03_26 = "2025-03-26";
constexpr std::string_view PROTOCOL_VERSION_2025_06_18 = "2025-06-18";
constexpr std::string_view LATEST_PROTOCOL_VERSION = PROTOCOL_VERSION_2025_06_18;

constexpr std::array<std::string_view, 3> SUPPORTED_PROTOCOL_VERSIONS = {
    PROTOCOL_VERSION_2024_11_05, PROTOCOL_VERSION_2025_03_26, PROTOCOL_VERSION_2025_06_18};

struct Implementation {
    std::string name;
    std::string version;
    std::optional<std::string> title;
    std::optional<std::string> websiteUrl;
};

inline void to_json(nlohmann::json& json_obj, const Implementation& impl) {
    json_obj = nlohmann::json{{"name", impl.name}, {"version", impl.version}};
    if (impl.title) {
        json_obj["title"] = *impl.title;
    }
    if (impl.websiteUrl) {
        json_obj["websiteUrl"] = *impl.websiteUrl;
    }
}

inline void from_json(const nlohmann::json& json_obj, Implementation& impl) {
    json_obj.at("name").get_to(impl.name);
    json_obj.at("version").get_to(impl.version);
    if (json_obj.contains("title")) {
        impl.title = json_obj.at("title").get<std::string>();
    }
    if (json_obj.contains("websiteUrl")) {
        impl.websiteUrl = json_obj.at("websiteUrl").get<std::string>();
    }
}

using ClientInfo = Implementation;
using ServerInfo = Implementation;

struct InitializeRequest {
    std::string protocolVersion;
    ClientInfo clientInfo;
    nlohmann::json capabilities;
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(InitializeRequest, protocolVersion, clientInfo, capabilities)

struct InitializeResult {
    std::string protocolVersion;
    nlohmann::json capabilities;
    ServerInfo serverInfo;
    std::optional<std::string> instructions;
};

inline void to_json(nlohmann::json& json_obj, const InitializeResult& res) {
    json_obj = nlohmann::json{{"protocolVersion", res.protocolVersion},
                              {"capabilities", res.capabilities},
                              {"serverInfo", res.serverInfo}};
    if (res.instructions) {
        json_obj["instructions"] = *res.instructions;
    }
}

inline void from_json(const nlohmann::json& json_obj, InitializeResult& res) {
    json_obj.at("protocolVersion").get_to(res.protocolVersion);
    json_obj.at("capabilities").get_to(res.capabilities);
    json_obj.at("serverInfo").get_to(res.serverInfo);
    if (json_obj.contains("instructions")) {
        res.instructions = json_obj.at("instructions").get<std::string>();
    }
}

}  // namespace mcp

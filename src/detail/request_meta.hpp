#pragma once

#include <mcp/protocol/request_meta.hpp>

#include <array>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <utility>

namespace mcp::detail {

/// The `_meta` object of a request's params, or null when there is none.
inline const nlohmann::json* request_meta_object(const nlohmann::json& message) {
    const auto params = message.find("params");
    if (params == message.end() || !params->is_object()) {
        return nullptr;
    }
    const auto meta = params->find("_meta");
    if (meta == params->end() || !meta->is_object()) {
        return nullptr;
    }
    return &*meta;
}

/// Whether a request uses the per-request protocol of 2026-07-28 and later: its `_meta` names a
/// protocol version. initialize is always a request of an earlier revision, whatever it carries.
inline bool is_modern_request(const nlohmann::json& message, std::string_view method) {
    if (method == "initialize") {
        return false;
    }
    const auto* meta = request_meta_object(message);
    return meta != nullptr && meta->contains(g_META_PROTOCOL_VERSION);
}

/// The protocol version a modern request names, or null when the value is not a string.
inline const std::string* requested_protocol_version(const nlohmann::json& message) {
    const auto* meta = request_meta_object(message);
    if (meta == nullptr) {
        return nullptr;
    }
    const auto version = meta->find(g_META_PROTOCOL_VERSION);
    if (version == meta->end() || !version->is_string()) {
        return nullptr;
    }
    return &version->get_ref<const std::string&>();
}

/// Reads a logging level by its spec name. The enum's JSON conversion maps an unknown name to the
/// first level instead of failing, so names are matched here.
inline bool parse_logging_level(const nlohmann::json& value, LoggingLevel& level) {
    static constexpr std::array<std::pair<std::string_view, LoggingLevel>, 8> g_levels{{
        {"emergency", LoggingLevel::eEmergency},
        {"alert", LoggingLevel::eAlert},
        {"critical", LoggingLevel::eCritical},
        {"error", LoggingLevel::eError},
        {"warning", LoggingLevel::eWarning},
        {"notice", LoggingLevel::eNotice},
        {"info", LoggingLevel::eInfo},
        {"debug", LoggingLevel::eDebug},
    }};
    if (!value.is_string()) {
        return false;
    }
    const auto& name = value.get_ref<const std::string&>();
    for (const auto& [level_name, level_value] : g_levels) {
        if (name == level_name) {
            level = level_value;
            return true;
        }
    }
    return false;
}

struct RequestMetaParse {
    std::shared_ptr<const RequestMetadata> metadata;
    /// Why the fields are malformed; empty when `metadata` is set.
    std::string error;
};

/// Reads the per-request protocol fields of a request that is_modern_request() accepted and whose
/// version requested_protocol_version() returned.
inline RequestMetaParse parse_request_meta(const nlohmann::json& message) {
    const auto& meta = *request_meta_object(message);
    auto metadata = std::make_shared<RequestMetadata>();
    metadata->protocolVersion = meta.at(std::string(g_META_PROTOCOL_VERSION)).get<std::string>();
    metadata->meta = meta;

    const auto capabilities = meta.find(g_META_CLIENT_CAPABILITIES);
    if (capabilities == meta.end()) {
        return {nullptr, "Missing " + std::string(g_META_CLIENT_CAPABILITIES) + " in _meta"};
    }
    try {
        if (!capabilities->is_object()) {
            throw std::invalid_argument("not an object");
        }
        metadata->clientCapabilities = capabilities->get<ClientCapabilities>();
    } catch (const std::exception&) {
        return {nullptr, "Invalid " + std::string(g_META_CLIENT_CAPABILITIES) + " in _meta"};
    }

    if (const auto client_info = meta.find(g_META_CLIENT_INFO);
        client_info != meta.end() && !client_info->is_null()) {
        try {
            if (!client_info->is_object()) {
                throw std::invalid_argument("not an object");
            }
            metadata->clientInfo = client_info->get<Implementation>();
        } catch (const std::exception&) {
            return {nullptr, "Invalid " + std::string(g_META_CLIENT_INFO) + " in _meta"};
        }
    }

    if (const auto log_level = meta.find(g_META_LOG_LEVEL);
        log_level != meta.end() && !log_level->is_null()) {
        LoggingLevel level{};
        if (!parse_logging_level(*log_level, level)) {
            return {nullptr, "Invalid " + std::string(g_META_LOG_LEVEL) + " in _meta"};
        }
        metadata->logLevel = level;
    }

    return {std::move(metadata), {}};
}

}  // namespace mcp::detail

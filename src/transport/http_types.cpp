#include <mcp/transport/http_types.hpp>

#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace mcp {

namespace {

constexpr std::string_view g_well_known_prefix = "/.well-known/oauth-protected-resource";

/// @brief The origin and path components of an absolute resource URL.
struct SplitResource {
    std::string_view origin;  ///< Scheme and authority, with no trailing slash.
    std::string_view path;    ///< Everything after the authority, empty when there is none.
};

/// @brief Split an absolute resource URL, rejecting anything without a scheme and a host.
SplitResource split_resource(std::string_view resource) {
    constexpr std::string_view scheme_separator = "://";
    const auto scheme_end = resource.find(scheme_separator);
    if (scheme_end == std::string_view::npos) {
        throw std::invalid_argument("Protected-resource metadata resource must be an absolute URL");
    }

    const auto authority_start = scheme_end + scheme_separator.size();
    // A query or fragment is not part of a resource identifier; drop it before the split so it
    // cannot end up spliced into the metadata path.
    const auto trimmed = resource.substr(0, resource.find_first_of("?#", authority_start));
    const auto authority_end = trimmed.find('/', authority_start);
    if (authority_end == authority_start || trimmed.size() == authority_start) {
        throw std::invalid_argument("Protected-resource metadata resource must name a host");
    }

    if (authority_end == std::string_view::npos) {
        return SplitResource{trimmed, {}};
    }
    return SplitResource{trimmed.substr(0, authority_end), trimmed.substr(authority_end)};
}

/// @brief True when every byte may appear in an RFC 7235 quoted-string, escaped or not.
bool is_quotable(std::string_view value) {
    for (const auto character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte != '\t' && (byte < 0x20 || byte > 0x7E)) {
            return false;
        }
    }
    return true;
}

/// @brief Append `key="value"` with `\` and `"` escaped, separating it from any earlier parameter.
void append_challenge_parameter(std::string& header, bool& first, std::string_view key,
                                std::string_view value) {
    header += first ? " " : ", ";
    first = false;
    header += key;
    header += "=\"";
    for (const auto character : value) {
        if (character == '\\' || character == '"') {
            header += '\\';
        }
        header += character;
    }
    header += '"';
}

}  // namespace

std::string_view http_bearer_token(std::string_view authorization_header) {
    constexpr std::string_view scheme = "Bearer ";
    if (authorization_header.size() <= scheme.size()) {
        return {};
    }

    for (std::size_t index = 0; index < scheme.size(); ++index) {
        auto actual = authorization_header[index];
        auto expected = scheme[index];
        if (actual >= 'A' && actual <= 'Z') {
            actual = static_cast<char>(actual - 'A' + 'a');
        }
        if (expected >= 'A' && expected <= 'Z') {
            expected = static_cast<char>(expected - 'A' + 'a');
        }
        if (actual != expected) {
            return {};
        }
    }
    return authorization_header.substr(scheme.size());
}

std::string_view http_request_path(std::string_view target) {
    return target.substr(0, target.find_first_of("?#"));
}

std::string format_www_authenticate(const BearerChallengeConfig& config) {
    struct Parameter {
        std::string_view key;
        std::string_view value;
    };
    const Parameter parameters[] = {
        {"realm", config.realm},
        {"error", config.error},
        {"scope", config.scope},
        {"resource_metadata", config.resource_metadata},
    };

    for (const auto& [key, value] : parameters) {
        if (!is_quotable(value)) {
            throw std::invalid_argument("WWW-Authenticate " + std::string(key) +
                                        " contains a character that cannot be quoted");
        }
    }

    std::string header = "Bearer";
    bool first = true;
    for (const auto& [key, value] : parameters) {
        if (!value.empty()) {
            append_challenge_parameter(header, first, key, value);
        }
    }
    return header;
}

std::string format_protected_resource_metadata(const ProtectedResourceMetadataConfig& metadata) {
    nlohmann::json document;
    document["resource"] = metadata.resource;
    if (!metadata.authorization_servers.empty()) {
        document["authorization_servers"] = metadata.authorization_servers;
    }
    if (!metadata.scopes_supported.empty()) {
        document["scopes_supported"] = metadata.scopes_supported;
    }
    return document.dump();
}

std::string protected_resource_metadata_path(const ProtectedResourceMetadataConfig& metadata) {
    // Split even when the caller supplied a path, so an unusable resource is reported the same
    // way either way.
    const auto resource = split_resource(metadata.resource);
    if (!metadata.path.empty()) {
        return metadata.path;
    }

    // RFC 9728 3.1 inserts the well-known segment between the authority and the resource's own
    // path, so a resource at https://host/mcp is described at
    // https://host/.well-known/oauth-protected-resource/mcp.
    auto resource_path = resource.path;
    while (!resource_path.empty() && resource_path.back() == '/') {
        resource_path.remove_suffix(1);
    }
    return std::string(g_well_known_prefix) + std::string(resource_path);
}

std::string protected_resource_metadata_url(const ProtectedResourceMetadataConfig& metadata) {
    return std::string(split_resource(metadata.resource).origin) +
           protected_resource_metadata_path(metadata);
}

}  // namespace mcp

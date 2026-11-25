#include "authorization_common.hpp"

#include <mcp/auth/metadata_policy.hpp>

#include <algorithm>
#include <utility>

namespace mcp::auth::detail {

bool resource_identifies_server(const std::string& resource, const std::string& server_url) {
    if (resource == server_url) {
        return true;
    }
    if (resource.find('?') != std::string::npos || resource.find('#') != std::string::npos) {
        return false;
    }
    const auto resource_origin = metadata_url_origin(resource);
    const auto server_origin = metadata_url_origin(server_url);
    if (resource_origin.empty() || resource_origin != server_origin) {
        return false;
    }
    auto resource_path = resource.substr(resource_origin.size());
    if (resource_path.empty() || resource_path == "/") {
        return true;
    }
    if (resource_path.back() == '/') {
        resource_path.pop_back();
    }
    const auto server_path = server_url.substr(server_origin.size());
    return server_path == resource_path ||
           (server_path.size() > resource_path.size() && server_path.starts_with(resource_path) &&
            server_path[resource_path.size()] == '/');
}

std::vector<std::string> split_scopes(std::string_view scopes) {
    std::vector<std::string> split;
    std::size_t cursor = 0;
    while (cursor < scopes.size()) {
        const auto next = scopes.find(' ', cursor);
        const auto token = scopes.substr(cursor, next == std::string_view::npos ? next : next - cursor);
        if (!token.empty()) {
            split.emplace_back(token);
        }
        if (next == std::string_view::npos) {
            break;
        }
        cursor = next + 1;
    }
    return split;
}

std::string join_scopes(const std::vector<std::string>& scopes) {
    std::string joined;
    for (const auto& scope : scopes) {
        if (!joined.empty()) {
            joined.push_back(' ');
        }
        joined += scope;
    }
    return joined;
}

std::optional<std::string> union_scopes(const std::optional<std::string>& primary,
                                        const std::optional<std::string>& secondary) {
    auto merged = split_scopes(primary.value_or(""));
    for (auto& scope : split_scopes(secondary.value_or(""))) {
        if (std::ranges::find(merged, scope) == merged.end()) {
            merged.push_back(std::move(scope));
        }
    }
    return merged.empty() ? std::nullopt : std::optional<std::string>(join_scopes(merged));
}

}  // namespace mcp::auth::detail

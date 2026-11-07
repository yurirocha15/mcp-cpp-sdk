#pragma once

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace mcp::detail {

/// The client requests a server answers for a request of protocol revision 2026-07-28. Methods of
/// earlier revisions that 2026-07-28 removed (ping, logging/setLevel, resources/subscribe,
/// resources/unsubscribe) are not among them, and neither is initialize, which always starts a
/// session of an earlier revision.
inline constexpr std::array<std::string_view, 10> g_MODERN_METHODS = {
    "server/discover",          "tools/list",          "tools/call",  "resources/list",
    "resources/read",           "prompts/list",        "prompts/get", "completion/complete",
    "resources/templates/list", "subscriptions/listen"};

inline bool is_modern_method(std::string_view method) {
    return std::find(g_MODERN_METHODS.begin(), g_MODERN_METHODS.end(), method) !=
           g_MODERN_METHODS.end();
}

/// The message of the g_METHOD_NOT_FOUND that answers an initialize no legacy version can serve,
/// naming the versions that are served.
inline std::string initialize_not_served_message(const std::vector<std::string>& served) {
    std::string message = "Method not found: initialize; this server serves protocol versions";
    for (const auto& version : served) {
        message += ' ';
        message += version;
    }
    return message;
}

}  // namespace mcp::detail

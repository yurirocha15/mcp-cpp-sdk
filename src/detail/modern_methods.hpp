#pragma once

#include <algorithm>
#include <array>
#include <string_view>

namespace mcp::detail {

/// The client requests a server answers for a request of protocol revision 2026-07-28. Methods of
/// earlier revisions that 2026-07-28 removed (ping, logging/setLevel, resources/subscribe,
/// resources/unsubscribe) are not among them, and neither is initialize, which always starts a
/// session of an earlier revision.
inline constexpr std::array<std::string_view, 9> g_MODERN_METHODS = {
    "server/discover",          "tools/list",   "tools/call",  "resources/list",     "resources/read",
    "resources/templates/list", "prompts/list", "prompts/get", "completion/complete"};

inline bool is_modern_method(std::string_view method) {
    return std::find(g_MODERN_METHODS.begin(), g_MODERN_METHODS.end(), method) !=
           g_MODERN_METHODS.end();
}

}  // namespace mcp::detail

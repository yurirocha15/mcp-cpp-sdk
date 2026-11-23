#pragma once

#include <algorithm>
#include <array>
#include <mcp/core/constants.hpp>
#include <string>
#include <string_view>
#include <vector>
#include "mcp/detail/constants.hpp"

namespace mcp::detail {

inline bool is_modern_method(std::string_view method) {
    return std::find(mcp::detail::constants::modern_methods::g_NAMES.begin(),
                     mcp::detail::constants::modern_methods::g_NAMES.end(),
                     method) != mcp::detail::constants::modern_methods::g_NAMES.end();
}

/// The message of the mcp::constants::jsonrpc::g_METHOD_NOT_FOUND that answers an initialize no legacy
/// version can serve, naming the versions that are served.
inline std::string initialize_not_served_message(const std::vector<std::string>& served) {
    std::string message = "Method not found: initialize; this server serves protocol versions";
    for (const auto& version : served) {
        message += ' ';
        message += version;
    }
    return message;
}

}  // namespace mcp::detail

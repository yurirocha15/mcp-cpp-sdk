#pragma once

#include <mcp/auth/metadata_policy.hpp>

#include <string>
#include <string_view>

namespace mcp::detail {

/// Flattens and bounds peer-controlled text for a diagnostic, for throw sites outside `src/auth/`.
///
/// The implementation lives in `mcp::auth::detail`. Its declaration in
/// `include/mcp/auth/metadata_policy.hpp` is `MCP_API`-exported, so it cannot be moved or renamed
/// without breaking callers.
///
/// This header exists so `src/client/` and `src/server/` do not each grow their own
/// `#include <mcp/auth/...>` for their only dependency on the auth headers.
///
/// A plain forwarder: see the declaration in `mcp/auth/metadata_policy.hpp` for exactly what
/// "flatten" and "bound" mean.
[[nodiscard]] inline std::string sanitize_for_diagnostics(std::string_view value) {
    return auth::detail::sanitize_for_diagnostics(value);
}

}  // namespace mcp::detail

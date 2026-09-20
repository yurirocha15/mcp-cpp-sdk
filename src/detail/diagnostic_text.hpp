#pragma once

#include <mcp/auth/metadata_policy.hpp>

#include <string>
#include <string_view>

namespace mcp::detail {

/// Flattens and bounds peer-controlled text for a diagnostic, for throw sites outside `src/auth/`.
///
/// The implementation lives in `mcp::auth::detail` because that is where the need was first found.
/// Its declaration in `include/mcp/auth/metadata_policy.hpp` is `MCP_API`-exported, so it cannot be
/// moved or renamed without breaking callers, and it stays exactly where it is.
///
/// This header exists so that `src/client/` and `src/server/` do not each grow their own
/// `#include <mcp/auth/...>`. They have no other dependency on the auth headers, so one file naming
/// that dependency and saying why is one file to change if the utility is ever given a home of its
/// own. Everything compiles into a single library target, so this is a source-level seam, not a
/// build-graph one.
///
/// A plain forwarder: see the declaration in `mcp/auth/metadata_policy.hpp` for exactly what
/// "flatten" and "bound" mean.
[[nodiscard]] inline std::string sanitize_for_diagnostics(std::string_view value) {
    return auth::detail::sanitize_for_diagnostics(value);
}

}  // namespace mcp::detail

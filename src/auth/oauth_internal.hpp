/**
 * @file oauth_internal.hpp
 * @brief Reach-in accessors for this SDK's own tests. NOT part of the shipped interface.
 *
 * This header lives under `src/` and is not installed, so nothing declared here reaches a consumer of
 * the SDK. The symbols are exported because the test binary links the shared library, which is built
 * with hidden visibility. Nothing in the library itself calls these.
 */
#pragma once

#include <mcp/auth/oauth.hpp>
#include <mcp/core/export.hpp>

#include <cstddef>

namespace mcp::auth::internal {

/// How many per-scope abort records `client` is still holding on its own account.
///
/// A client that remembers which scopes were aborted keeps one record per closed scope forever;
/// a client that keeps each latch on the scope itself has nothing to report and answers zero
/// however many scopes have come and gone.
MCP_API std::size_t retained_scope_record_count(const OAuthHttpClient& client);

/// How many per-scope abort latches exist right now, across every client in this process.
///
/// Counts latch objects, not references to them and not scopes. An in-flight exchange holds a
/// `shared_ptr` copy of an existing latch, so the count does NOT rise during a request, and a latch
/// outlives its scope while an exchange it issued is still running. Intended for leak checks: a count
/// that does not return to its starting value once the scopes are gone means something outlived its
/// scope. For a number that rises while a request is in flight, use `use_count()` on the shared
/// state.
MCP_API std::size_t live_scope_latch_count();

}  // namespace mcp::auth::internal

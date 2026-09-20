/**
 * @file oauth_internal.hpp
 * @brief Reach-in accessors for this SDK's own tests. NOT part of the shipped interface.
 *
 * This header lives under `src/` and is deliberately not installed: `install(DIRECTORY include/mcp)`
 * ships the public tree only, so nothing declared here reaches a consumer of the SDK. The symbols
 * are exported because the test binary links the shared library, which is built with hidden
 * visibility -- exported, but undeclared anywhere a consumer can include.
 *
 * Nothing in the library itself calls these. They exist so a test can assert on retained state
 * directly instead of on a stand-in that merely correlates with it.
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
/// This counts latch OBJECTS, not references to them. It is incremented in
/// `OAuthScopeState`'s constructor and decremented in its destructor, so it equals the number of
/// live scopes. An exchange in flight holds a `shared_ptr` copy of an existing object and adds
/// nothing to it: the count does not rise during a request. Measured, not assumed -- an earlier
/// version of this comment claimed the opposite and two reviewers believed it.
///
/// What the count is good for is the leak check. A count that does not return to its starting
/// value after the scopes are destroyed means something outlived the scope it belongs to -- a
/// capture that escaped, or a reference cycle. For a number that does rise while a request is in
/// flight, report `use_count()` on the shared state instead.
MCP_API std::size_t live_scope_latch_count();

}  // namespace mcp::auth::internal

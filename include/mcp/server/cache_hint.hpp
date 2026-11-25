#pragma once

#include <mcp/protocol/capabilities.hpp>

#include <cstdint>

namespace mcp {

/**
 * @brief A result whose caching hints Server::set_cache_hints() sets.
 *
 * @details Protocol revision 2026-07-28 requires `ttlMs` and `cacheScope` on each of these results.
 */
enum class CacheTarget : std::uint8_t {
    eDiscover,               ///< server/discover.
    eToolsList,              ///< tools/list.
    ePromptsList,            ///< prompts/list.
    eResourcesList,          ///< resources/list.
    eResourceTemplatesList,  ///< resources/templates/list.
    eResourcesRead,          ///< resources/read.
};

/**
 * @brief The caching hints a server attaches to a cacheable result of a 2026-07-28 request.
 *
 * @details The defaults tell a client not to reuse the result (`ttlMs` 0) and, should it cache it
 * anyway, not to share it across authorization contexts (`private`).
 */
struct CacheHint {
    /// How long, in milliseconds, a client MAY consider the result fresh. 0 means do not reuse it.
    std::int64_t ttlMs = 0;
    /// Who may share a cached copy: only the same authorization context, or anyone.
    CacheScope scope = CacheScope::ePrivate;
};

}  // namespace mcp

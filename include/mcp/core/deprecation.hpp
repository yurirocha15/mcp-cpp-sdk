#pragma once

#include <mcp/core/export.hpp>

#include <cstdint>
#include <functional>
#include <string_view>

namespace mcp {

/**
 * @brief A protocol feature that a revision deprecates and the SDK still implements, because peers
 *        of earlier revisions use it.
 */
enum class DeprecatedFeature : std::uint8_t {
    eLogging,         ///< Logging (SEP-2577).
    eSampling,        ///< Sampling (SEP-2577).
    eRoots,           ///< Roots (SEP-2577).
    eIncludeContext,  ///< The `includeContext` values `thisServer` and `allServers` (SEP-2596).
    eDynamicClientRegistration,  ///< Dynamic client registration (RFC 7591; PR #2858).
};

/**
 * @brief What the SDK reports the first time an object uses a deprecated feature.
 *
 * @details Every field refers to static text, so a handler may keep the notice.
 */
struct DeprecationNotice {
    DeprecatedFeature feature{};        ///< The feature used.
    std::string_view name;              ///< Its name, such as "Logging".
    std::string_view proposal;          ///< The proposal that deprecates it, such as "SEP-2577".
    std::string_view deprecated_in;     ///< The protocol revision that deprecates it.
    std::string_view earliest_removal;  ///< When the specification may remove it, at the earliest.
    std::string_view migration;         ///< What the specification names instead.
};

/**
 * @brief Receives a deprecation notice.
 *
 * @details It is called at most once per feature for each `Server`, `Client` or
 * `OAuthAuthorizationManager`, on the thread that used the feature, so it should return quickly.
 */
using DeprecationHandler = std::function<void(const DeprecationNotice&)>;

/**
 * @brief The notice the SDK reports for @p feature.
 *
 * @param feature A deprecated feature.
 * @return The notice; its strings are static.
 */
[[nodiscard]] MCP_API const DeprecationNotice& deprecation_notice(DeprecatedFeature feature);

}  // namespace mcp

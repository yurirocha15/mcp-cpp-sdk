#pragma once

#include <mcp/core/deprecation.hpp>

#include <array>
#include <cstddef>
#include <mutex>
#include <utility>

namespace mcp::detail {

/**
 * @brief Reports each deprecated feature once for the object that owns it.
 *
 * @details Without a handler the notice goes to standard error, unless the environment variable
 * MCP_SILENCE_DEPRECATION_NOTICES is set to a non-empty value.
 */
class DeprecationReporter {
   public:
    explicit DeprecationReporter(DeprecationHandler handler = {}) : handler_(std::move(handler)) {}

    void set_handler(DeprecationHandler handler);
    void report(DeprecatedFeature feature);

   private:
    std::mutex mutex_;
    DeprecationHandler handler_;
    std::array<bool, static_cast<std::size_t>(DeprecatedFeature::eDynamicClientRegistration) + 1>
        reported_{};
};

}  // namespace mcp::detail

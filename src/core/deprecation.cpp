#include <mcp/core/deprecation.hpp>

#include <mcp/detail/constants.hpp>
#include <mcp/detail/deprecation_reporter.hpp>

#include <array>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>

namespace mcp {
namespace {

namespace deprecation = detail::constants::deprecation;

bool notices_silenced() {
    const std::string name{deprecation::g_SILENCE_ENV};
    const char* value = std::getenv(name.c_str());
    return value != nullptr && *value != '\0';
}

}  // namespace

const DeprecationNotice& deprecation_notice(DeprecatedFeature feature) {
    static constexpr auto s_notices = std::to_array<DeprecationNotice>({
        {
            .feature = DeprecatedFeature::eLogging,
            .name = "Logging",
            .proposal = "SEP-2577",
            .deprecated_in = "2026-07-28",
            .earliest_removal = deprecation::g_REVISION_ON_OR_AFTER_2027_07_28,
            .migration = "Log to standard error on stdio; use OpenTelemetry for observability.",
        },
        {
            .feature = DeprecatedFeature::eSampling,
            .name = "Sampling",
            .proposal = "SEP-2577",
            .deprecated_in = "2026-07-28",
            .earliest_removal = deprecation::g_REVISION_ON_OR_AFTER_2027_07_28,
            .migration = "Integrate directly with LLM provider APIs.",
        },
        {
            .feature = DeprecatedFeature::eRoots,
            .name = "Roots",
            .proposal = "SEP-2577",
            .deprecated_in = "2026-07-28",
            .earliest_removal = deprecation::g_REVISION_ON_OR_AFTER_2027_07_28,
            .migration = "Pass directories or files through tool parameters, resource URIs or server "
                         "configuration.",
        },
        {
            .feature = DeprecatedFeature::eIncludeContext,
            .name = "includeContext other than \"none\"",
            .proposal = "SEP-2596",
            .deprecated_in = "2025-11-25",
            .earliest_removal = "with Sampling (SEP-2577)",
            .migration = "Omit includeContext or use \"none\".",
        },
        {
            .feature = DeprecatedFeature::eDynamicClientRegistration,
            .name = "Dynamic client registration",
            .proposal = "PR #2858",
            .deprecated_in = "2026-07-28",
            .earliest_removal = deprecation::g_REVISION_ON_OR_AFTER_2027_07_28,
            .migration =
                "Use a client ID metadata document (ClientIdentityConfig::client_metadata_url) "
                "or pre-registered credentials.",
        },
        {
            .feature = DeprecatedFeature::eHttpSse,
            .name = "HTTP+SSE transport",
            .proposal = "SEP-2596",
            .deprecated_in = "2025-03-26",
            .earliest_removal = "three months after SEP-2596 reaches Final",
            .migration = "Use Streamable HTTP.",
        },
    });
    static_assert(s_notices.size() == static_cast<std::size_t>(DeprecatedFeature::eHttpSse) + 1,
                  "one notice per DeprecatedFeature");
    return s_notices.at(static_cast<std::size_t>(feature));
}

namespace detail {

void DeprecationReporter::set_handler(DeprecationHandler handler) {
    std::scoped_lock lock(mutex_);
    handler_ = std::move(handler);
}

void DeprecationReporter::report(DeprecatedFeature feature) {
    DeprecationHandler handler;
    {
        std::scoped_lock lock(mutex_);
        auto& reported = reported_.at(static_cast<std::size_t>(feature));
        if (reported) {
            return;
        }
        reported = true;
        handler = handler_;
    }
    const auto& notice = deprecation_notice(feature);
    if (handler) {
        handler(notice);
        return;
    }
    if (notices_silenced()) {
        return;
    }
    std::cerr << "mcp-cpp-sdk: " << notice.proposal << ": " << notice.name
              << " is deprecated since protocol revision " << notice.deprecated_in
              << "; earliest removal: " << notice.earliest_removal << ". " << notice.migration << '\n';
}

}  // namespace detail
}  // namespace mcp

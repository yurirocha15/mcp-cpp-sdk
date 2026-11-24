#pragma once

#include <algorithm>
#include <mcp/transport/http_types.hpp>

#include <algorithm>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace mcp::detail {

/// What the scope check decided about one request.
struct ScopeCheck {
    enum class Outcome {
        eAllowed,
        eInsufficient,
        eFailed,
    };
    Outcome outcome{Outcome::eAllowed};
    /// The challenge of the `403`, set when @c outcome is @c eInsufficient.
    BearerChallengeConfig challenge;
};

/// Decides whether the token's scopes cover what the request needs. A provider that throws has
/// failed to decide, which is the server's failure and not the client's, and so is one that names
/// a scope the challenge header cannot carry.
///
/// @p required names the scopes the request needs and @p held what the validator reported for the
/// token (none reported means none held). The challenge keeps @p base's realm and metadata URL and
/// carries every required scope, not only the missing ones, so one authorization satisfies the
/// operation.
inline ScopeCheck check_request_scopes(const ScopeRequirementProvider& required,
                                       const std::optional<std::vector<std::string>>& held,
                                       const nlohmann::json& request_json,
                                       const BearerChallengeConfig& base) {
    if (!required || !request_json.contains("method") || !request_json.at("method").is_string()) {
        return {};
    }
    const auto method = request_json.at("method").get<std::string>();
    static const nlohmann::json s_no_params;
    const auto params_it = request_json.find("params");
    const ScopeRequestContext context{
        .method = method,
        .params = params_it == request_json.end() ? s_no_params : *params_it,
    };
    std::vector<std::string> needed;
    try {
        needed = required(context);
    } catch (...) {
        return {.outcome = ScopeCheck::Outcome::eFailed, .challenge = {}};
    }
    const bool satisfied = std::all_of(needed.begin(), needed.end(), [&held](const auto& scope) {
        return held && std::find(held->begin(), held->end(), scope) != held->end();
    });
    if (satisfied) {
        return {};
    }
    auto challenge = base;
    challenge.error = "insufficient_scope";
    challenge.scope.clear();
    for (const auto& scope : needed) {
        challenge.scope += (challenge.scope.empty() ? "" : " ") + scope;
    }
    try {
        // A scope that cannot be quoted in the header is a provider failure, not a client's.
        (void)format_www_authenticate(challenge);
    } catch (const std::invalid_argument&) {
        return {.outcome = ScopeCheck::Outcome::eFailed, .challenge = {}};
    }
    return {.outcome = ScopeCheck::Outcome::eInsufficient, .challenge = std::move(challenge)};
}

}  // namespace mcp::detail

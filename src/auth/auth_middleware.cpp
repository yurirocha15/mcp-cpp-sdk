#include <mcp/auth/auth_middleware.hpp>
#include <mcp/core/async.hpp>

#include "mcp/detail/protocol/middleware_rejection.hpp"

#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <utility>

// GCC 11 SSO Coroutine Safety -- see docs/contributing.rst "Known Issues".
// Strings and string-containing protocol values that cross a suspension point
// live in shared operation state rather than directly in coroutine frames.

namespace mcp::auth {

namespace {

struct MiddlewareInvocation {
    TokenValidator validator;
    mcp::Context* context;
    nlohmann::json params;
    TypeErasedHandler next;
    std::string token;
};

Task<nlohmann::json> invoke_auth_middleware(std::shared_ptr<MiddlewareInvocation> invocation) {
    // A tool call reports the refusal as a tool error result; other requests answer it as a
    // JSON-RPC error. See MiddlewareRejection.
    if (invocation->token.empty()) {
        throw mcp::detail::MiddlewareRejection("Unauthorized: missing Bearer token");
    }
    // Awaited into a local, never in the condition: see "GCC 12 co_await in a condition" in
    // docs/contributing.rst.
    const bool valid = co_await invocation->validator(invocation->token);
    if (!valid) {
        throw mcp::detail::MiddlewareRejection("Unauthorized: invalid Bearer token");
    }
    co_return co_await invocation->next(*invocation->context, invocation->params);
}

}  // namespace

Middleware make_auth_middleware(TokenValidator validator) {
    return [validator = std::move(validator)](mcp::Context& context, const nlohmann::json& params,
                                              TypeErasedHandler next) -> Task<nlohmann::json> {
        auto invocation = std::make_shared<MiddlewareInvocation>();
        invocation->validator = validator;
        invocation->context = &context;
        invocation->params = params;
        invocation->next = std::move(next);
        if (params.contains("_meta") && params["_meta"].contains("auth_token")) {
            invocation->token = params["_meta"]["auth_token"].get<std::string>();
        }
        return invoke_auth_middleware(std::move(invocation));
    };
}

std::string extract_bearer_token(std::string_view auth_header_value) {
    constexpr std::string_view prefix = "Bearer ";
    if (auth_header_value.size() > prefix.size() && auth_header_value.starts_with(prefix)) {
        return std::string(auth_header_value.substr(prefix.size()));
    }
    return {};
}

}  // namespace mcp::auth

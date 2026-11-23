#pragma once

#include <mcp/core/async_fwd.hpp>
#include <mcp/core/export.hpp>
#include <mcp/server/middleware.hpp>

#include <functional>
#include <string>
#include <string_view>

namespace mcp::auth {

/// @brief Legacy callback used to validate a bearer token embedded in request metadata.
using TokenValidator = std::function<Task<bool>(const std::string& token)>;

/**
 * @brief Create legacy middleware that validates bearer tokens in request metadata.
 *
 * For Streamable HTTP servers, prefer set_bearer_token_validator() on the HTTP transport or
 * session manager so authentication is enforced at the HTTP boundary.
 *
 * A missing or refused token is answered with a tool error result (`isError: true`) on
 * `tools/call`, and with a JSON-RPC `-32603` error on every other request the middleware wraps,
 * such as `resources/read` and `prompts/get`. The handler is not invoked.
 *
 * @param validator Async callback that returns true when the token is accepted.
 * @return Middleware enforcing presence and validity of `_meta.auth_token`.
 */
MCP_API Middleware make_auth_middleware(TokenValidator validator);

/**
 * @brief Extract a bearer token from an Authorization header value.
 *
 * @param auth_header_value Header value to parse.
 * @return The token value without the `Bearer ` prefix, or an empty string on mismatch.
 */
MCP_API std::string extract_bearer_token(std::string_view auth_header_value);

}  // namespace mcp::auth

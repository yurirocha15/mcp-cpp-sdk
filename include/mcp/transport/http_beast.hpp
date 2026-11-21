#pragma once

// The Boost.Beast message types of the HTTP transports, apart from <mcp/transport/http_types.hpp>
// so that the types an application configures a transport with need no Boost.

#include <mcp/core/export.hpp>

#include <boost/beast/http.hpp>

#include <functional>
#include <optional>

namespace mcp {

class StreamableHttpSessionManager;

/**
 * @brief Alias for a Boost.Beast HTTP request with a string body.
 */
using StringRequest = boost::beast::http::request<boost::beast::http::string_body>;

/**
 * @brief Alias for a Boost.Beast HTTP response with a string body.
 */
using StringResponse = boost::beast::http::response<boost::beast::http::string_body>;

/// @brief Handles an HTTP request that is not for MCP, such as a health check, or returns
/// std::nullopt to let the session manager handle it as MCP.
using CustomRequestHandler = std::function<std::optional<StringResponse>(const StringRequest&)>;

/**
 * @brief Set a custom request handler for non-MCP routes (e.g., health checks) on @p manager.
 *
 * @param manager The session manager whose requests @p handler sees first.
 * @param handler A function that receives the HTTP request and optionally returns
 *                a response. If it returns std::nullopt, the request is handled
 *                as MCP protocol.
 * @throws std::logic_error If listen() has already been called or the manager is closed.
 */
MCP_API void set_custom_request_handler(StreamableHttpSessionManager& manager,
                                        CustomRequestHandler handler);

}  // namespace mcp

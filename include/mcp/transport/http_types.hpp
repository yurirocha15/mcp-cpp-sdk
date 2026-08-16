#pragma once

#include <mcp/core/export.hpp>

#include <boost/beast/http.hpp>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mcp {

/**
 * @brief A generic list of string pairs, often used for query params or form fields.
 */
using KeyValuePairList = std::vector<std::pair<std::string, std::string>>;

/**
 * @brief A list of SSE events as (id, data) pairs.
 */
using SseEventList = KeyValuePairList;

/**
 * @brief Alias for a Boost.Beast HTTP request with a string body.
 */
using StringRequest = boost::beast::http::request<boost::beast::http::string_body>;

/**
 * @brief Alias for a Boost.Beast HTTP response with a string body.
 */
using StringResponse = boost::beast::http::response<boost::beast::http::string_body>;

/// @brief Synchronous validator invoked at the HTTP boundary for a bearer token.
using BearerTokenValidator = std::function<bool(std::string_view)>;

/**
 * @brief Extract a bearer token from an HTTP Authorization header.
 * @param authorization_header Complete Authorization header value.
 * @return The characters after the Bearer scheme, or an empty view when the
 *         scheme is absent or no characters follow it.
 */
MCP_API std::string_view http_bearer_token(std::string_view authorization_header);

}  // namespace mcp

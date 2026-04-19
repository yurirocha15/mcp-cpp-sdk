#pragma once

#include <boost/beast/http.hpp>
#include <string>
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

}  // namespace mcp

#pragma once

// The Boost.Beast message types of the HTTP transports, apart from <mcp/transport/http_types.hpp>
// so that the types an application configures a transport with need no Boost.

#include <boost/beast/http.hpp>

namespace mcp {

/**
 * @brief Alias for a Boost.Beast HTTP request with a string body.
 */
using StringRequest = boost::beast::http::request<boost::beast::http::string_body>;

/**
 * @brief Alias for a Boost.Beast HTTP response with a string body.
 */
using StringResponse = boost::beast::http::response<boost::beast::http::string_body>;

}  // namespace mcp

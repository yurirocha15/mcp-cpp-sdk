#pragma once

// Boost.Asio headers are intentionally kept here: `Task<T>` is a template
// alias for `boost::asio::awaitable<T>`, which appears in virtual method
// signatures across the transport interface hierarchy.  Because template
// aliases cannot be forward-declared, the full Boost type must be visible at
// every call site that uses `Task<T>`.
#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include <cstdint>
#include <functional>
#include <string_view>

namespace mcp {

/**
 * @brief Alias for boost::asio::awaitable, representing an asynchronous task.
 *
 * @tparam T The return type of the task.
 */
template <typename T>
using Task = boost::asio::awaitable<T>;

}  // namespace mcp

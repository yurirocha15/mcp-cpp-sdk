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

/**
 * @brief Enumeration of log levels.
 */
enum class LogLevel : std::uint8_t {
    Debug,  ///< Debug level for detailed information.
    Info,   ///< Info level for general information.
    Warn,   ///< Warn level for potential issues.
    Error   ///< Error level for critical failures.
};

/**
 * @brief Function type for handling log messages.
 *
 * @param level The severity level of the log message.
 * @param message The log message content.
 */
using LogHandler = std::function<void(LogLevel level, std::string_view message)>;

}  // namespace mcp

#pragma once

#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include <cstdint>
#include <functional>
#include <string_view>

namespace mcp {

// Async Model: Use boost::asio::awaitable
template <typename T>
using Task = boost::asio::awaitable<T>;

// Logging
enum class LogLevel : std::uint8_t { Debug, Info, Warn, Error };

using LogHandler = std::function<void(LogLevel, std::string_view)>;

}  // namespace mcp

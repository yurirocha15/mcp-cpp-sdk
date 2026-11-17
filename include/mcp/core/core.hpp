#pragma once

// <utility> comes first: Boost 1.74's awaitable.hpp uses std::exchange without including it.
#include <utility>

#include <boost/asio/io_context.hpp>
#include <cstdint>
#include <functional>
#include <mcp/core/async.hpp>
#include <string_view>

#pragma once

// Names the coroutine types without including Boost. Headers that only declare functions taking
// or returning them include this; code that defines or awaits a coroutine includes
// <mcp/core/async.hpp>.

#include <mcp/core/executor.hpp>

namespace mcp {

template <typename T>
class Task;

}  // namespace mcp

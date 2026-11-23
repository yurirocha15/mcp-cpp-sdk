#pragma once

#include <mcp/core/async_fwd.hpp>
#include <mcp/core/context.hpp>

#include <functional>
#include <nlohmann/json.hpp>

namespace mcp {

/// @brief Type-erased handler: takes a Context and JSON params, returns JSON result.
using TypeErasedHandler = std::function<Task<nlohmann::json>(Context&, const nlohmann::json&)>;

/// @brief Middleware function signature for intercepting handler invocations.
using Middleware =
    std::function<Task<nlohmann::json>(Context&, const nlohmann::json&, TypeErasedHandler)>;

}  // namespace mcp

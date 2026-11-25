#pragma once

// GCC 11 SSO Coroutine Safety — see docs/contributing.rst "Known Issues" for full details.
// Do NOT store std::string in a coroutine frame across co_await (GCC bugs #107288/#100611).
// Named patterns referenced at each call site below:
//   [no-named-temporaries] Pass params.get<In>() directly; don't store it in a named local.

#include <mcp/core/async_fwd.hpp>
#include <mcp/core/concepts.hpp>
#include <mcp/core/context.hpp>
#include <mcp/protocol/completion.hpp>
#include <mcp/server/middleware.hpp>

#include <concepts>
#include <coroutine>
#include <cstdint>
#include <functional>
#include <nlohmann/json.hpp>
#include <string>
#include <type_traits>
#include <utility>

namespace mcp {

/// @brief Type-erased synchronous handler: takes a Context and JSON params, returns the JSON
/// result directly.
using SyncTypeErasedHandler = std::function<nlohmann::json(Context&, const nlohmann::json&)>;

/// @brief Completion handler: takes CompleteParams, returns CompleteResult.
using CompletionHandler = std::function<Task<CompleteResult>(const CompleteParams&)>;

namespace detail {

enum class ToolResultMode : std::uint8_t {
    eNormalize,
    eValidated,
    /// The handler returns a CallToolResult value, which serializes to a result the protocol can
    /// decode: only the checks that depend on the protocol revision remain.
    eTyped,
};

/// @brief Detects whether Fn is invocable with (In) returning Task<Out>.
template <typename Fn, typename In, typename Out>
concept AsyncHandlerNoCtx = requires(Fn fn, In in) {
    { fn(std::move(in)) } -> std::same_as<Task<Out>>;
};

/// @brief Detects whether Fn is invocable with (Context&, In) returning Task<Out>.
template <typename Fn, typename In, typename Out>
concept AsyncHandlerWithCtx = requires(Fn fn, Context& ctx, In in) {
    { fn(ctx, std::move(in)) } -> std::same_as<Task<Out>>;
};

/// @brief Detects whether Fn is invocable with (In) returning Out (sync, no context).
template <typename Fn, typename In, typename Out>
concept SyncHandlerNoCtx = requires(Fn fn, In in) {
    { fn(std::move(in)) } -> std::same_as<Out>;
};

/// @brief Detects whether Fn is invocable with (Context&, In) returning Out (sync, with context).
template <typename Fn, typename In, typename Out>
concept SyncHandlerWithCtx = requires(Fn fn, Context& ctx, In in) {
    { fn(ctx, std::move(in)) } -> std::same_as<Out>;
};

/// @brief Detects a synchronous handler of either supported signature.
template <typename Fn, typename In, typename Out>
concept SyncHandler = SyncHandlerWithCtx<Fn, In, Out> || SyncHandlerNoCtx<Fn, In, Out>;

/// @brief Detects an asynchronous handler of either supported signature.
template <typename Fn, typename In, typename Out>
concept AsyncHandler = AsyncHandlerWithCtx<Fn, In, Out> || AsyncHandlerNoCtx<Fn, In, Out>;

/**
 * @brief Wraps a synchronous handler into a SyncTypeErasedHandler, without a coroutine.
 *
 * @tparam In  The input type (must satisfy JsonSerializable).
 * @tparam Out The output type (must satisfy JsonSerializable).
 * @tparam Fn  The handler callable type: `Out(Context&, In)` or `Out(In)`.
 * @param fn The sync handler to wrap.
 * @return A SyncTypeErasedHandler that deserializes JSON to In, calls fn, and serializes Out.
 */
template <JsonSerializable In, JsonSerializable Out, typename Fn>
    requires SyncHandler<Fn, In, Out>
SyncTypeErasedHandler wrap_sync_handler(Fn fn) {
    return [handler = std::move(fn)](Context& ctx, const nlohmann::json& params) -> nlohmann::json {
        if constexpr (SyncHandlerWithCtx<Fn, In, Out>) {
            return nlohmann::json(handler(ctx, params.get<In>()));
        } else {
            return nlohmann::json(handler(params.get<In>()));
        }
    };
}

/// @brief T, named through a type that depends on Dependent, so that a coroutine returning it is
/// checked only when the enclosing template is instantiated: a header can then define a
/// coroutine returning Task while Task is only declared.
template <typename Dependent, typename T>
struct DependentType {
    using type = T;
};

/**
 * @brief Wraps an asynchronous handler into a TypeErasedHandler.
 *
 * @tparam In  The input type (must satisfy JsonSerializable).
 * @tparam Out The output type (must satisfy JsonSerializable).
 * @tparam Fn  The handler callable type (must be async).
 * @param fn The async handler to wrap.
 * @return A TypeErasedHandler that deserializes JSON to In, calls fn, and serializes Out to JSON.
 */
template <JsonSerializable In, JsonSerializable Out, typename Fn>
    requires AsyncHandler<Fn, In, Out>
TypeErasedHandler wrap_handler(Fn fn) {
    using Result = DependentType<Fn, Task<nlohmann::json>>::type;
    return [handler = std::move(fn)](Context& ctx, const nlohmann::json& params) -> Result {
        // [gcc11-sso: no-named-temporaries] DO NOT introduce a named `input` variable here.
        if constexpr (AsyncHandlerWithCtx<Fn, In, Out>) {
            Out output = co_await handler(ctx, params.get<In>());
            co_return nlohmann::json(std::move(output));
        } else {
            Out output = co_await handler(params.get<In>());
            co_return nlohmann::json(std::move(output));
        }
    };
}

/// @brief Detects a coroutine handler shaped as TypeErasedHandler is, which is stored as one
/// without another coroutine around it.
template <typename Fn>
concept ErasedAsyncHandler =
    std::same_as<std::invoke_result_t<Fn&, Context&, const nlohmann::json&>, Task<nlohmann::json>>;

/// @brief A coroutine handler of JSON params and results, with or without the Context.
template <typename Fn>
concept JsonAsyncHandler = ErasedAsyncHandler<Fn> || AsyncHandler<Fn, nlohmann::json, nlohmann::json>;

/// @brief A JsonAsyncHandler as a TypeErasedHandler.
template <typename Fn>
    requires JsonAsyncHandler<Fn>
TypeErasedHandler erase_json_handler(Fn fn) {
    if constexpr (ErasedAsyncHandler<Fn>) {
        return TypeErasedHandler(std::move(fn));
    } else {
        return wrap_handler<nlohmann::json, nlohmann::json>(std::move(fn));
    }
}

/// @brief Detects a coroutine that returns text, called with @p Args.
template <typename Fn, typename... Args>
concept TextAsyncHandler = std::same_as<std::invoke_result_t<Fn&, Args...>, Task<std::string>>;

}  // namespace detail

}  // namespace mcp

#pragma once

// <utility> comes first: Boost 1.74's awaitable.hpp uses std::exchange without including it.
#include <utility>

#include <boost/asio/awaitable.hpp>
#include <coroutine>
#include <mcp/core/async_fwd.hpp>

namespace mcp {

/**
 * @brief An asynchronous task: a coroutine that produces a T when awaited.
 *
 * @details A Task is a boost::asio::awaitable<T>. It can be awaited from any awaitable coroutine,
 * passed to boost::asio::co_spawn, and combined with Asio's awaitable operators. A function that
 * hands co_spawn a callable instead of a Task value declares that callable's return type as
 * boost::asio::awaitable<T>, because co_spawn recognizes only that type as a callable's result.
 *
 * @tparam T The type the task produces.
 */
template <typename T>
class [[nodiscard]] Task : public boost::asio::awaitable<T> {
   public:
    /// The coroutine promise of boost::asio::awaitable<T>, so a Task coroutine runs as one.
    using promise_type = typename std::coroutine_traits<boost::asio::awaitable<T>>::promise_type;

    /// An empty task that holds no coroutine.
    Task() noexcept = default;

    /// Take over the coroutine of an awaitable.
    Task(boost::asio::awaitable<T>&& other) noexcept  // NOLINT(google-explicit-constructor)
        : boost::asio::awaitable<T>(std::move(other)) {}
};

}  // namespace mcp

#pragma once

// <utility> comes first: Boost 1.74's awaitable.hpp uses std::exchange without including it.
#include <utility>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <coroutine>
#include <exception>
#include <mcp/core/async_fwd.hpp>
#include <memory>
#include <stdexcept>

namespace mcp {

/**
 * @brief An asynchronous task: a coroutine that produces a T when awaited.
 *
 * @details A Task is a boost::asio::awaitable<T>. It can be awaited from any awaitable coroutine,
 * combined with Asio's awaitable operators, and passed to boost::asio::co_spawn, either as a value
 * or as a callable that returns one, such as `[&]() -> mcp::Task<void> { ... }`.
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

namespace detail {

template <typename AsioExecutor>
struct ExecutorWrap {
    static std::shared_ptr<const void> wrap(const AsioExecutor& executor) {
        boost::asio::any_io_executor held(executor);
        if (!held) {
            // An empty Asio executor, such as a default-constructed any_io_executor.
            return nullptr;
        }
        return std::make_shared<const boost::asio::any_io_executor>(std::move(held));
    }
};

template <typename Unused>
struct ExecutorAccess {
    static const boost::asio::any_io_executor& get(const Executor& executor) {
        if (!executor.target_) {
            throw std::invalid_argument("mcp::Executor holds no executor");
        }
        return *static_cast<const boost::asio::any_io_executor*>(executor.target_.get());
    }
};

}  // namespace detail

}  // namespace mcp

namespace boost::asio::detail {

// co_spawn reads the completion signature of a callable's result from awaitable_signature, which
// Asio defines for awaitable alone. These let it take a callable that returns a Task.
template <typename T>
struct awaitable_signature<mcp::Task<T>> {
    using type = void(std::exception_ptr, T);
};

template <>
struct awaitable_signature<mcp::Task<void>> {
    using type = void(std::exception_ptr);
};

}  // namespace boost::asio::detail

#pragma once

#include <mcp/core/core.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/version.hpp>

#include <exception>
#include <optional>
#include <utility>

namespace mcp::detail {

#if BOOST_VERSION >= 107700 && BOOST_VERSION < 108500
/// Boost 1.77 to 1.84 end co_spawn() with a co_await that still throws when the spawned coroutine
/// has been cancelled. Nothing catches that exception, and the process ends. Boost 1.85 switches
/// throw_if_cancelled off there itself; this does the same as the coroutine returns. The setting
/// belongs to the spawned coroutine, which is finishing, so nothing else sees it.
template <typename T>
Task<T> clear_throw_if_cancelled_on_return(Task<T> task) {
    std::optional<T> result;
    std::exception_ptr failure;
    try {
        result.emplace(co_await std::move(task));
    } catch (...) {
        failure = std::current_exception();
    }
    co_await boost::asio::this_coro::throw_if_cancelled(false);
    if (failure) {
        std::rethrow_exception(failure);
    }
    co_return std::move(*result);
}

inline Task<void> clear_throw_if_cancelled_on_return(Task<void> task) {
    std::exception_ptr failure;
    try {
        co_await std::move(task);
    } catch (...) {
        failure = std::current_exception();
    }
    co_await boost::asio::this_coro::throw_if_cancelled(false);
    if (failure) {
        std::rethrow_exception(failure);
    }
}
#endif

/// co_spawn(executor, task, use_awaitable): runs `task` on `executor` and resumes the caller with
/// its outcome. A cancellation of the caller is forwarded to `task`.
template <typename Executor, typename T>
Task<T> spawn_awaitable(Executor executor, Task<T> task) {
#if BOOST_VERSION >= 107700 && BOOST_VERSION < 108500
    return boost::asio::co_spawn(std::move(executor),
                                 clear_throw_if_cancelled_on_return(std::move(task)),
                                 boost::asio::use_awaitable);
#else
    return boost::asio::co_spawn(std::move(executor), std::move(task), boost::asio::use_awaitable);
#endif
}

}  // namespace mcp::detail

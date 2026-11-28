#pragma once

#include <mcp/core/async_fwd.hpp>
#include <mcp/core/constants.hpp>

#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>

namespace mcp {

class StreamableHttpSessionManager;

/**
 * @brief What Server::run_http() runs with, and how it shuts down.
 *
 * @details A shutdown starts on SIGINT, SIGTERM, Server::stop() or when the listener fails, and
 * runs these steps:
 *
 * 1. @ref on_drain_start is called.
 * 2. The session manager drains (StreamableHttpSessionManager::drain()): it answers a request
 *    that begins from then on with `503` and `Connection: close`, and ends the
 *    `subscriptions/listen` and legacy GET streams. It still serves what a legacy client sends to
 *    finish a request in flight: its answers to the server's questions, its notifications, such
 *    as `notifications/cancelled`, and a DELETE that ends its session. The answers to requests
 *    already being served are still written, with `Connection: close` when they begin after the
 *    drain did.
 * 3. The run waits until no request is being served, for @ref drain_timeout at most.
 * 4. @ref on_shutdown is awaited. The connections stay open meanwhile, so an answer a handler
 *    produces while it runs is delivered.
 * 5. The run waits again until no request is being served.
 * 6. The manager closes, and run_http() returns once the handlers still running have finished.
 *
 * @ref shutdown_timeout, measured from the start of the shutdown, ends every step at once: what
 * is unfinished by then is abandoned, which includes an @ref on_shutdown that does not return.
 */
struct HttpRunOptions {
    /**
     * @brief The number of threads that run the event loop: the calling thread and
     *        `io_threads - 1` threads that run_http() starts and joins before it returns.
     *
     * @details With more than one, the coroutine handlers of one server can run at the same time
     * on different threads, which a program with a single loop thread never had to consider: what
     * handlers share needs its own synchronization. Server::sync() refuses on each of these
     * threads.
     *
     * @throws std::invalid_argument From run_http() when zero.
     */
    std::size_t io_threads{1};

    /// Called once, on the calling thread, with the session manager before anything runs, to set
    /// what its setters take. An exception it throws ends run_http() before anything is served.
    std::function<void(StreamableHttpSessionManager&)> configure;

    /// Awaited on the event loop before the manager listens, so a client that connects finds what it
    /// sets up, such as the tools registered on the server. An exception ends the run: the
    /// shutdown steps run, without a listener, and run_http() rethrows it. A shutdown that begins
    /// while it runs keeps the manager from listening and run_http() returns normally, also when
    /// on_start then throws. It is not cancelled: it ends when its own awaits fail or when the
    /// run's loop stops.
    std::function<Task<void>(StreamableHttpSessionManager&)> on_start;

    /// Called once, on the event loop, when the shutdown begins.
    std::function<void()> on_drain_start;

    /// Awaited on the event loop after the first wait for requests, bounded by @ref
    /// shutdown_timeout.
    std::function<Task<void>()> on_shutdown;

    /// How long the first wait for requests lasts at most. Unset, half of @ref shutdown_timeout,
    /// so that @ref on_shutdown always has time to run. It must not exceed @ref shutdown_timeout,
    /// and must be shorter than it when @ref on_shutdown is set; run_http() throws
    /// std::invalid_argument otherwise.
    std::optional<std::chrono::milliseconds> drain_timeout;

    /// How long the whole shutdown lasts at most, from its start.
    std::chrono::milliseconds shutdown_timeout{mcp::constants::shutdown::g_TIMEOUT_MS};
};

}  // namespace mcp

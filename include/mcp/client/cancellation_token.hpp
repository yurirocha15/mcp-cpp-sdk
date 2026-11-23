#pragma once

#include <mcp/core/export.hpp>

#include <memory>
#include <optional>
#include <string>

namespace mcp {

namespace detail {
struct CancellationState;
struct CancellationAccess;
}  // namespace detail

/**
 * @brief Cancels the requests it is given to, from any thread.
 *
 * @details Copies share one state: the application keeps one copy and passes another as
 * RequestOptions::cancellation of each request it may want to cancel. cancel() tells the server
 * about each of those requests still waiting for its answer, with `notifications/cancelled` (an
 * HTTP transport instead closes the response stream of a 2026-07-28 request), and ends the wait
 * at once, in every protocol revision: the call throws McpError with
 * mcp::constants::jsonrpc::g_REQUEST_CANCELLED, and an answer that arrives afterwards is dropped. A
 * request given a token that is already cancelled fails the same way without being sent.
 *
 * A call answering the `input_required` questions of a 2026-07-28 server stops before it sends
 * its next round. call_tool() following a task stops waiting and asks the server to cancel the task
 * with `tasks/cancel`; a call cancelled before the server's answer arrives never learns of a task
 * the server started for it, which then runs on. TaskHandle::wait() and TaskHandle::result() stop
 * waiting and leave the task to its handle. listen() does not use the token: Subscription::close()
 * ends a stream.
 */
class MCP_API CancellationToken {
   public:
    CancellationToken();

    /// Cancels the requests given this token, those waiting now and any given it later. Only the
    /// first call has an effect; @p reason goes in each `notifications/cancelled` it sends.
    void cancel(std::optional<std::string> reason = std::nullopt) const;

    /// Whether cancel() has been called on this token or a copy of it.
    [[nodiscard]] bool is_cancelled() const noexcept;

   private:
    friend struct detail::CancellationAccess;
    std::shared_ptr<detail::CancellationState> state_;
};

}  // namespace mcp

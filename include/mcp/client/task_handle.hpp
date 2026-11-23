#pragma once

#include <mcp/client/request_options.hpp>
#include <mcp/core/async_fwd.hpp>
#include <mcp/core/export.hpp>
#include <mcp/protocol/tasks.hpp>
#include <mcp/protocol/tools.hpp>

#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

namespace mcp {

namespace detail {
struct BlockingLoop;
}  // namespace detail

class SyncTaskHandle;

/**
 * @brief A task a server started for a tool call (the tasks extension, protocol revision
 * 2026-07-28), which the application can follow, answer and cancel.
 *
 * @details Client::start_tool() returns one when the server answers with a task, and
 * Client::task_handle() makes one from a task id kept from an earlier session of the server. A
 * copy refers to the same task. Every call needs the Client that made the handle to be alive and
 * connected. Each request carries the extension in its capabilities, whatever
 * ClientOptions::tasks is set to.
 */
class MCP_API TaskHandle {
   public:
    /** @brief A handle that is not attached to a task; every call on it throws McpError. */
    TaskHandle() = default;

    /** @brief For Client; applications do not call this. */
    TaskHandle(std::shared_ptr<void> client, TaskData created, std::optional<std::string> tool);

    /** @brief The task's identifier, which an application may keep to resume it later. */
    [[nodiscard]] const std::string& id() const { return created_.taskId; }

    /** @brief The state the task had when the server created it. */
    [[nodiscard]] const TaskData& created() const { return created_; }

    /**
     * @brief Fetch the task's current state with `tasks/get`.
     *
     * @throws McpError With mcp::constants::jsonrpc::g_INVALID_PARAMS when the server no longer knows
     * the task.
     */
    Task<DetailedTask> get(const RequestOptions& request_options = {}) const;

    /**
     * @brief Answer the requests of an `input_required` task with `tasks/update`.
     *
     * @param input_responses An object holding the answer to each request of the task's
     *        `inputRequests` that is answered, under the same key.
     * @param request_options Per-call options of the `tasks/update` request.
     */
    Task<void> update(const nlohmann::json& input_responses,
                      const RequestOptions& request_options = {}) const;

    /// @brief Blocking calls to this task, for a handle of a Client constructed from an Endpoint
    /// (see Client::sync()).
    /// @details The handle may outlive its Client. Once that Client has been destroyed, each
    /// blocking call throws McpError (mcp::constants::jsonrpc::g_CONNECTION_CLOSED), since the
    /// event loop it would run on has ended; the task itself goes on on the server.
    /// @throws std::logic_error If the client runs on the application's executor.
    [[nodiscard]] SyncTaskHandle sync() const;

    /**
     * @brief Ask the server to cancel the task with `tasks/cancel`.
     *
     * @details The server decides whether and when the task stops: the task may still finish
     * with another status.
     */
    Task<void> cancel(const RequestOptions& request_options = {}) const;

    /**
     * @brief Poll the task until it has finished and return its final state.
     *
     * @details Polls as ClientOptions::task_poll_interval and the task's `pollIntervalMs` say, and
     * answers the task's `inputRequests` with the handlers registered for elicitation, sampling
     * and roots, each question once.
     *
     * @throws McpError With mcp::constants::jsonrpc::g_REQUEST_TIMEOUT after
     * ClientOptions::task_timeout.
     */
    Task<DetailedTask> wait(const RequestOptions& request_options = {}) const;

    /**
     * @brief wait(), then the tool's result.
     *
     * @return The CallToolResult of a `completed` task, checked against the tool's output schema
     *         as call_tool() does when the handle comes from start_tool().
     * @throws McpError The error a `failed` task carries, or mcp::constants::jsonrpc::g_INVALID_REQUEST
     * for a task that was cancelled.
     */
    Task<CallToolResult> result(const RequestOptions& request_options = {}) const;

   private:
    friend class SyncTaskHandle;

    /// The loop of the client the task belongs to, when it was constructed from an Endpoint.
    [[nodiscard]] std::shared_ptr<const detail::BlockingLoop> blocking_loop() const;

    std::shared_ptr<void> client_;
    TaskData created_;
    std::optional<std::string> tool_;
};

}  // namespace mcp

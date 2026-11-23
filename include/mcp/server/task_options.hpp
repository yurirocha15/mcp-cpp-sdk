#pragma once

#include <mcp/core/async_fwd.hpp>
#include <mcp/core/context.hpp>
#include <mcp/protocol/tools.hpp>
#include <mcp/server/task_store.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

namespace mcp {

/**
 * @brief Whether a tool call may become a task (see Server::set_tool_task_mode()).
 */
enum class ToolTaskMode : std::uint8_t {
    /// The call always runs inside its request and answers with the tool's result.
    eNever,
    /// A client that declared the tasks extension gets a task handle in place of the result, and
    /// any other client waits for the result as usual.
    eAuto,
    /// Like eAuto, except that a client that did not declare the tasks extension is answered with
    /// `mcp::constants::jsonrpc::g_MISSING_REQUIRED_CLIENT_CAPABILITY` naming it.
    eRequired,
};

/**
 * @brief What a tool's task preparation decides (see Server::set_tool_task_preparation()).
 */
struct TaskPreparation {
    /// The arguments the task's handler is called with.
    nlohmann::json arguments = nlohmann::json::object();
    /// When set, answers the call with this result and no task is created.
    std::optional<CallToolResult> result;
};

/// Runs inside a tools/call that would create a task, before the task exists.
using ToolTaskPreparation =
    std::function<Task<TaskPreparation>(Context&, const nlohmann::json& arguments)>;

/**
 * @brief Settings of the tasks extension (see Server::enable_tasks()).
 *
 * @details The defaults are starting points to be set for the deployment.
 */
struct TaskOptions {
    /// Where tasks are kept. When unset, the server keeps them in an InMemoryTaskStore with
    /// `max_tasks_per_principal` as its per-principal limit.
    std::shared_ptr<TaskStore> store;
    /// How long a new task is kept after its creation. Unset keeps it without limit.
    std::optional<std::chrono::milliseconds> ttl{std::chrono::hours(1)};
    /// The polling interval suggested to clients in the state of a task.
    std::chrono::milliseconds poll_interval{std::chrono::seconds(2)};
    /// Handlers one principal may have running at once. A task counts until its handler returns,
    /// whatever its status: a cancelled or expired task whose handler has not returned still
    /// counts, and a new task is refused while the principal is at the limit. The default store
    /// also holds at most this many tasks of one principal, finished ones included until they
    /// expire.
    std::size_t max_tasks_per_principal{100};
    /// Handlers that may run at once for all principals, counted like `max_tasks_per_principal`.
    /// The default store also holds at most this many tasks.
    std::size_t max_tasks{10000};
    /// Receives a warning about a configuration that works but is likely not what was meant.
    /// When unset, warnings are written to standard error.
    std::function<void(const std::string&)> warning_handler;
};

}  // namespace mcp

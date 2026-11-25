#pragma once

#include <mcp/core/constants.hpp>
#include <mcp/core/export.hpp>
#include <mcp/protocol/tasks.hpp>

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>

namespace mcp {

/**
 * @brief Everything a TaskStore keeps about one task.
 */
struct TaskRecord {
    /// The principal that created the task (Context::principal()), empty for an anonymous caller.
    /// Only that principal may read, answer or cancel it.
    std::string owner;
    std::string id;                                 ///< The unguessable task identifier.
    TaskStatus status = TaskStatus::eWorking;       ///< Where the task is in its life cycle.
    std::optional<std::string> status_message;      ///< Free-text note on the status, shown to clients.
    std::chrono::system_clock::time_point created;  ///< When the task was created.
    std::chrono::system_clock::time_point updated;  ///< When the task last changed.
    /// How long after `created` the store may discard the task; unset keeps it without limit.
    std::optional<std::chrono::milliseconds> ttl;
    std::optional<std::chrono::milliseconds> poll_interval;  ///< Polling interval suggested to clients.
    /// The requests of the task's handler that wait for the client, keyed by identifier, as the
    /// `tasks/get` of an `input_required` task lists them.
    nlohmann::json input_requests = nlohmann::json::object();
    /// The client's answers to `input_requests` that the handler has not taken yet, keyed like them.
    nlohmann::json input_responses = nlohmann::json::object();
    std::optional<nlohmann::json> result;  ///< The request's result, once `completed`.
    std::optional<nlohmann::json> error;   ///< The JSON-RPC error object, once `failed`.

    /// Whether the task reached `completed`, `failed` or `cancelled`.
    [[nodiscard]] bool terminal() const {
        return status == TaskStatus::eCompleted || status == TaskStatus::eFailed ||
               status == TaskStatus::eCancelled;
    }

    /// The task as `tasks/get` and `notifications/tasks` describe it.
    [[nodiscard]] MCP_API DetailedTask to_detailed() const;
};

/// Formats @p time as an ISO 8601 UTC timestamp with millisecond precision, such as
/// `2026-07-28T10:30:00.250Z`.
[[nodiscard]] MCP_API std::string format_task_time(std::chrono::system_clock::time_point time);

/**
 * @brief Where a Server keeps its tasks (see Server::enable_tasks()).
 *
 * @details The Server runs a task's handler and the store holds the task's state, so that an
 * application can keep that state wherever it likes. The Server calls the store from any thread,
 * possibly at once, and from coroutines it must not block for long: an implementation is
 * thread-safe and each call returns quickly. A task must be findable by get() as soon as create()
 * returns.
 *
 * Methods that change a task return its state after the change, which the Server sends to clients
 * that listen for it, or nullopt when nothing changed: the task is unknown, expired, already
 * finished, or the change does not apply to it. Every method but get() and create() takes a task
 * id and not an owner; the Server has checked the owner with get() before it acts for a client.
 *
 * The in-memory implementation is InMemoryTaskStore. It forgets its tasks when the process ends,
 * so a task created before a restart cannot be found afterwards.
 */
class MCP_API TaskStore {
   public:
    virtual ~TaskStore() = default;

    /**
     * @brief Creates a `working` task for @p owner under a new unguessable identifier.
     *
     * @param owner The principal the task belongs to.
     * @param ttl How long the task is kept after creation; unset keeps it without limit.
     * @param poll_interval Polling interval to suggest to clients; unset suggests none.
     * @return The task, or nullopt when the store refuses another task for @p owner.
     */
    virtual std::optional<TaskRecord> create(
        std::string_view owner, std::optional<std::chrono::milliseconds> ttl,
        std::optional<std::chrono::milliseconds> poll_interval) = 0;

    /// The task @p id, or nullopt when there is none, it expired, or @p owner did not create it.
    /// The three cases are not told apart.
    virtual std::optional<TaskRecord> get(std::string_view owner, std::string_view id) = 0;

    /// Sets the status message of a task that has not finished.
    virtual std::optional<TaskRecord> set_status_message(std::string_view id, std::string message) = 0;

    /// Adds the request @p request, a `{"method", "params"}` object, to the outstanding input
    /// requests under @p key and sets the task `input_required`.
    virtual std::optional<TaskRecord> add_input_request(std::string_view id, std::string_view key,
                                                        nlohmann::json request) = 0;

    /**
     * @brief Records the client's answers to outstanding input requests.
     *
     * @param id The task identifier.
     * @param responses An object of answers by key. An answer whose key is not outstanding is
     *                  ignored. An answered request leaves the outstanding set, and the task
     *                  returns to `working` once none is left.
     * @return The task, or nullopt when no answer was recorded.
     */
    virtual std::optional<TaskRecord> submit_input_responses(std::string_view id,
                                                             const nlohmann::json& responses) = 0;

    /// Takes the recorded answer for @p key, or nullopt when there is none yet.
    virtual std::optional<nlohmann::json> take_input_response(std::string_view id,
                                                              std::string_view key) = 0;

    /// Finishes the task as `completed` with the request's @p result.
    virtual std::optional<TaskRecord> complete(std::string_view id, nlohmann::json result) = 0;

    /// Finishes the task as `failed` with the JSON-RPC error object @p error and @p message.
    virtual std::optional<TaskRecord> fail(std::string_view id, nlohmann::json error,
                                           std::string message) = 0;

    /// Finishes the task as `cancelled`, with @p message as its status message.
    virtual std::optional<TaskRecord> cancel(std::string_view id, std::string message) = 0;
};

/**
 * @brief Limits of an InMemoryTaskStore.
 *
 * @details The defaults are starting points to be set for the deployment.
 */
struct InMemoryTaskStoreOptions {
    /// Tasks one owner may hold at once, finished ones included until they expire. At the limit
    /// the owner's oldest finished task makes room, and create() refuses when there is none.
    std::size_t max_tasks_per_owner = mcp::constants::tasks::g_DEFAULT_MAX_TASKS_PER_OWNER;
    /// Tasks the store may hold in all, with the same rule.
    std::size_t max_tasks = mcp::constants::tasks::g_DEFAULT_MAX_TASKS;
    /// The current time; unset reads the system clock. Tests set it to expire tasks without
    /// waiting.
    std::function<std::chrono::system_clock::time_point()> clock;
};

/**
 * @brief A TaskStore that keeps tasks in memory.
 */
class MCP_API InMemoryTaskStore final : public TaskStore {
   public:
    /// Creates an empty store that applies the limits and the clock of @p options.
    explicit InMemoryTaskStore(InMemoryTaskStoreOptions options = {});

    InMemoryTaskStore(const InMemoryTaskStore&) = delete;
    InMemoryTaskStore& operator=(const InMemoryTaskStore&) = delete;

    std::optional<TaskRecord> create(std::string_view owner,
                                     std::optional<std::chrono::milliseconds> ttl,
                                     std::optional<std::chrono::milliseconds> poll_interval) override;
    std::optional<TaskRecord> get(std::string_view owner, std::string_view id) override;
    std::optional<TaskRecord> set_status_message(std::string_view id, std::string message) override;
    std::optional<TaskRecord> add_input_request(std::string_view id, std::string_view key,
                                                nlohmann::json request) override;
    std::optional<TaskRecord> submit_input_responses(std::string_view id,
                                                     const nlohmann::json& responses) override;
    std::optional<nlohmann::json> take_input_response(std::string_view id,
                                                      std::string_view key) override;
    std::optional<TaskRecord> complete(std::string_view id, nlohmann::json result) override;
    std::optional<TaskRecord> fail(std::string_view id, nlohmann::json error,
                                   std::string message) override;
    std::optional<TaskRecord> cancel(std::string_view id, std::string message) override;

    /// The tasks held now, expired ones not counted.
    [[nodiscard]] std::size_t size();

   private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

}  // namespace mcp

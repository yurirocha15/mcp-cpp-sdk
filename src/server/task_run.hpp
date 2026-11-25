#pragma once

// The running side of a task: the state a task's handler shares with the requests that answer or
// cancel the task. The task's own state lives in its TaskStore; a TaskRun holds only what exists
// while the handler runs in this process.

#include <mcp/core/async.hpp>
#include <mcp/server/task_store.hpp>

#include <atomic>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

namespace mcp::detail {

/// Thrown from a task's request for input once the task ended: it was cancelled, it expired or its
/// server is gone. The handler unwinds, and what it still returns goes nowhere.
class TaskAbandoned : public std::runtime_error {
   public:
    TaskAbandoned() : std::runtime_error("the task ended while waiting for client input") {}
};

class TaskRun : public std::enable_shared_from_this<TaskRun> {
   public:
    TaskRun(boost::asio::strand<boost::asio::any_io_executor> task_strand,
            std::shared_ptr<TaskStore> task_store, std::string task_id, std::string task_owner,
            nlohmann::json capabilities)
        : id(std::move(task_id)),
          owner(std::move(task_owner)),
          client_capabilities(std::move(capabilities)),
          store(std::move(task_store)),
          cancelled(std::make_shared<std::atomic<bool>>(false)),
          strand(std::move(task_strand)) {}

    const std::string id;
    const std::string owner;
    /// The `io.modelcontextprotocol/clientCapabilities` of the request that created the task.
    const nlohmann::json client_capabilities;
    const std::shared_ptr<TaskStore> store;
    /// Set when the task is cancelled or its server goes away; the handler sees it as
    /// Context::is_cancelled().
    const std::shared_ptr<std::atomic<bool>> cancelled;
    /// Runs the handler, and the waits for the client's answers.
    boost::asio::strand<boost::asio::any_io_executor> strand;
    /// Tells clients that listen for the task about its new state.
    std::function<Task<void>(std::shared_ptr<const TaskRecord>)> publish;

    /// Wakes the handler's wait for input so that it looks at the task again. Safe from any thread.
    void signal();

    /// Cancels the task's handler and wakes it. Safe from any thread.
    void abandon() {
        cancelled->store(true, std::memory_order_release);
        signal();
    }

    /// Cancels the handler of @p run once @p ttl has passed, unless stop_expiry() ran first. Runs
    /// on the strand; the task's time to live is counted from its creation. It holds @p run, since
    /// the timer can complete after the handler let go of the run.
    static Task<void> abandon_after(std::shared_ptr<TaskRun> run, std::chrono::milliseconds ttl);

    /// Ends abandon_after(), so that no timer outlives the handler. Runs on the strand.
    void stop_expiry();

    /// Publishes @p record without waiting, for a caller that cannot suspend.
    void publish_detached(std::shared_ptr<const TaskRecord> record) const;

    /// Generates the key of the next request for input of the task. Keys never repeat within a task.
    std::uint64_t next_key() { return ++key_counter_; }

    /// Records a key the handler chose for a question. Returns false when the task used it before.
    bool claim_key(const std::string& key) {
        const std::scoped_lock lock(keys_mutex_);
        return named_keys_.insert(key).second;
    }

    /// The number of times signal() ran; a wait that saw @p seen returns once it moved on.
    [[nodiscard]] std::uint64_t version() const { return version_.load(std::memory_order_acquire); }

    /// Waits until signal() runs after @p seen was read from version(), or a while passes, so
    /// that a task the store dropped is noticed.
    Task<void> wait_after(std::uint64_t seen);

   private:
    friend Task<void> task_wait_on_strand(std::shared_ptr<TaskRun> run, std::uint64_t seen);

    std::atomic<std::uint64_t> version_{0};
    std::atomic<std::uint64_t> key_counter_{0};
    std::mutex keys_mutex_;
    std::set<std::string> named_keys_;
    // Guarded by the strand.
    std::set<boost::asio::steady_timer*> waiters_;
    boost::asio::steady_timer* expiry_ = nullptr;
    bool expiry_stopped_ = false;
};

Task<void> task_wait_on_strand(std::shared_ptr<TaskRun> run, std::uint64_t seen);

/// Asks the client for input on behalf of the handler of @p run, and returns the client's answer
/// once `tasks/update` brings it. Throws TaskAbandoned when the task ended, and std::runtime_error
/// when the client did not declare the capability the request needs. A string @p chosen_key names
/// the question; it must not be empty, look like a generated name or repeat a key of the task, or
/// std::invalid_argument is thrown.
Task<nlohmann::json> task_request_input(std::shared_ptr<TaskRun> run, nlohmann::json method,
                                        std::optional<nlohmann::json> params,
                                        nlohmann::json chosen_key = nlohmann::json());

/// The status message that reports @p progress out of @p total, or @p message when it is given.
std::string task_progress_message(double progress, const std::optional<double>& total,
                                  const std::optional<std::string>& message);

}  // namespace mcp::detail

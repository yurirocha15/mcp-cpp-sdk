#pragma once

/**
 * @file client_impl.hpp
 * @brief Client::Impl, the state behind a Client, and the helpers it uses. Private to the library.
 */

#include "mcp/detail/async/spawn_awaitable.hpp"
#include "mcp/detail/client/response_cache.hpp"
#include "mcp/detail/constants.hpp"
#include "mcp/detail/deprecation_reporter.hpp"
#include "mcp/detail/json/peer_json.hpp"
#include "mcp/detail/protocol/notification_budget.hpp"
#include "mcp/detail/text/diagnostic_text.hpp"

#include <mcp/client/client.hpp>
#include <mcp/core/async.hpp>
#include <mcp/core/constants.hpp>
#include <mcp/detail/json_dump.hpp>
#include <mcp/transport/http_client.hpp>
#include <mcp/transport/tls.hpp>
#include "mcp/detail/async/serialized_transport_writer.hpp"

#include <algorithm>
#include <atomic>
#include <boost/asio/bind_executor.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/system_error.hpp>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <exception>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace methods = mcp::constants::methods;

namespace mcp {

/// Awaits @p request and decodes its result as a Result.
template <typename Result>
Task<Result> parse_result(Task<nlohmann::json> request) {
    auto result_json = co_await std::move(request);
    co_return result_json.template get<Result>();
}

inline std::optional<nlohmann::json> make_paginated_params(const std::optional<std::string>& cursor) {
    if (!cursor) {
        return std::nullopt;
    }

    PaginatedRequestParams params;
    params.cursor = cursor;
    return nlohmann::json(std::move(params));
}

/// Throws @p failure, a transport's failure to send a message to the server. An HTTP error status,
/// a TransportError and a network error become McpError with g_CONNECTION_CLOSED, with the
/// transport's exception nested in it for std::rethrow_if_nested(); an HTTP status is also in its
/// data() as `httpStatus`. Any other exception is thrown as it is.
[[noreturn]] inline void throw_transport_failure(const std::exception_ptr& failure) {
    try {
        std::rethrow_exception(failure);
    } catch (const HttpStatusError& error) {
        std::throw_with_nested(
            McpError(mcp::constants::jsonrpc::g_CONNECTION_CLOSED,
                     "The server answered with HTTP status " + std::to_string(error.status()),
                     nlohmann::json{{"httpStatus", error.status()}}));
    } catch (const boost::system::system_error& error) {
        std::throw_with_nested(
            McpError(mcp::constants::jsonrpc::g_CONNECTION_CLOSED,
                     "The connection to the server failed: " + error.code().message()));
    } catch (const TransportError& error) {
        std::throw_with_nested(McpError(mcp::constants::jsonrpc::g_CONNECTION_CLOSED, error.what()));
    }
}

/// What the client knows about the output of a tool it is about to call.
struct OutputCheck {
    std::optional<CompiledSchema> schema;
    /// Set when the server declared a schema that cannot be used; the call then fails unsent.
    std::optional<SchemaError> refusal;
};

/// Decodes a tools/call result and checks it against the tool's output schema, when the client has
/// one for it.
inline CallToolResult finish_call_result(const nlohmann::json& result_json, const OutputCheck& check) {
    const auto& output_schema = check.schema;
    auto result = result_json.get<CallToolResult>();
    if (output_schema && !result.isError.value_or(false) && result.structuredContent) {
        const auto validation = output_schema->validate(*result.structuredContent);
        if (!validation.valid) {
            std::string message = "structuredContent does not conform to the tool's outputSchema";
            nlohmann::json violations = nlohmann::json::array();
            for (const auto& violation : validation.violations) {
                violations.push_back(
                    {{"instancePath", violation.instance_path}, {"message", violation.message}});
            }
            throw StructuredContentError(mcp::constants::jsonrpc::g_INTERNAL_ERROR, std::move(message),
                                         nlohmann::json{{"violations", std::move(violations)}});
        }
    }
    return result;
}

/// Checks a tools/call result against the tool's output schema, when the client has one for it.
inline Task<CallToolResult> checked_call_result(Task<nlohmann::json> request, OutputCheck check) {
    if (check.refusal) {
        throw *check.refusal;
    }
    auto result_json = co_await std::move(request);
    co_return finish_call_result(result_json, check);
}

/// Whether @p result is the CreateTaskResult of the tasks extension.
inline bool is_task_result(const nlohmann::json& result) {
    if (!result.is_object()) {
        return false;
    }
    const auto found = result.find("resultType");
    return found != result.end() && found->is_string() &&
           found->get_ref<const std::string&>() == mcp::constants::tasks::g_RESULT_TYPE_TASK;
}

/// Decodes the answer of `tasks/get`, which an invalid answer turns into an McpError.
inline DetailedTask decode_task_state(const nlohmann::json& reply) {
    try {
        return reply.get<DetailedTask>();
    } catch (const std::exception& error) {
        throw McpError(mcp::constants::jsonrpc::g_INVALID_REQUEST,
                       "Invalid task state: " + detail::sanitize_for_diagnostics(error.what()));
    }
}

/// The final result of a finished task, as the request it stands for would have returned it.
/// @p task is the task's `tasks/get` answer.
inline nlohmann::json task_outcome(const nlohmann::json& task) {
    // [gcc11-sso: string-in-frame] The outcome is held as json; a failure becomes an McpError
    // only after the decoders are done with the document.
    DetailedTask detailed;
    std::optional<Error> failure;
    try {
        detailed = task.get<DetailedTask>();
        if (detailed.task.status == TaskStatus::eFailed) {
            failure = detailed.error->get<Error>();
        }
    } catch (const std::exception& error) {
        throw McpError(mcp::constants::jsonrpc::g_INVALID_REQUEST,
                       "Invalid task state: " + detail::sanitize_for_diagnostics(error.what()));
    }
    if (failure) {
        throw McpError(std::move(*failure));
    }
    if (detailed.task.status == TaskStatus::eCancelled) {
        throw McpError(mcp::constants::jsonrpc::g_INVALID_REQUEST, "The task was cancelled");
    }
    return std::move(*detailed.result);
}

/// Throws ElicitationContentError when @p result accepts a form request with content its
/// `requestedSchema` rejects. A schema that cannot be compiled leaves the content unchecked.
inline void check_elicitation_content(const nlohmann::json& params, const ElicitResult& result) {
    if (result.action != ElicitAction::eAccept || !params.is_object()) {
        return;
    }
    const auto mode = params.find("mode");
    if (mode != params.end() && mode->is_string() && mode->get_ref<const std::string&>() != "form") {
        return;
    }
    const auto schema = params.find("requestedSchema");
    if (schema == params.end() || !schema->is_object()) {
        return;
    }
    ValidationResult validation;
    try {
        validation = validate_elicitation_content(*schema, result.content);
    } catch (const SchemaError&) {
        return;
    }
    if (validation.valid) {
        return;
    }
    nlohmann::json violations = nlohmann::json::array();
    for (const auto& violation : validation.violations) {
        violations.push_back(
            {{"instancePath", violation.instance_path}, {"message", violation.message}});
    }
    throw ElicitationContentError(mcp::constants::jsonrpc::g_INVALID_PARAMS,
                                  "The elicitation content does not match the requested schema: " +
                                      describe_violations(validation),
                                  nlohmann::json{{"violations", std::move(violations)}});
}

namespace detail {

/// The event loop a Client constructed from an Endpoint runs on, for the calls that wait for it.
struct BlockingLoop {
    BlockingLoop(std::shared_ptr<void> loop_owner, boost::asio::any_io_executor loop_executor,
                 std::thread::id loop_thread)
        : owner(std::move(loop_owner)), executor(std::move(loop_executor)), thread(loop_thread) {}

    /// Owns the loop's io_context. Every state that holds this BlockingLoop declares it first, so
    /// that the io_context is destroyed after that state's timers and strand, also when a
    /// TaskHandle or Subscription keeps the state after the Client is gone.
    std::shared_ptr<void> owner;
    boost::asio::any_io_executor executor;
    std::thread::id thread;

    /// Whether the Client has stopped the loop for good, after which nothing runs a blocking call,
    /// and the calls that wait. Held apart from the loop, which a waiting call must not keep.
    struct End {
        std::mutex mutex;
        std::condition_variable changed;
        bool ended = false;  // guarded by mutex
    };
    std::shared_ptr<End> end = std::make_shared<End>();

    /// Ends the waits of the blocking calls, including those whose coroutine the loop never ran.
    void mark_ended() const {
        {
            std::scoped_lock lock(end->mutex);
            end->ended = true;
        }
        end->changed.notify_all();
    }
};

/// Runs the coroutine @p start makes on @p loop and waits for it; see Client::sync().
template <typename T>
T run_blocking(const std::shared_ptr<const BlockingLoop>& loop, std::function<Task<T>()> start) {
    if (!loop) {
        throw std::logic_error(
            "mcp::Client: blocking calls need a client constructed from an Endpoint; a client on "
            "the application's executor is used through its coroutines");
    }
    if (std::this_thread::get_id() == loop->thread) {
        throw std::logic_error(
            "mcp::Client: a blocking call on the client's own event loop thread would wait for "
            "itself; await the coroutine instead");
    }
    struct Call {
        std::conditional_t<std::is_void_v<T>, bool, std::optional<T>> value{};
        std::exception_ptr error;
        bool done = false;
    };
    auto call = std::make_shared<Call>();
    const auto end = loop->end;
    const auto closed = [] {
        return McpError(mcp::constants::jsonrpc::g_CONNECTION_CLOSED,
                        "The client has been destroyed, and its connection with it");
    };
    // Spawned with the mutex held, so the end comes either before the check or after the spawn and
    // then ends the wait below. The coroutine is made on the loop as well, so that no part of the
    // call runs on this thread.
    {
        std::scoped_lock lock(end->mutex);
        if (end->ended) {
            throw closed();
        }
        auto body = [start = std::move(start)]() -> boost::asio::awaitable<T> {
            co_return co_await start();
        };
        if constexpr (std::is_void_v<T>) {
            boost::asio::co_spawn(loop->executor, std::move(body),
                                  [call, end](std::exception_ptr error) {
                                      {
                                          std::scoped_lock done_lock(end->mutex);
                                          call->error = std::move(error);
                                          call->done = true;
                                      }
                                      end->changed.notify_all();
                                  });
        } else {
            boost::asio::co_spawn(loop->executor, std::move(body),
                                  [call, end](std::exception_ptr error, T value) {
                                      {
                                          std::scoped_lock done_lock(end->mutex);
                                          call->error = std::move(error);
                                          if (!call->error) {
                                              call->value.emplace(std::move(value));
                                          }
                                          call->done = true;
                                      }
                                      end->changed.notify_all();
                                  });
        }
    }
    std::unique_lock lock(end->mutex);
    end->changed.wait(lock, [&] { return call->done || end->ended; });
    if (!call->done) {
        throw closed();
    }
    if (call->error) {
        std::rethrow_exception(call->error);
    }
    if constexpr (!std::is_void_v<T>) {
        return std::move(call->value).value();
    }
}

/// The client's side of one subscriptions/listen stream. Every change to it happens on the client
/// strand; the mutex only lets the handle's accessors read it from any thread.
struct SubscriptionState {
    SubscriptionState(const boost::asio::strand<boost::asio::any_io_executor>& strand,
                      std::size_t limit)
        : wake(strand), queue_limit(limit) {}

    /// The loop of a Client constructed from an Endpoint, for Subscription::sync(); null otherwise.
    /// Declared first, as BlockingLoop::owner requires.
    std::shared_ptr<const BlockingLoop> blocking_loop;
    /// Wakes whoever waits for the acknowledgment or the next notification.
    boost::asio::steady_timer wake;
    std::size_t queue_limit;
    RequestId id;
    SubscriptionFilter requested;
    /// Sends notifications/cancelled for the listen request, when the connection is still open.
    std::function<Task<void>()> send_cancel;
    /// Takes the stream out of the client's table of open streams.
    std::function<void()> forget;

    mutable std::mutex mutex;
    bool acknowledged_seen{false};
    SubscriptionFilter acknowledged;
    std::deque<SubscriptionEvent> queue;
    std::optional<SubscriptionEnd> ended;
    std::optional<Error> failure;
    std::uint64_t dropped{0};

    void signal() { wake.expires_at(std::chrono::steady_clock::now()); }

    void acknowledge(SubscriptionFilter honored) {
        {
            std::scoped_lock lock(mutex);
            if (acknowledged_seen) {
                return;
            }
            acknowledged = std::move(honored);
            acknowledged_seen = true;
        }
        signal();
    }

    void push(SubscriptionEvent event) {
        {
            std::scoped_lock lock(mutex);
            if (ended) {
                return;
            }
            if (queue.size() >= queue_limit) {
                queue.pop_front();
                ++dropped;
            }
            queue.push_back(std::move(event));
        }
        signal();
    }

    void finish(SubscriptionEnd how, std::optional<Error> error) {
        {
            std::scoped_lock lock(mutex);
            if (ended) {
                return;
            }
            ended = how;
            failure = std::move(error);
        }
        signal();
    }

    /// The state of the wait for the acknowledgment; holds no string, so it is safe to keep
    /// across a suspension point.
    enum class AckWait : std::uint8_t {
        ePending,
        eAcknowledged,
        eEnded,
    };

    AckWait ack_wait() const {
        std::scoped_lock lock(mutex);
        if (acknowledged_seen) {
            return AckWait::eAcknowledged;
        }
        return ended ? AckWait::eEnded : AckWait::ePending;
    }

    /// Runs on the strand: the next notification, or nothing once the stream has ended.
    static Task<std::optional<SubscriptionEvent>> wait_next(std::shared_ptr<SubscriptionState> state) {
        for (;;) {
            {
                std::scoped_lock lock(state->mutex);
                if (!state->queue.empty()) {
                    std::optional<SubscriptionEvent> event(std::move(state->queue.front()));
                    state->queue.pop_front();
                    co_return event;
                }
                if (state->ended) {
                    co_return std::nullopt;
                }
            }
            // Ending the stream and queueing both run on this strand and signal after changing
            // the state, so a change is either seen above or wakes the wait that starts here.
            state->wake.expires_at(std::chrono::steady_clock::time_point::max());
            try {
                co_await state->wake.async_wait(boost::asio::use_awaitable);
            } catch (const boost::system::system_error& error) {
                if (error.code() != boost::asio::error::operation_aborted) {
                    throw;
                }
            }
        }
    }

    /// Runs on the strand: ends the stream locally, then tells the server.
    static Task<void> cancel(std::shared_ptr<SubscriptionState> state) {
        {
            std::scoped_lock lock(state->mutex);
            if (state->ended) {
                co_return;
            }
        }
        state->finish(SubscriptionEnd::eCancelled, std::nullopt);
        state->forget();
        co_await state->send_cancel();
    }
};

}  // namespace detail

inline Task<std::optional<SubscriptionEvent>> no_event() { co_return std::nullopt; }

inline Task<void> nothing_to_send() { co_return; }

/// The `notifications` object of an acknowledgment, read as leniently as the server's own
/// output allows: a member of the wrong type counts as not honored.
inline SubscriptionFilter honored_filter(const nlohmann::json& params) {
    SubscriptionFilter honored;
    const auto found = params.find("notifications");
    if (found == params.end() || !found->is_object()) {
        return honored;
    }
    const auto flag = [&found](const char* name) {
        const auto member = found->find(name);
        return member != found->end() && member->is_boolean() && member->get<bool>();
    };
    honored.toolsListChanged = flag("toolsListChanged");
    honored.promptsListChanged = flag("promptsListChanged");
    honored.resourcesListChanged = flag("resourcesListChanged");
    if (const auto uris = found->find("resourceSubscriptions");
        uris != found->end() && uris->is_array()) {
        for (const auto& uri : *uris) {
            if (uri.is_string()) {
                honored.resourceSubscriptions.push_back(uri.get<std::string>());
            }
        }
    }
    if (const auto ids = found->find("taskIds"); ids != found->end() && ids->is_array()) {
        for (const auto& id : *ids) {
            if (id.is_string()) {
                honored.taskIds.push_back(id.get<std::string>());
            }
        }
    }
    return honored;
}

namespace detail {

/// What the copies of a CancellationToken share: whether it was cancelled, why, and what runs when
/// it is.
struct CancellationState {
    using Callback = std::function<void(const std::optional<std::string>& reason)>;

    std::mutex mutex;
    std::atomic<bool> cancelled{false};
    /// Set once, before `cancelled`, and never changed afterwards.
    std::optional<std::string> reason;
    std::uint64_t next_registration{0};
    std::map<std::uint64_t, Callback> callbacks;

    /// Adds @p callback, which runs once when the token is cancelled. Returns nothing, and keeps
    /// nothing, when the token already is.
    std::optional<std::uint64_t> add(Callback callback) {
        std::scoped_lock lock(mutex);
        if (cancelled.load(std::memory_order_relaxed)) {
            return std::nullopt;
        }
        const auto registration = next_registration++;
        callbacks.emplace(registration, std::move(callback));
        return registration;
    }

    void remove(std::uint64_t registration) {
        std::scoped_lock lock(mutex);
        callbacks.erase(registration);
    }
};

struct CancellationAccess {
    static const std::shared_ptr<CancellationState>& state(const CancellationToken& token) {
        return token.state_;
    }
};

}  // namespace detail

/// Keeps a callback on a CancellationToken for as long as the wait it ends lasts.
class CancellationRegistration {
   public:
    CancellationRegistration() = default;
    CancellationRegistration(std::shared_ptr<detail::CancellationState> state,
                             std::uint64_t registration)
        : state_(std::move(state)), registration_(registration) {}
    CancellationRegistration(const CancellationRegistration&) = delete;
    CancellationRegistration& operator=(const CancellationRegistration&) = delete;
    CancellationRegistration(CancellationRegistration&& other) noexcept
        : state_(std::move(other.state_)), registration_(other.registration_) {}
    CancellationRegistration& operator=(CancellationRegistration&& other) noexcept {
        if (this != &other) {
            release();
            state_ = std::move(other.state_);
            registration_ = other.registration_;
        }
        return *this;
    }
    ~CancellationRegistration() { release(); }

   private:
    void release() noexcept {
        if (state_) {
            try {
                state_->remove(registration_);
            } catch (...) {
                // A mutex that cannot be locked leaves a callback that finds nothing to end.
            }
            state_.reset();
        }
    }

    std::shared_ptr<detail::CancellationState> state_;
    std::uint64_t registration_{0};
};

struct Client::Impl {
    struct PendingRequest {
        PendingRequest(const boost::asio::any_io_executor& executor, std::chrono::milliseconds timeout)
            : timer(executor), deadline(std::chrono::steady_clock::now() + timeout) {
            timer.expires_at(deadline);
        }

        boost::asio::steady_timer timer;
        std::chrono::steady_clock::time_point deadline;
        std::shared_ptr<std::atomic<bool>> cancel_write_before_start =
            std::make_shared<std::atomic<bool>>(false);
        nlohmann::json result;
        std::optional<Error> error;
        std::exception_ptr write_error;
        bool completed{false};
        /// The era probe: a reply that cannot be decoded is an answer to it, see fail_probe().
        bool probe{false};
        /// Set while the transport reports the write is waiting for a person: what was left of the
        /// time budget, which resumes running afterwards. `deadline` is the far future meanwhile.
        std::optional<std::chrono::steady_clock::duration> paused_remaining;
    };

    Impl(std::shared_ptr<ITransport> client_transport, const boost::asio::any_io_executor& executor,
         ClientOptions client_options)
        : transport(std::move(client_transport)),
          strand(boost::asio::make_strand(executor)),
          options(std::move(client_options)),
          writer(transport, strand),
          overlapping_writes(transport && transport->overlapping_writes()) {}

    /// Whether a message goes beside the writer's queue: on such a transport a write lasts until
    /// its answer starts, and must not hold back the cancellation of a call or the next call.
    bool writes_beside_queue() const { return overlapping_writes; }

    /// Sends a request of the session's era. A modern session adds the per-request `_meta` and checks
    /// the result type of the answer; a legacy session sends @p params as given.
    /// @p declare_tasks makes the request declare the tasks extension, as every request of a
    /// session with ClientOptions::tasks set does, and lets a tools/call be answered with a task.
    /// @p answered_input, when given, is set when the answer came after the client answered the
    /// server's `input_required` questions.
    static Task<nlohmann::json> request(std::shared_ptr<Impl> state, std::string_view method,
                                        const std::optional<nlohmann::json>& params,
                                        const RequestOptions& request_options,
                                        bool declare_tasks = false,
                                        std::shared_ptr<bool> answered_input = nullptr);

    /// A request of a cacheable method, answered from the response cache when the cache is on and
    /// holds a fresh result, as RequestOptions::cache_mode allows; see ResponseCacheOptions.
    static Task<nlohmann::json> cacheable_request(std::shared_ptr<Impl> state, std::string_view method,
                                                  std::optional<nlohmann::json> params,
                                                  const RequestOptions& request_options);

    static Task<nlohmann::json> cached_request(std::shared_ptr<Impl> state,
                                               std::shared_ptr<const std::string> method,
                                               std::optional<nlohmann::json> params,
                                               RequestOptions request_options);

    /// The RequestOptions::on_progress callback of one request in flight. The read loop calls it
    /// holding `mutex`, and the request's end closes it holding `mutex`, so a progress
    /// notification that arrives after the answer never reaches a callback whose caller returned.
    struct ProgressRouteState {
        explicit ProgressRouteState(const NotificationRateLimit& limit) : bucket(limit) {}

        std::recursive_mutex mutex;
        std::function<void(const ProgressNotificationParams&)> callback;
        bool open = true;
        /// The progress notifications of this request that may still reach the callback.
        detail::TokenBucket bucket;
    };

    /// Ends a request's progress route when the request ends.
    struct ProgressRoute {
        std::shared_ptr<Impl> state;
        std::shared_ptr<const std::string> key;
        std::shared_ptr<ProgressRouteState> route;

        ProgressRoute() = default;
        ProgressRoute(const ProgressRoute&) = delete;
        ProgressRoute& operator=(const ProgressRoute&) = delete;
        ProgressRoute(ProgressRoute&&) noexcept = default;
        ProgressRoute& operator=(ProgressRoute&&) = delete;
        ~ProgressRoute() {
            if (!route) {
                return;
            }
            {
                std::scoped_lock lock(state->handlers_mutex);
                state->progress_routes.erase(*key);
            }
            std::scoped_lock lock(route->mutex);
            route->open = false;
        }
    };

    /// Gives the request in @p params a progress token, unless its `_meta` names one, and routes
    /// the notifications/progress carrying it to RequestOptions::on_progress until the returned
    /// route ends. The callback is taken out of @p request_options, so the requests made on this
    /// one's behalf (input rounds, task polls, a cancel) carry no token of their own.
    static ProgressRoute route_progress(const std::shared_ptr<Impl>& state, nlohmann::json& params,
                                        RequestOptions& request_options);

    /// Sends a request whose RequestOptions::on_progress is set, routing its progress for as long
    /// as the request lasts.
    static Task<nlohmann::json> request_with_progress(
        std::shared_ptr<Impl> state, std::shared_ptr<const std::string> method, nlohmann::json params,
        RequestOptions request_options, bool declare_tasks, std::shared_ptr<bool> answered_input);

    /// Hands a notifications/progress to the RequestOptions::on_progress of the request whose
    /// token it carries. Returns false when no request in flight has that token, leaving the
    /// notification to Client::on_progress.
    static bool deliver_request_progress(const std::shared_ptr<Impl>& state,
                                         const nlohmann::json& json_message);

    static Task<nlohmann::json> request_wire(std::shared_ptr<Impl> state, std::string_view method,
                                             const std::optional<nlohmann::json>& params,
                                             const RequestOptions& request_options, bool probe);

    /// The methods 2026-07-28 removed. The server answers none of them, so the request is refused
    /// here with the reason instead of costing a round trip for a method-not-found.
    static void reject_removed_method(const std::string& version, std::string_view method);

    /// A request carrying `resultType` takes part in the 2026-07-28 result rules: absent means
    /// "complete", and any other value is one this client cannot act on.
    static Task<nlohmann::json> checked_result(Task<nlohmann::json> pending);

    static void check_complete(const nlohmann::json& result);

    static Task<void> discard_result(Task<nlohmann::json> request_task);

    static void start_read_loop(const std::shared_ptr<Impl>& state);

    static void request_close(const std::shared_ptr<Impl>& state, std::string message);

    static std::string request_correlation_key(int64_t id);

    static Task<nlohmann::json> send_request_wire(std::shared_ptr<Impl> state,
                                                  std::shared_ptr<const std::string> wire, int64_t id,
                                                  std::chrono::milliseconds timeout, bool probe,
                                                  bool cancellable,
                                                  std::optional<CancellationToken> cancellation);

    /// Sends `notifications/cancelled` for the request @p id, unless the client is closed. Nothing
    /// waits for the write, and a write that fails changes nothing for the caller.
    static void send_cancelled(const std::shared_ptr<Impl>& state, int64_t id,
                               std::optional<std::string> reason);

    /// Runs on the strand when the CancellationToken of the request @p id is cancelled: ends the
    /// request's wait with g_REQUEST_CANCELLED and tells the server, unless it already ended.
    static void cancel_request(const std::shared_ptr<Impl>& state, int64_t id,
                               const std::optional<std::string>& reason);

    /// Waits @p time on the strand, ending early when @p cancellation is cancelled or the client
    /// closes.
    static Task<void> pause_unless_cancelled(std::shared_ptr<Impl> state,
                                             std::chrono::milliseconds time,
                                             std::optional<CancellationToken> cancellation);

    static Task<void> notification(std::shared_ptr<Impl> state, std::string_view method,
                                   const std::optional<nlohmann::json>& params);

    static Task<void> send_notification_wire(std::shared_ptr<Impl> state,
                                             std::shared_ptr<const std::string> wire);

    /// Two failure classes meet in this loop and only one is fatal. read_message() failing means the
    /// transport can no longer produce messages, so the session ends: pending requests fail and the
    /// transport closes; it is the only statement inside the outer try for that reason. A failure
    /// after it concerns one message already off the wire: it is reported through on_protocol_error
    /// and dropped, and the loop continues.
    static Task<void> read_loop(std::shared_ptr<Impl> state);

    /// Decodes and routes one received message. Never throws: an unusable message is reported
    /// and dropped so that a single bad message cannot end the session.
    static void dispatch_message(const std::shared_ptr<Impl>& state, const std::string& raw);

    static void report_protocol_error(const std::shared_ptr<Impl>& state, int code,
                                      std::string message);

    static void dispatch_response(const std::shared_ptr<Impl>& state,
                                  const nlohmann::json& json_message);

    static void dispatch_notification(const std::shared_ptr<Impl>& state,
                                      const nlohmann::json& json_message);

    static Task<void> dispatch_incoming_request(std::shared_ptr<Impl> state,
                                                nlohmann::json json_message);

    static std::string make_result_wire(const RequestId& id, nlohmann::json result);

    static std::string make_error_wire(const RequestId& id, int code, std::string message);

    /// Whether request `id` is still waiting on a deadline that has not come: the timer woke because
    /// the deadline was paused or resumed, not because it fired or the request finished.
    static bool deadline_moved(const std::shared_ptr<Impl>& state, int64_t id);

    /// The transport says the write of request `key` waits for a person, such as an OAuth consent
    /// prompt. The prompt's time is the user's, so the request's deadline stops until it ends. Runs
    /// on the strand, which owns the request's timer.
    static void on_interaction(const std::shared_ptr<Impl>& state, std::string_view key, bool waiting);

    static void signal_pending_request(PendingRequest& pending_request);

    static void complete_request_write(const std::shared_ptr<Impl>& state, const std::string& id_key,
                                       std::exception_ptr write_error);

    static void erase_pending_request(const std::shared_ptr<Impl>& state, const std::string& id_key);

    static void fail_pending_requests(const std::shared_ptr<Impl>& state, const Error& error);

    /// The requests whose answer may be an input_required result.
    static bool supports_input_required(std::string_view method);

    /// Answers one input_required result: runs the registered handler of each entry in
    /// `inputRequests` and returns the `inputResponses` for the retry, keyed as the server asked.
    static Task<nlohmann::json> answer_input_requests(std::shared_ptr<Impl> state,
                                                      std::shared_ptr<const nlohmann::json> requests,
                                                      std::size_t limit);

    /// What answer_input_requests() left for the call that stopped waiting or goes on.
    struct InputAnswers {
        explicit InputAnswers(const boost::asio::any_io_executor& executor) : wake(executor) {}

        boost::asio::steady_timer wake;
        nlohmann::json answers;
        std::exception_ptr failure;
        bool done{false};
    };

    /// Runs the handlers beside the call on its strand, so @p deadline holds whatever they do. A
    /// handler still running then cannot be stopped; what it returns is dropped.
    static Task<nlohmann::json> answers_by(std::shared_ptr<Impl> state,
                                           std::shared_ptr<const nlohmann::json> requests,
                                           std::size_t limit,
                                           std::chrono::steady_clock::time_point deadline);

    /// Sends a request that may be answered with an input_required result and resumes it: each
    /// round answers the server's questions with the registered handlers, then sends the original
    /// request again as a new request carrying `inputResponses` and the server's `requestState`
    /// untouched. The timeout of the call bounds all rounds together.
    static Task<nlohmann::json> resume_input_required(std::shared_ptr<Impl> state,
                                                      std::shared_ptr<const std::string> method,
                                                      std::optional<nlohmann::json> params,
                                                      RequestOptions request_options,
                                                      std::shared_ptr<const std::string> version,
                                                      bool declare_tasks,
                                                      std::shared_ptr<bool> answered_input);

    /// The request of one task method; the answer is the task's state for tasks/get and an
    /// acknowledgment for the others.
    static Task<nlohmann::json> task_request(std::shared_ptr<Impl> state, std::string_view method,
                                             nlohmann::json params, RequestOptions request_options);

    /// Fetches the task's state and checks that it has the shape its status calls for.
    static Task<DetailedTask> task_state(std::shared_ptr<Impl> state, nlohmann::json params,
                                         RequestOptions request_options);

    static Task<void> task_ack(std::shared_ptr<Impl> state, std::string_view method,
                               nlohmann::json params, RequestOptions request_options);

    /// Polls the task named by @p params (`{"taskId"}`) until it finishes and returns its final
    /// `tasks/get` answer. The questions of an `input_required` task are answered once each with
    /// the registered handlers. The task's `pollIntervalMs` paces the polls.
    static Task<nlohmann::json> wait_for_task(std::shared_ptr<Impl> state, nlohmann::json params,
                                              RequestOptions request_options);

    /// Sends a tools/call that declares the tasks extension and follows the task the server starts,
    /// if it starts one, to its result.
    static Task<CallToolResult> call_tool_following(std::shared_ptr<Impl> state, nlohmann::json params,
                                                    OutputCheck check, RequestOptions request_options);

    /// Sends a tools/call that declares the tasks extension, and hands the task the server starts
    /// to the caller.
    static Task<std::variant<CallToolResult, TaskHandle>> start_tool(std::shared_ptr<Impl> state,
                                                                     nlohmann::json params,
                                                                     OutputCheck check,
                                                                     RequestOptions request_options);

    /// Opens a subscriptions/listen stream. The request is never answered while the stream is open,
    /// so it bypasses the table of pending requests (and their deadline): the stream has its own
    /// entry, and `timeout` limits only the wait for the acknowledgment.
    static Task<Subscription> listen(std::shared_ptr<Impl> state, const SubscriptionFilter& filter,
                                     const RequestOptions& request_options);

    static Task<Subscription> listen_on_strand(std::shared_ptr<Impl> state,
                                               std::shared_ptr<detail::SubscriptionState> stream,
                                               std::shared_ptr<const std::string> wire,
                                               std::chrono::milliseconds timeout);

    /// Routes a notification that names a subscription to that subscription's queue. The
    /// acknowledgment is consumed by the stream; every notification still reaches the handlers
    /// registered with on_notification().
    static void route_to_subscription(const std::shared_ptr<Impl>& state, const std::string& method,
                                      const nlohmann::json& json_message);

    /// The answer to a listen request ends its stream: the empty result is the server's graceful
    /// close, an error is a refusal.
    static void end_subscription_from_response(const std::shared_ptr<Impl>& state,
                                               const std::string& id_key,
                                               const nlohmann::json& json_message);

    /// On a transport without per-request streams the server ends a subscription it closes with
    /// notifications/cancelled naming the listen request, after the closing result. Returns whether
    /// the notification named an open stream; that stream ends as closed by the server.
    static bool end_subscription_from_cancel(const std::shared_ptr<Impl>& state,
                                             const nlohmann::json& json_message);

    /// The connection ended: every open stream ends without the server's closing result.
    static void end_subscriptions(const std::shared_ptr<Impl>& state, const Error& error);

    /// What the probe learned. `discover` is set for eDiscover and `error` for the error kinds; a
    /// probe the transport failed has neither.
    struct ProbeOutcome {
        enum class Kind : std::uint8_t {
            eDiscover,     ///< A DiscoverResult: the server is modern.
            eModernError,  ///< A modern protocol error: the server is modern, the request is not.
            eOtherError,   ///< Any other error, a reply that is not JSON, or a failed write: legacy.
            eTimeout,      ///< No reply in time: legacy.
            eClosed,       ///< The connection ended: neither.
        };
        Kind kind{Kind::eOtherError};
        std::optional<DiscoverResult> discover;
        std::optional<Error> error;
    };

    /// The outcome of one initialize attempt; `error` is the server's refusal of it.
    struct InitializeAttempt {
        std::optional<ConnectResult> result;
        std::optional<Error> error;
    };

    // The outcomes below travel as shared_ptr: they hold strings, and a coroutine frame must not
    // keep a std::string across a suspension point (see the header note on GCC 11).
    using ProbePtr = std::shared_ptr<ProbeOutcome>;
    using AttemptPtr = std::shared_ptr<InitializeAttempt>;
    using VersionPtr = std::shared_ptr<const std::string>;
    using VersionsPtr = std::shared_ptr<const std::vector<std::string>>;

    static bool is_modern_error_code(int code);

    static ProbePtr classify_probe_error(const std::shared_ptr<Impl>& state, const Error& error);

    static ProbePtr classify_probe_reply(const nlohmann::json& reply);

    /// Sends server/discover naming @p version and waits at most @p timeout for the answer.
    static Task<ProbePtr> probe(std::shared_ptr<Impl> state, VersionPtr version,
                                std::chrono::milliseconds timeout);

    /// The versions a protocol error lists in `data.supported`, empty when it lists none.
    static std::vector<std::string> advertised_versions(const Error& error);

    /// The newest version of @p ours (oldest first) that @p theirs also lists, or null.
    template <std::size_t N>
    static VersionPtr newest_mutual(const std::array<std::string_view, N>& ours,
                                    const std::vector<std::string>& theirs) {
        for (auto version = ours.rbegin(); version != ours.rend(); ++version) {
            if (std::ranges::find(theirs, *version) != theirs.end()) {
                return std::make_shared<const std::string>(*version);
            }
        }
        return nullptr;
    }

    static bool lists_modern_version(const Error& error);

    void report_era(ProtocolEra era, EraReason reason,
                    std::chrono::steady_clock::time_point started) const;

    static std::optional<nlohmann::json> initialize_params(const std::shared_ptr<Impl>& state,
                                                           const std::string& version);

    /// Sends initialize naming @p version. The server's refusal is returned, not thrown, so that
    /// the caller can tell a modern server that rejects initialize from a failure.
    static Task<AttemptPtr> attempt_initialize(std::shared_ptr<Impl> state, VersionPtr version,
                                               VersionsPtr advertised);

    /// initialize for a caller that has nowhere else to go: a refusal is an error.
    static Task<ConnectResult> initialize_session(std::shared_ptr<Impl> state, VersionPtr version,
                                                  VersionsPtr advertised);

    static ConnectResult modern_result(const DiscoverResult& discover, const std::string& version);

    /// Settles on the era a DiscoverResult allows: modern when the server lists a modern version
    /// this client speaks, else legacy when it lists a legacy one, else the connect fails.
    static Task<ConnectResult> adopt_discover(std::shared_ptr<Impl> state, ProbePtr outcome,
                                              EraReason reason,
                                              std::chrono::steady_clock::time_point started);

    /// Explains a probe that did not come back as a DiscoverResult to a caller that needs one.
    [[noreturn]] static void throw_probe_failure(const ProbeOutcome& outcome,
                                                 const std::string& version);

    /// Whether @p outcome refuses @p version as unsupported while listing it as supported.
    static bool refuses_listed_version(const ProbeOutcome& outcome, const std::string& version);

    /// ClientProtocol::pinned("2026-07-28"): discover for ConnectResult; never initialize.
    static Task<ConnectResult> connect_pinned_modern(std::shared_ptr<Impl> state,
                                                     std::chrono::steady_clock::time_point started);

    /// ClientProtocol::automatic().
    static Task<ConnectResult> connect_auto(std::shared_ptr<Impl> state,
                                            std::chrono::steady_clock::time_point started);

    static Task<ConnectResult> connect_session(std::shared_ptr<Impl> state);

    /// A line the client cannot decode while it waits for the era probe's answer is that answer: a
    /// server that replies to server/discover with plain text is not a modern one, and waiting out
    /// the probe timeout would only delay the fallback.
    static void fail_probe(const std::shared_ptr<Impl>& state);

    /// The loop of a Client constructed from an Endpoint, for the blocking calls; null otherwise.
    /// Declared first, as BlockingLoop::owner requires.
    std::shared_ptr<const detail::BlockingLoop> blocking_loop;
    std::shared_ptr<ITransport> transport;
    /// The executor the Client was constructed with, as Client::executor() returns it.
    Executor executor;
    boost::asio::strand<boost::asio::any_io_executor> strand;
    /// How many requests of the server are being answered. Touched on the strand only.
    std::size_t incoming_requests{0};
    /// Memory those requests take, as json_memory_cost() estimates it. Touched on the strand only.
    std::size_t incoming_request_bytes{0};
    boost::asio::steady_timer incoming_requests_wake{strand};
    /// The pauses between the polls of the tasks being waited for, which closing ends. Touched on
    /// the strand only.
    std::vector<std::weak_ptr<boost::asio::steady_timer>> poll_pauses;
    ClientOptions options;
    /// See ClientOptions::deprecation_handler; reporting from const members is no change of state.
    mutable detail::DeprecationReporter deprecations{options.deprecation_handler};
    detail::ResponseCache response_cache{options.response_cache};
    /// See ClientOptions::notification_rate_limit.
    detail::TokenBucket progress_bucket{options.notification_rate_limit};
    detail::TokenBucket log_bucket{options.notification_rate_limit};
    std::atomic<std::uint64_t> dropped_notifications{0};
    detail::SerializedTransportWriter writer;
    /// What the transport said of itself; see writes_beside_queue().
    bool overlapping_writes{false};

    mutable std::mutex pending_requests_mutex;
    std::map<std::string, std::unique_ptr<PendingRequest>, std::less<>> pending_requests;
    std::atomic<int64_t> next_request_id{1};
    std::atomic<bool> read_loop_started{false};
    std::atomic<bool> closed{false};

    /// The open subscriptions/listen streams by request id; the answer to such a request ends the
    /// stream, so these never sit in pending_requests.
    mutable std::mutex subscriptions_mutex;
    std::map<std::string, std::shared_ptr<detail::SubscriptionState>, std::less<>> subscriptions;

    mutable std::mutex handlers_mutex;
    std::map<std::string, NotificationCallback, std::less<>> notification_handlers;
    /// The progress routes of the requests in flight, by the correlation key of their token.
    std::map<std::string, std::shared_ptr<ProgressRouteState>, std::less<>> progress_routes;
    std::atomic<int64_t> next_progress_token{1};
    std::map<std::string, RequestHandler, std::less<>> request_handlers;
    std::vector<Root> roots;
    ClientCapabilities client_capabilities;
    Implementation client_info;

    /// The version every request names once connect() settled on the modern era; empty while the
    /// session is legacy or not yet connected. It is decided once and never changes.
    mutable std::mutex era_mutex;
    std::optional<std::string> modern_protocol_version;

    void become_modern(const std::string& version);

    std::optional<std::string> modern_version() const;

    /// What this client tells a server it can do: the capabilities given to connect(), plus what
    /// its registered handlers imply. A legacy session sends it once, in initialize; a modern one
    /// reads it afresh for every request, so a handler registered after connect() counts from the
    /// next request on.
    ClientCapabilities effective_capabilities() const;

    /// @p params with the per-request `_meta` of a modern request merged in. The caller's own
    /// `_meta` entries (progressToken, traceparent, ...) stay as they are; the four protocol keys
    /// are the client's to set.
    /// @p declare_tasks adds the tasks extension to the declared capabilities, which the client does
    /// for every request when ClientOptions::tasks is set.
    std::optional<nlohmann::json> with_request_meta(const std::string& version,
                                                    const std::optional<nlohmann::json>& params,
                                                    const std::optional<LoggingLevel>& level,
                                                    bool declare_tasks = false) const;

    /// What the client knows of a listed tool's schemas. The serialized schemas let a later
    /// listing reuse the compiled output schema when nothing changed.
    struct ListedTool {
        std::optional<std::string> output_text;
        std::optional<CompiledSchema> output_schema;
        /// Why output_schema is empty when the tool declares one and the client cannot use it.
        std::optional<SchemaError> output_problem;
    };
    mutable std::mutex listed_tools_mutex;
    std::map<std::string, ListedTool, std::less<>> listed_tools;

    /// The output schema to check a call of @p name against, if the client has one.
    OutputCheck output_check_for(std::string_view name) const;

    /// Compiles @p tool's output schema and remembers it; the problem when it cannot be used. The
    /// input schema is the server's to enforce, so nothing here depends on it.
    std::optional<SchemaError> remember_tool(const Tool& tool);

    /// Remembers the output schemas of @p result, reporting those that cannot be used. Every tool
    /// stays listed: whether a tool can be called is the server's to say.
    void accept_listed_tools(const ListToolsResult& result);

    static Task<ListToolsResult> accepted_tools(std::shared_ptr<Impl> state,
                                                Task<ListToolsResult> request);
};

}  // namespace mcp

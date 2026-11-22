#include "../detail/constants.hpp"
#include "../detail/diagnostic_text.hpp"
#include "../detail/notification_budget.hpp"
#include "../detail/peer_json.hpp"
#include "../detail/response_cache.hpp"
#include "../detail/spawn_awaitable.hpp"

#include <mcp/client/client.hpp>
#include <mcp/core/async.hpp>
#include <mcp/core/constants.hpp>
#include <mcp/detail/json_dump.hpp>
#include <mcp/detail/serialized_transport_writer.hpp>
#include <mcp/transport/http_client.hpp>
#include <mcp/transport/tls.hpp>

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

namespace mcp {

namespace {

/// Awaits @p request and decodes its result as a Result.
template <typename Result>
Task<Result> parse_result(Task<nlohmann::json> request) {
    auto result_json = co_await std::move(request);
    co_return result_json.template get<Result>();
}

std::optional<nlohmann::json> make_paginated_params(const std::optional<std::string>& cursor) {
    if (!cursor) {
        return std::nullopt;
    }

    PaginatedRequestParams params;
    params.cursor = cursor;
    return nlohmann::json(std::move(params));
}

/// Throws @p failure, a transport's failure to send a message to the server. An HTTP error status
/// and a failed connection become McpError with g_CONNECTION_CLOSED, as a closed stdio connection
/// does, with the transport's exception nested in it for std::rethrow_if_nested(); an HTTP status
/// is also in its data() as `httpStatus`. Any other exception is thrown as it is.
[[noreturn]] void throw_transport_failure(const std::exception_ptr& failure) {
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
    } catch (const TlsError& error) {
        std::throw_with_nested(McpError(mcp::constants::jsonrpc::g_CONNECTION_CLOSED, error.what()));
    }
}

}  // namespace

/// `error.message` is text the peer chose (dispatch_response() deserializes `error` with no
/// validation) and what() is what an application logs, so what() is flattened and bounded. error(),
/// and the message() it exposes, are structured protocol data a caller may compare or re-encode, and
/// stay exactly as the peer sent them; the split matches MetadataPolicyError's.
McpError::McpError(Error error)
    : std::runtime_error("JSON-RPC error " + std::to_string(error.code) + ": " +
                         detail::sanitize_for_diagnostics(error.message)),
      error_(std::move(error)) {}

McpError::McpError(int code, std::string message, std::optional<nlohmann::json> data)
    : McpError(Error{code, std::move(message), std::move(data)}) {}

ClientProtocol ClientProtocol::automatic() { return {Kind::eAuto, std::string()}; }

ClientProtocol ClientProtocol::legacy() {
    return {Kind::eLegacy, std::string(mcp::constants::protocol::g_LATEST_LEGACY_PROTOCOL_VERSION)};
}

ClientProtocol ClientProtocol::pinned(std::string version) {
    if (!is_supported_protocol_version(version)) {
        throw std::invalid_argument("Unknown protocol version: " +
                                    detail::sanitize_for_diagnostics(version));
    }
    return {Kind::eVersion, std::move(version)};
}

void to_json(nlohmann::json& json_obj, const SubscriptionFilter& filter) {
    json_obj = nlohmann::json::object();
    if (filter.toolsListChanged) {
        json_obj["toolsListChanged"] = true;
    }
    if (filter.promptsListChanged) {
        json_obj["promptsListChanged"] = true;
    }
    if (filter.resourcesListChanged) {
        json_obj["resourcesListChanged"] = true;
    }
    if (!filter.resourceSubscriptions.empty()) {
        json_obj["resourceSubscriptions"] = filter.resourceSubscriptions;
    }
    if (!filter.taskIds.empty()) {
        json_obj["taskIds"] = filter.taskIds;
    }
}

void from_json(const nlohmann::json& json_obj, SubscriptionFilter& filter) {
    filter = SubscriptionFilter{};
    filter.toolsListChanged = json_obj.value("toolsListChanged", false);
    filter.promptsListChanged = json_obj.value("promptsListChanged", false);
    filter.resourcesListChanged = json_obj.value("resourcesListChanged", false);
    if (const auto found = json_obj.find("resourceSubscriptions");
        found != json_obj.end() && !found->is_null()) {
        found->get_to(filter.resourceSubscriptions);
    }
    if (const auto found = json_obj.find("taskIds"); found != json_obj.end() && !found->is_null()) {
        found->get_to(filter.taskIds);
    }
}

namespace {

/// What the client knows about the output of a tool it is about to call.
struct OutputCheck {
    std::optional<CompiledSchema> schema;
    /// Set when the server declared a schema that cannot be used; the call then fails unsent.
    std::optional<SchemaError> refusal;
};

/// Decodes a tools/call result and checks it against the tool's output schema, when the client has
/// one for it.
CallToolResult finish_call_result(const nlohmann::json& result_json, const OutputCheck& check) {
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
Task<CallToolResult> checked_call_result(Task<nlohmann::json> request, OutputCheck check) {
    if (check.refusal) {
        throw *check.refusal;
    }
    auto result_json = co_await std::move(request);
    co_return finish_call_result(result_json, check);
}

/// Whether @p result is the CreateTaskResult of the tasks extension.
bool is_task_result(const nlohmann::json& result) {
    if (!result.is_object()) {
        return false;
    }
    const auto found = result.find("resultType");
    return found != result.end() && found->is_string() &&
           found->get_ref<const std::string&>() == mcp::constants::tasks::g_RESULT_TYPE_TASK;
}

/// Decodes the answer of `tasks/get`, which an invalid answer turns into an McpError.
DetailedTask decode_task_state(const nlohmann::json& reply) {
    try {
        return reply.get<DetailedTask>();
    } catch (const std::exception& error) {
        throw McpError(mcp::constants::jsonrpc::g_INVALID_REQUEST,
                       "Invalid task state: " + detail::sanitize_for_diagnostics(error.what()));
    }
}

/// The final result of a finished task, as the request it stands for would have returned it.
/// @p task is the task's `tasks/get` answer.
nlohmann::json task_outcome(const nlohmann::json& task) {
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
void check_elicitation_content(const nlohmann::json& params, const ElicitResult& result) {
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

}  // namespace

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
            std::lock_guard lock(end->mutex);
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
        std::lock_guard lock(end->mutex);
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
                                          std::lock_guard done_lock(end->mutex);
                                          call->error = std::move(error);
                                          call->done = true;
                                      }
                                      end->changed.notify_all();
                                  });
        } else {
            boost::asio::co_spawn(loop->executor, std::move(body),
                                  [call, end](std::exception_ptr error, T value) {
                                      {
                                          std::lock_guard done_lock(end->mutex);
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
            std::lock_guard lock(mutex);
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
            std::lock_guard lock(mutex);
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
            std::lock_guard lock(mutex);
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
        eEnded
    };

    AckWait ack_wait() const {
        std::lock_guard lock(mutex);
        if (acknowledged_seen) {
            return AckWait::eAcknowledged;
        }
        return ended ? AckWait::eEnded : AckWait::ePending;
    }

    /// Runs on the strand: the next notification, or nothing once the stream has ended.
    static Task<std::optional<SubscriptionEvent>> wait_next(std::shared_ptr<SubscriptionState> state) {
        for (;;) {
            {
                std::lock_guard lock(state->mutex);
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
            std::lock_guard lock(state->mutex);
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

namespace {

Task<std::optional<SubscriptionEvent>> no_event() { co_return std::nullopt; }

Task<void> nothing_to_send() { co_return; }

}  // namespace

Subscription::Subscription() = default;

Subscription::Subscription(std::shared_ptr<detail::SubscriptionState> state)
    : state_(std::move(state)) {}

RequestId Subscription::id() const { return state_ ? state_->id : RequestId{}; }

SubscriptionFilter Subscription::requested() const {
    return state_ ? state_->requested : SubscriptionFilter{};
}

SubscriptionFilter Subscription::acknowledged() const {
    if (!state_) {
        return {};
    }
    std::lock_guard lock(state_->mutex);
    return state_->acknowledged;
}

SubscriptionFilter Subscription::unhonored() const {
    const auto honored = acknowledged();
    const auto asked = requested();
    SubscriptionFilter missing;
    missing.toolsListChanged = asked.toolsListChanged && !honored.toolsListChanged;
    missing.promptsListChanged = asked.promptsListChanged && !honored.promptsListChanged;
    missing.resourcesListChanged = asked.resourcesListChanged && !honored.resourcesListChanged;
    for (const auto& uri : asked.resourceSubscriptions) {
        if (std::find(honored.resourceSubscriptions.begin(), honored.resourceSubscriptions.end(),
                      uri) == honored.resourceSubscriptions.end()) {
            missing.resourceSubscriptions.push_back(uri);
        }
    }
    for (const auto& id : asked.taskIds) {
        if (std::find(honored.taskIds.begin(), honored.taskIds.end(), id) == honored.taskIds.end()) {
            missing.taskIds.push_back(id);
        }
    }
    return missing;
}

Task<std::optional<SubscriptionEvent>> Subscription::next() {
    if (!state_) {
        return no_event();
    }
    return detail::spawn_awaitable(state_->wake.get_executor(),
                                   detail::SubscriptionState::wait_next(state_));
}

Task<void> Subscription::close() {
    if (!state_) {
        return nothing_to_send();
    }
    return detail::spawn_awaitable(state_->wake.get_executor(),
                                   detail::SubscriptionState::cancel(state_));
}

std::optional<SubscriptionEnd> Subscription::end_reason() const {
    if (!state_) {
        return SubscriptionEnd::eCancelled;
    }
    std::lock_guard lock(state_->mutex);
    return state_->ended;
}

std::optional<Error> Subscription::end_error() const {
    if (!state_) {
        return std::nullopt;
    }
    std::lock_guard lock(state_->mutex);
    return state_->failure;
}

std::uint64_t Subscription::dropped_events() const {
    if (!state_) {
        return 0;
    }
    std::lock_guard lock(state_->mutex);
    return state_->dropped;
}

namespace {

/// The `notifications` object of an acknowledgment, read as leniently as the server's own
/// output allows: a member of the wrong type counts as not honored.
SubscriptionFilter honored_filter(const nlohmann::json& params) {
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

}  // namespace

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
        std::lock_guard lock(mutex);
        if (cancelled.load(std::memory_order_relaxed)) {
            return std::nullopt;
        }
        const auto registration = next_registration++;
        callbacks.emplace(registration, std::move(callback));
        return registration;
    }

    void remove(std::uint64_t registration) {
        std::lock_guard lock(mutex);
        callbacks.erase(registration);
    }
};

struct CancellationAccess {
    static const std::shared_ptr<CancellationState>& state(const CancellationToken& token) {
        return token.state_;
    }
};

}  // namespace detail

CancellationToken::CancellationToken() : state_(std::make_shared<detail::CancellationState>()) {}

void CancellationToken::cancel(std::optional<std::string> reason) const {
    std::map<std::uint64_t, detail::CancellationState::Callback> callbacks;
    {
        std::lock_guard lock(state_->mutex);
        if (state_->cancelled.load(std::memory_order_relaxed)) {
            return;
        }
        state_->reason = std::move(reason);
        state_->cancelled.store(true, std::memory_order_release);
        callbacks.swap(state_->callbacks);
    }
    // Run outside the lock: a callback may end a wait that removes its own registration.
    for (auto& [registration, callback] : callbacks) {
        callback(state_->reason);
    }
}

bool CancellationToken::is_cancelled() const noexcept {
    return state_->cancelled.load(std::memory_order_acquire);
}

namespace {

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

}  // namespace

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
                                        std::shared_ptr<bool> answered_input = nullptr) {
        if (request_options.on_progress) {
            return request_with_progress(std::move(state), std::make_shared<const std::string>(method),
                                         params.value_or(nlohmann::json()), request_options,
                                         declare_tasks, std::move(answered_input));
        }
        const auto modern_version = state->modern_version();
        if (!modern_version) {
            return request_wire(std::move(state), method, params, request_options, false);
        }
        reject_removed_method(*modern_version, method);
        declare_tasks = declare_tasks || method.starts_with("tasks/");
        if (supports_input_required(method)) {
            return resume_input_required(std::move(state), std::make_shared<const std::string>(method),
                                         params, request_options,
                                         std::make_shared<const std::string>(*modern_version),
                                         declare_tasks, std::move(answered_input));
        }
        auto modern_params =
            state->with_request_meta(*modern_version, params, request_options.log_level, declare_tasks);
        return checked_result(
            request_wire(std::move(state), method, modern_params, request_options, false));
    }

    /// A request of a cacheable method, answered from the response cache when the cache is on and
    /// holds a fresh result, as RequestOptions::cache_mode allows; see ResponseCacheOptions.
    static Task<nlohmann::json> cacheable_request(std::shared_ptr<Impl> state, std::string_view method,
                                                  std::optional<nlohmann::json> params,
                                                  const RequestOptions& request_options) {
        if (!state->read_loop_started.load(std::memory_order_acquire)) {
            throw McpError(mcp::constants::jsonrpc::g_INVALID_REQUEST,
                           "Client must connect before sending requests");
        }
        // A closed client fails the call as it would without the cache.
        if (!state->response_cache.enabled() || request_options.cache_mode == CacheMode::eBypass ||
            state->closed.load(std::memory_order_acquire)) {
            return request(std::move(state), method, params, request_options);
        }
        return cached_request(std::move(state), std::make_shared<const std::string>(method),
                              std::move(params), request_options);
    }

    static Task<nlohmann::json> cached_request(std::shared_ptr<Impl> state,
                                               std::shared_ptr<const std::string> method,
                                               std::optional<nlohmann::json> params,
                                               RequestOptions request_options) {
        auto& cache = state->response_cache;
        // On the heap: GCC 11 mishandles a std::string that lives across a suspension point.
        const auto key =
            std::make_shared<const std::string>(detail::ResponseCache::key(*method, params));
        if (request_options.cache_mode == CacheMode::eUse) {
            auto kept = cache.find(*key);
            if (kept) {
                co_return std::move(*kept);
            }
        }
        const auto generation = cache.generation();
        auto answered_input = std::make_shared<bool>(false);
        nlohmann::json reply;
        try {
            reply = co_await request(state, *method, params, request_options, false, answered_input);
        } catch (const McpError& error) {
            // A refused cursor means the pages kept so far may not fit together any more; the
            // next call starts the list again from the first page.
            if (error.code() == mcp::constants::jsonrpc::g_INVALID_PARAMS && params &&
                params->is_object() && params->contains("cursor")) {
                cache.forget(*method);
            }
            throw;
        }
        // A result that depends on the answers to the server's questions is not one the method and
        // parameters identify.
        if (!*answered_input) {
            cache.store(*key, *method, params, reply, generation);
        }
        co_return reply;
    }

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
                std::lock_guard lock(state->handlers_mutex);
                state->progress_routes.erase(*key);
            }
            std::lock_guard lock(route->mutex);
            route->open = false;
        }
    };

    /// Gives the request in @p params a progress token, unless its `_meta` names one, and routes
    /// the notifications/progress carrying it to RequestOptions::on_progress until the returned
    /// route ends. The callback is taken out of @p request_options, so the requests made on this
    /// one's behalf (input rounds, task polls, a cancel) carry no token of their own.
    static ProgressRoute route_progress(const std::shared_ptr<Impl>& state, nlohmann::json& params,
                                        RequestOptions& request_options) {
        ProgressRoute ended;
        if (!request_options.on_progress) {
            return ended;
        }
        auto route = std::make_shared<ProgressRouteState>(state->options.notification_rate_limit);
        route->callback = std::move(request_options.on_progress);
        request_options.on_progress = nullptr;
        if (params.is_null()) {
            params = nlohmann::json::object();
        }
        if (!params.is_object()) {
            throw McpError(mcp::constants::jsonrpc::g_INVALID_PARAMS,
                           "Request params must be an object");
        }
        auto& meta = params["_meta"];
        if (meta.is_null()) {
            meta = nlohmann::json::object();
        }
        if (!meta.is_object()) {
            throw McpError(mcp::constants::jsonrpc::g_INVALID_PARAMS,
                           "Request _meta must be an object");
        }
        ProgressToken token;
        if (const auto found = meta.find("progressToken"); found != meta.end()) {
            try {
                token = found->get<ProgressToken>();
            } catch (const std::exception&) {
                throw McpError(mcp::constants::jsonrpc::g_INVALID_PARAMS,
                               "Request _meta.progressToken must be a string or an integer");
            }
        } else {
            token = ProgressToken{
                std::string(detail::constants::client_requests::g_PROGRESS_TOKEN_PREFIX) +
                std::to_string(state->next_progress_token.fetch_add(1, std::memory_order_relaxed))};
            meta["progressToken"] = token;
        }
        auto key = std::make_shared<const std::string>(token.correlation_key());
        {
            std::lock_guard lock(state->handlers_mutex);
            if (!state->progress_routes.emplace(*key, route).second) {
                throw std::invalid_argument(
                    "The request's progress token is already used by another request in flight");
            }
        }
        ProgressRoute routed;
        routed.state = state;
        routed.key = std::move(key);
        routed.route = std::move(route);
        return routed;
    }

    /// Sends a request whose RequestOptions::on_progress is set, routing its progress for as long
    /// as the request lasts.
    static Task<nlohmann::json> request_with_progress(
        std::shared_ptr<Impl> state, std::shared_ptr<const std::string> method, nlohmann::json params,
        RequestOptions request_options, bool declare_tasks, std::shared_ptr<bool> answered_input) {
        const auto route = route_progress(state, params, request_options);
        auto reply = co_await request(state, *method, params, request_options, declare_tasks,
                                      std::move(answered_input));
        co_return reply;
    }

    /// Hands a notifications/progress to the RequestOptions::on_progress of the request whose
    /// token it carries. Returns false when no request in flight has that token, leaving the
    /// notification to Client::on_progress.
    static bool deliver_request_progress(const std::shared_ptr<Impl>& state,
                                         const nlohmann::json& json_message) {
        const auto params = json_message.find("params");
        if (params == json_message.end() || !params->is_object()) {
            return false;
        }
        const auto token = params->find("progressToken");
        if (token == params->end() || !(token->is_string() || token->is_number_integer())) {
            return false;
        }
        std::shared_ptr<ProgressRouteState> route;
        {
            const auto key = token->get<ProgressToken>().correlation_key();
            std::lock_guard lock(state->handlers_mutex);
            const auto found = state->progress_routes.find(key);
            if (found == state->progress_routes.end()) {
                return false;
            }
            route = found->second;
        }
        ProgressNotificationParams progress;
        try {
            params->get_to(progress);
        } catch (const std::exception& error) {
            report_protocol_error(state, mcp::constants::jsonrpc::g_INVALID_PARAMS,
                                  "Malformed progress notification params: " +
                                      detail::sanitize_for_diagnostics(error.what()));
            return true;
        }
        // As for any notification callback, a throw is reported and the session carries on.
        std::lock_guard lock(route->mutex);
        if (!route->open) {
            return true;
        }
        if (!route->bucket.take()) {
            state->dropped_notifications.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        try {
            route->callback(progress);
        } catch (const std::exception& error) {
            report_protocol_error(
                state, mcp::constants::jsonrpc::g_INTERNAL_ERROR,
                "Progress callback threw: " + detail::sanitize_for_diagnostics(error.what()));
        } catch (...) {
            report_protocol_error(state, mcp::constants::jsonrpc::g_INTERNAL_ERROR,
                                  "Progress callback threw");
        }
        return true;
    }

    static Task<nlohmann::json> request_wire(std::shared_ptr<Impl> state, std::string_view method,
                                             const std::optional<nlohmann::json>& params,
                                             const RequestOptions& request_options, bool probe) {
        const auto timeout = request_options.timeout.value_or(state->options.request_timeout);
        if (timeout <= std::chrono::milliseconds::zero()) {
            throw std::invalid_argument("Request timeout must be positive");
        }

        const int64_t id = state->next_request_id.fetch_add(1, std::memory_order_relaxed);
        JSONRPCRequest request;
        request.id = RequestId{std::to_string(id)};
        request.method = std::string(method);
        request.params = params;
        auto wire = std::make_shared<const std::string>(
            mcp::detail::dump_json(nlohmann::json(std::move(request))));
        auto strand = state->strand;
        // Awaiting post(strand) would not rebind a caller-owned coroutine's continuation.
        // Spawn the complete pending-request lifecycle on the client strand instead.
        // A client must not cancel an initialize request.
        const bool cancellable = method != "initialize";
        return boost::asio::co_spawn(
            strand,
            send_request_wire(std::move(state), std::move(wire), id, timeout, probe, cancellable,
                              request_options.cancellation),
            boost::asio::use_awaitable);
    }

    /// The methods 2026-07-28 removed. The server answers none of them, so the request is refused
    /// here with the reason instead of costing a round trip for a method-not-found.
    static void reject_removed_method(const std::string& version, std::string_view method) {
        static constexpr std::array<std::string_view, 5> s_removed = {
            "initialize", "ping", "logging/setLevel", "resources/subscribe", "resources/unsubscribe"};
        if (std::find(s_removed.begin(), s_removed.end(), method) != s_removed.end()) {
            throw McpError(
                mcp::constants::jsonrpc::g_METHOD_NOT_FOUND,
                "Method " + std::string(method) + " does not exist in protocol version " + version);
        }
    }

    /// A request carrying `resultType` takes part in the 2026-07-28 result rules: absent means
    /// "complete", and any other value is one this client cannot act on.
    static Task<nlohmann::json> checked_result(Task<nlohmann::json> pending) {
        auto result = co_await std::move(pending);
        check_complete(result);
        co_return result;
    }

    static void check_complete(const nlohmann::json& result) {
        if (result.is_object()) {
            const auto found = result.find("resultType");
            if (found != result.end() && !(found->is_string() && *found == "complete")) {
                std::string shown = found->is_string() ? found->get<std::string>() : found->dump();
                throw McpError(
                    mcp::constants::jsonrpc::g_INVALID_REQUEST,
                    "Unsupported resultType in result: " + detail::sanitize_for_diagnostics(shown));
            }
        }
    }

    /// The requests whose answer may be an input_required result.
    static bool supports_input_required(std::string_view method) {
        return method == "tools/call" || method == "prompts/get" || method == "resources/read";
    }

    /// Answers one input_required result: runs the registered handler of each entry in
    /// `inputRequests` and returns the `inputResponses` for the retry, keyed as the server asked.
    static Task<nlohmann::json> answer_input_requests(std::shared_ptr<Impl> state,
                                                      std::shared_ptr<const nlohmann::json> requests,
                                                      std::size_t limit) {
        if (requests->size() > limit) {
            throw McpError(mcp::constants::jsonrpc::g_INVALID_REQUEST,
                           "The server asked " + std::to_string(requests->size()) +
                               " questions in one input_required result "
                               "(RequestOptions::max_input_requests)");
        }
        nlohmann::json responses = nlohmann::json::object();
        for (const auto& entry : requests->items()) {
            const auto& request = entry.value();
            if (!request.is_object() || !request.contains("method") || !request["method"].is_string()) {
                throw McpError(mcp::constants::jsonrpc::g_INVALID_REQUEST,
                               "input_required entry " + detail::sanitize_for_diagnostics(entry.key()) +
                                   " is not a request");
            }
            const auto method =
                std::make_shared<const std::string>(request["method"].get<std::string>());
            RequestHandler handler;
            {
                std::lock_guard lock(state->handlers_mutex);
                const auto found = state->request_handlers.find(*method);
                if (found != state->request_handlers.end()) {
                    handler = found->second;
                }
            }
            const bool answerable = *method == "elicitation/create" ||
                                    *method == "sampling/createMessage" || *method == "roots/list";
            if (!answerable || !handler) {
                throw McpError(mcp::constants::jsonrpc::g_INVALID_REQUEST,
                               "The server asked for " + detail::sanitize_for_diagnostics(*method) +
                                   " in an input_required result, but " +
                                   (answerable ? "no handler is registered for it"
                                               : "that is not a request a client answers"));
            }
            const nlohmann::json handler_params =
                request.contains("params") ? request["params"] : nlohmann::json::object();
            nlohmann::json answer;
            try {
                answer = co_await handler(handler_params);
            } catch (const nlohmann::json::exception& error) {
                throw McpError(
                    mcp::constants::jsonrpc::g_INVALID_REQUEST,
                    "input_required entry " + detail::sanitize_for_diagnostics(entry.key()) +
                        " cannot be decoded: " + detail::sanitize_for_diagnostics(error.what()));
            }
            responses[entry.key()] = std::move(answer);
        }
        co_return responses;
    }

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
                                           std::chrono::steady_clock::time_point deadline) {
        auto outcome = std::make_shared<InputAnswers>(state->strand);
        auto handlers = boost::asio::make_strand(state->strand.get_inner_executor());
        boost::asio::co_spawn(
            handlers, answer_input_requests(state, std::move(requests), limit),
            boost::asio::bind_executor(state->strand,
                                       [outcome](std::exception_ptr failure, nlohmann::json answers) {
                                           outcome->failure = std::move(failure);
                                           outcome->answers = std::move(answers);
                                           outcome->done = true;
                                           outcome->wake.cancel();
                                       }));
        while (!outcome->done && std::chrono::steady_clock::now() < deadline) {
            outcome->wake.expires_at(deadline);
            boost::system::error_code ignored;
            co_await outcome->wake.async_wait(
                boost::asio::redirect_error(boost::asio::use_awaitable, ignored));
        }
        if (!outcome->done) {
            throw McpError(mcp::constants::jsonrpc::g_REQUEST_TIMEOUT, "Request timed out");
        }
        if (outcome->failure) {
            std::rethrow_exception(outcome->failure);
        }
        co_return std::move(outcome->answers);
    }

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
                                                      std::shared_ptr<bool> answered_input) {
        const auto timeout = request_options.timeout.value_or(state->options.request_timeout);
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        // A tools/call that declared the extension may be answered with a task, which the caller
        // takes over.
        const bool task_allowed =
            *method == "tools/call" && (declare_tasks || state->options.tasks == ClientTaskMode::eAuto);
        std::shared_ptr<const nlohmann::json> input_responses;
        std::shared_ptr<const nlohmann::json> request_state;
        for (int round = 0;; ++round) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            if (remaining <= std::chrono::milliseconds::zero()) {
                throw McpError(mcp::constants::jsonrpc::g_REQUEST_TIMEOUT, "Request timed out");
            }
            request_options.timeout = remaining;
            auto round_params =
                state->with_request_meta(*version, params, request_options.log_level, declare_tasks);
            if (input_responses) {
                (*round_params)["inputResponses"] = *input_responses;
            }
            if (request_state) {
                (*round_params)["requestState"] = *request_state;
            }
            auto reply = co_await request_wire(state, *method, round_params, request_options, false);
            const auto type = reply.is_object() ? reply.find("resultType") : reply.end();
            if (!reply.is_object() || type == reply.end() || !type->is_string() ||
                *type != "input_required") {
                if (!(task_allowed && is_task_result(reply))) {
                    check_complete(reply);
                }
                if (answered_input && round > 0) {
                    *answered_input = true;
                }
                co_return reply;
            }
            if (round >= request_options.max_input_rounds) {
                throw McpError(mcp::constants::jsonrpc::g_INVALID_REQUEST,
                               "The server still asked for input after " +
                                   std::to_string(request_options.max_input_rounds) +
                                   " rounds (RequestOptions::max_input_rounds)");
            }
            const auto requests = reply.find("inputRequests");
            const auto reply_state = reply.find("requestState");
            const bool has_requests = requests != reply.end();
            const bool has_state = reply_state != reply.end();
            if ((has_requests && !requests->is_object()) || (has_state && !reply_state->is_string())) {
                throw McpError(mcp::constants::jsonrpc::g_INVALID_REQUEST,
                               "input_required result: inputRequests must be an object and "
                               "requestState a string");
            }
            if (!has_requests && !has_state) {
                throw McpError(mcp::constants::jsonrpc::g_INVALID_REQUEST,
                               "input_required result has neither inputRequests nor requestState");
            }
            request_state = has_state ? std::make_shared<const nlohmann::json>(*reply_state) : nullptr;
            if (has_requests) {
                auto questions = std::make_shared<const nlohmann::json>(*requests);
                const auto limit = static_cast<std::size_t>(request_options.max_input_requests);
                auto strand = state->strand;
                auto answers = co_await detail::spawn_awaitable(
                    strand, answers_by(state, std::move(questions), limit, deadline));
                input_responses = std::make_shared<const nlohmann::json>(std::move(answers));
            } else {
                input_responses = nullptr;
            }
        }
    }

    /// The request of one task method; the answer is the task's state for tasks/get and an
    /// acknowledgment for the others.
    static Task<nlohmann::json> task_request(std::shared_ptr<Impl> state, std::string_view method,
                                             nlohmann::json params, RequestOptions request_options) {
        co_return co_await request(state, method, params, request_options, true);
    }

    /// Fetches the task's state and checks that it has the shape its status calls for.
    static Task<DetailedTask> task_state(std::shared_ptr<Impl> state, nlohmann::json params,
                                         RequestOptions request_options) {
        auto reply =
            co_await task_request(std::move(state), "tasks/get", std::move(params), request_options);
        co_return decode_task_state(reply);
    }

    static Task<void> task_ack(std::shared_ptr<Impl> state, std::string_view method,
                               nlohmann::json params, RequestOptions request_options) {
        co_await task_request(std::move(state), method, std::move(params), request_options);
    }

    /// Polls the task named by @p params (`{"taskId"}`) until it finishes and returns its final
    /// `tasks/get` answer. The questions of an `input_required` task are answered once each with
    /// the registered handlers. The task's `pollIntervalMs` paces the polls.
    static Task<nlohmann::json> wait_for_task(std::shared_ptr<Impl> state, nlohmann::json params,
                                              RequestOptions request_options) {
        const auto deadline = std::chrono::steady_clock::now() + state->options.task_timeout;
        const auto limit = static_cast<std::size_t>(request_options.max_input_requests);
        // The keys already answered, as an object so that no string is held across a suspension.
        nlohmann::json answered = nlohmann::json::object();
        for (;;) {
            nlohmann::json current = co_await task_request(state, "tasks/get", params, request_options);
            TaskStatus status = TaskStatus::eWorking;
            std::int64_t poll_ms = state->options.task_poll_interval.count();
            nlohmann::json pending = nlohmann::json::object();
            {
                const auto detailed = decode_task_state(current);
                status = detailed.task.status;
                if (detailed.task.pollIntervalMs && *detailed.task.pollIntervalMs > 0) {
                    poll_ms = *detailed.task.pollIntervalMs;
                }
                if (detailed.inputRequests) {
                    pending = *detailed.inputRequests;
                }
            }
            if (status == TaskStatus::eCompleted || status == TaskStatus::eFailed ||
                status == TaskStatus::eCancelled) {
                co_return current;
            }

            if (status == TaskStatus::eInputRequired) {
                auto fresh = std::make_shared<nlohmann::json>(nlohmann::json::object());
                for (const auto& entry : pending.items()) {
                    if (!answered.contains(entry.key())) {
                        (*fresh)[entry.key()] = entry.value();
                        answered[entry.key()] = true;
                    }
                }
                if (!fresh->empty()) {
                    std::shared_ptr<const nlohmann::json> questions = std::move(fresh);
                    auto responses = co_await answer_input_requests(state, std::move(questions), limit);
                    nlohmann::json update = {{"taskId", params.at("taskId")},
                                             {"inputResponses", std::move(responses)}};
                    co_await task_request(state, "tasks/update", std::move(update), request_options);
                }
            }

            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            if (remaining <= std::chrono::milliseconds::zero()) {
                throw McpError(mcp::constants::jsonrpc::g_REQUEST_TIMEOUT,
                               "Timed out waiting for the task to finish");
            }
            // A cancelled token ends the pause, and the next tasks/get then fails without being sent.
            auto strand = state->strand;
            auto pause =
                pause_unless_cancelled(state, std::min(std::chrono::milliseconds(poll_ms), remaining),
                                       request_options.cancellation);
            co_await detail::spawn_awaitable(strand, std::move(pause));
        }
    }

    /// Sends a tools/call that declares the tasks extension and follows the task the server starts,
    /// if it starts one, to its result.
    static Task<CallToolResult> call_tool_following(std::shared_ptr<Impl> state, nlohmann::json params,
                                                    OutputCheck check, RequestOptions request_options) {
        if (check.refusal) {
            throw *check.refusal;
        }
        const auto route = route_progress(state, params, request_options);
        nlohmann::json reply = co_await request(state, "tools/call", params, request_options, true);
        if (is_task_result(reply)) {
            nlohmann::json task_params = {{"taskId", nlohmann::json()}};
            try {
                task_params["taskId"] = reply.get<CreateTaskResult>().task.taskId;
            } catch (const std::exception& error) {
                throw McpError(mcp::constants::jsonrpc::g_INVALID_REQUEST,
                               "Invalid task: " + detail::sanitize_for_diagnostics(error.what()));
            }
            // Nobody else holds this task, so a call that gives up on it asks the server to stop it.
            std::exception_ptr gave_up;
            nlohmann::json finished;
            try {
                finished = co_await wait_for_task(state, task_params, request_options);
            } catch (...) {
                gave_up = std::current_exception();
            }
            if (gave_up) {
                // The cancel goes out even when the caller's token is what ended the wait.
                RequestOptions cancel_options = request_options;
                cancel_options.cancellation.reset();
                try {
                    co_await task_request(state, "tasks/cancel", task_params, cancel_options);
                } catch (...) {
                    // The caller gets the error that ended the wait; a cancel that fails changes
                    // nothing.
                }
                std::rethrow_exception(gave_up);
            }
            reply = task_outcome(finished);
        }
        co_return finish_call_result(reply, check);
    }

    /// Sends a tools/call that declares the tasks extension, and hands the task the server starts
    /// to the caller.
    static Task<std::variant<CallToolResult, TaskHandle>> start_tool(std::shared_ptr<Impl> state,
                                                                     nlohmann::json params,
                                                                     OutputCheck check,
                                                                     RequestOptions request_options) {
        if (check.refusal) {
            throw *check.refusal;
        }
        std::optional<std::string> tool;
        if (params.contains("name") && params["name"].is_string()) {
            tool = params["name"].get<std::string>();
        }
        const auto route = route_progress(state, params, request_options);
        nlohmann::json reply = co_await request(state, "tools/call", params, request_options, true);
        if (is_task_result(reply)) {
            try {
                co_return std::variant<CallToolResult, TaskHandle>(
                    TaskHandle(state, reply.get<CreateTaskResult>().task, std::move(tool)));
            } catch (const std::exception& error) {
                throw McpError(mcp::constants::jsonrpc::g_INVALID_REQUEST,
                               "Invalid task: " + detail::sanitize_for_diagnostics(error.what()));
            }
        }
        co_return std::variant<CallToolResult, TaskHandle>(finish_call_result(reply, check));
    }

    /// Opens a subscriptions/listen stream. The request is never answered while the stream is open,
    /// so it bypasses the table of pending requests (and their deadline): the stream has its own
    /// entry, and `timeout` limits only the wait for the acknowledgment.
    static Task<Subscription> listen(std::shared_ptr<Impl> state, const SubscriptionFilter& filter,
                                     const RequestOptions& request_options) {
        const auto modern_version = state->modern_version();
        if (!modern_version) {
            throw McpError(mcp::constants::jsonrpc::g_METHOD_NOT_FOUND,
                           "subscriptions/listen needs a session of protocol version 2026-07-28 or "
                           "later");
        }
        const auto timeout = request_options.timeout.value_or(state->options.request_timeout);
        if (timeout <= std::chrono::milliseconds::zero()) {
            throw std::invalid_argument("Request timeout must be positive");
        }

        nlohmann::json params = nlohmann::json::object();
        params["notifications"] = filter;
        const auto wire_params = state->with_request_meta(
            *modern_version, params, request_options.log_level, !filter.taskIds.empty());

        const int64_t id = state->next_request_id.fetch_add(1, std::memory_order_relaxed);
        JSONRPCRequest request;
        request.id = RequestId{std::to_string(id)};
        request.method = "subscriptions/listen";
        request.params = wire_params;
        auto wire = std::make_shared<const std::string>(
            mcp::detail::dump_json(nlohmann::json(std::move(request))));

        auto stream = std::make_shared<detail::SubscriptionState>(
            state->strand, state->options.subscription_queue_limit);
        stream->blocking_loop = state->blocking_loop;
        stream->id = RequestId{std::to_string(id)};
        stream->requested = filter;
        std::weak_ptr<Impl> weak_state = state;
        const auto key = std::make_shared<const std::string>(request_correlation_key(id));
        stream->forget = [weak_state, key] {
            if (auto active = weak_state.lock()) {
                std::lock_guard lock(active->subscriptions_mutex);
                active->subscriptions.erase(*key);
            }
        };
        stream->send_cancel = [weak_state, id]() -> Task<void> {
            auto active = weak_state.lock();
            if (!active || active->closed.load(std::memory_order_acquire)) {
                return nothing_to_send();
            }
            CancelledNotificationParams cancelled;
            cancelled.requestId = RequestId{std::to_string(id)};
            const std::optional<nlohmann::json> cancel_params = nlohmann::json(std::move(cancelled));
            return notification(std::move(active), "notifications/cancelled", cancel_params);
        };

        auto strand = state->strand;
        return detail::spawn_awaitable(
            strand, listen_on_strand(std::move(state), std::move(stream), std::move(wire), timeout));
    }

    static Task<Subscription> listen_on_strand(std::shared_ptr<Impl> state,
                                               std::shared_ptr<detail::SubscriptionState> stream,
                                               std::shared_ptr<const std::string> wire,
                                               std::chrono::milliseconds timeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        {
            std::lock_guard lock(state->subscriptions_mutex);
            state->subscriptions.emplace(stream->id.correlation_key(), stream);
        }
        // The stream is registered before the closed check, so a connection that ends from here on
        // either shows as closed or finds the stream and ends it.
        if (state->closed.load(std::memory_order_acquire)) {
            stream->forget();
            throw McpError(mcp::constants::jsonrpc::g_CONNECTION_CLOSED, "Client transport is closed");
        }

        // Not awaited: on HTTP the write lasts until the answer starts, and the deadline covers that.
        auto write_failure = std::make_shared<std::exception_ptr>();
        boost::asio::co_spawn(state->strand,
                              state->writes_beside_queue() ? state->writer.write_overlapping(wire)
                                                           : state->writer.write_message(wire),
                              [stream, write_failure](std::exception_ptr failure) {
                                  if (failure) {
                                      *write_failure = std::move(failure);
                                      stream->wake.cancel();
                                  }
                              });

        for (;;) {
            if (*write_failure) {
                stream->forget();
                if (state->closed.load(std::memory_order_acquire)) {
                    throw McpError(mcp::constants::jsonrpc::g_CONNECTION_CLOSED,
                                   "Client transport is closed");
                }
                throw_transport_failure(*write_failure);
            }
            if (stream->ack_wait() != detail::SubscriptionState::AckWait::ePending ||
                std::chrono::steady_clock::now() >= deadline) {
                break;
            }
            stream->wake.expires_at(deadline);
            try {
                co_await stream->wake.async_wait(boost::asio::use_awaitable);
            } catch (const boost::system::system_error& error) {
                if (error.code() != boost::asio::error::operation_aborted) {
                    stream->forget();
                    throw;
                }
            }
        }

        const auto outcome = stream->ack_wait();
        if (outcome == detail::SubscriptionState::AckWait::eAcknowledged) {
            co_return Subscription(stream);
        }
        if (outcome == detail::SubscriptionState::AckWait::eEnded) {
            std::optional<Error> failure;
            {
                std::lock_guard lock(stream->mutex);
                failure = stream->failure;
            }
            if (failure) {
                throw McpError(std::move(*failure));
            }
            throw McpError(mcp::constants::jsonrpc::g_INTERNAL_ERROR,
                           "subscriptions/listen ended before it was acknowledged");
        }

        // No acknowledgment in time: end the stream here and tell the server, best effort.
        stream->finish(SubscriptionEnd::eCancelled, std::nullopt);
        stream->forget();
        try {
            co_await stream->send_cancel();
        } catch (...) {
            // The request timeout below is the failure to report.
        }
        throw McpError(mcp::constants::jsonrpc::g_REQUEST_TIMEOUT,
                       "subscriptions/listen was not acknowledged in time");
    }

    /// Routes a notification that names a subscription to that subscription's queue. The
    /// acknowledgment is consumed by the stream; every notification still reaches the handlers
    /// registered with on_notification().
    static void route_to_subscription(const std::shared_ptr<Impl>& state, const std::string& method,
                                      const nlohmann::json& json_message) {
        if (!detail::has_json_value(json_message, "params")) {
            return;
        }
        const auto& params = json_message.at("params");
        if (!params.is_object()) {
            return;
        }
        const auto meta = params.find("_meta");
        if (meta == params.end() || !meta->is_object()) {
            return;
        }
        const auto named = meta->find(std::string(mcp::constants::meta::g_META_SUBSCRIPTION_ID));
        if (named == meta->end() || !detail::is_request_id(*named)) {
            return;
        }
        std::shared_ptr<detail::SubscriptionState> stream;
        {
            std::lock_guard lock(state->subscriptions_mutex);
            const auto found = state->subscriptions.find(named->get<RequestId>().correlation_key());
            if (found == state->subscriptions.end()) {
                return;
            }
            stream = found->second;
        }
        if (method == "notifications/subscriptions/acknowledged") {
            stream->acknowledge(honored_filter(params));
            return;
        }
        stream->push(SubscriptionEvent{method, params});
    }

    /// The answer to a listen request ends its stream: the empty result is the server's graceful
    /// close, an error is a refusal.
    static void end_subscription_from_response(const std::shared_ptr<Impl>& state,
                                               const std::string& id_key,
                                               const nlohmann::json& json_message) {
        std::shared_ptr<detail::SubscriptionState> stream;
        {
            std::lock_guard lock(state->subscriptions_mutex);
            const auto found = state->subscriptions.find(id_key);
            if (found == state->subscriptions.end()) {
                return;
            }
            stream = found->second;
            state->subscriptions.erase(found);
        }
        if (detail::has_json_value(json_message, "error")) {
            try {
                // HttpClientTransport reports a stream that broke as an answer with this code.
                auto error = json_message.at("error").get<Error>();
                const auto end = error.code == mcp::constants::jsonrpc::g_CONNECTION_CLOSED
                                     ? SubscriptionEnd::eDropped
                                     : SubscriptionEnd::eFailed;
                stream->finish(end, std::move(error));
            } catch (const std::exception& decode_error) {
                stream->finish(SubscriptionEnd::eFailed,
                               Error{mcp::constants::jsonrpc::g_INVALID_REQUEST,
                                     "Malformed JSON-RPC error object: " +
                                         detail::sanitize_for_diagnostics(decode_error.what()),
                                     std::nullopt});
            }
            return;
        }
        stream->finish(SubscriptionEnd::eServerClosed, std::nullopt);
    }

    /// On a transport without per-request streams the server ends a subscription it closes with
    /// notifications/cancelled naming the listen request, after the closing result. Returns whether
    /// the notification named an open stream; that stream ends as closed by the server.
    static bool end_subscription_from_cancel(const std::shared_ptr<Impl>& state,
                                             const nlohmann::json& json_message) {
        if (!detail::has_json_value(json_message, "params")) {
            return false;
        }
        const auto& params = json_message.at("params");
        const auto named = params.is_object() ? params.find("requestId") : params.end();
        if (named == params.end() || !detail::is_request_id(*named)) {
            return false;
        }
        std::shared_ptr<detail::SubscriptionState> stream;
        {
            std::lock_guard lock(state->subscriptions_mutex);
            const auto found = state->subscriptions.find(named->get<RequestId>().correlation_key());
            if (found == state->subscriptions.end()) {
                return false;
            }
            stream = found->second;
            state->subscriptions.erase(found);
        }
        stream->finish(SubscriptionEnd::eServerClosed, std::nullopt);
        return true;
    }

    /// The connection ended: every open stream ends without the server's closing result.
    static void end_subscriptions(const std::shared_ptr<Impl>& state, const Error& error) {
        std::map<std::string, std::shared_ptr<detail::SubscriptionState>, std::less<>> open;
        {
            std::lock_guard lock(state->subscriptions_mutex);
            open.swap(state->subscriptions);
        }
        for (auto& [key, stream] : open) {
            static_cast<void>(key);
            stream->finish(SubscriptionEnd::eDropped, error);
        }
    }

    static Task<void> notification(std::shared_ptr<Impl> state, std::string_view method,
                                   const std::optional<nlohmann::json>& params) {
        JSONRPCNotification notification;
        notification.method = std::string(method);
        notification.params = params;
        auto wire = std::make_shared<const std::string>(
            mcp::detail::dump_json(nlohmann::json(std::move(notification))));
        auto strand = state->strand;
        // Keep the closed-state check and write initiation on the client strand.
        return boost::asio::co_spawn(strand, send_notification_wire(std::move(state), std::move(wire)),
                                     boost::asio::use_awaitable);
    }

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

    static bool is_modern_error_code(int code) {
        return code == mcp::constants::jsonrpc::g_UNSUPPORTED_PROTOCOL_VERSION ||
               code == mcp::constants::jsonrpc::g_HEADER_MISMATCH ||
               code == mcp::constants::jsonrpc::g_MISSING_REQUIRED_CLIENT_CAPABILITY;
    }

    static ProbePtr classify_probe_error(const std::shared_ptr<Impl>& state, const Error& error) {
        auto outcome = std::make_shared<ProbeOutcome>();
        outcome->error = error;
        if (state->closed.load(std::memory_order_acquire)) {
            outcome->kind = ProbeOutcome::Kind::eClosed;
        } else if (is_modern_error_code(error.code)) {
            outcome->kind = ProbeOutcome::Kind::eModernError;
        } else if (error.code == mcp::constants::jsonrpc::g_REQUEST_TIMEOUT) {
            outcome->kind = ProbeOutcome::Kind::eTimeout;
        }
        return outcome;
    }

    static ProbePtr classify_probe_reply(const nlohmann::json& reply) {
        auto outcome = std::make_shared<ProbeOutcome>();
        try {
            auto discover = reply;
            if (discover.is_object() && !discover.contains("resultType")) {
                discover["resultType"] = "complete";
            }
            auto parsed = discover.get<DiscoverResult>();
            if (parsed.resultType == "complete") {
                outcome->kind = ProbeOutcome::Kind::eDiscover;
                outcome->discover = std::move(parsed);
            }
        } catch (const std::exception&) {
            // Not a DiscoverResult: whatever answered is not a modern server.
        }
        return outcome;
    }

    /// Sends server/discover naming @p version and waits at most @p timeout for the answer.
    static Task<ProbePtr> probe(std::shared_ptr<Impl> state, VersionPtr version,
                                std::chrono::milliseconds timeout) {
        ProbePtr outcome;
        nlohmann::json reply;
        bool answered = false;
        try {
            RequestOptions options;
            options.timeout = timeout;
            auto params = state->with_request_meta(*version, std::nullopt, std::nullopt);
            auto pending = request_wire(state, "server/discover", params, options, true);
            reply = co_await std::move(pending);
            answered = true;
        } catch (const McpError& error) {
            outcome = classify_probe_error(state, error.error());
        } catch (...) {
            // The write failed: an HTTP server refusing the POST, or the connection going away.
            outcome = std::make_shared<ProbeOutcome>();
            if (state->closed.load(std::memory_order_acquire)) {
                outcome->kind = ProbeOutcome::Kind::eClosed;
            }
        }
        if (answered) {
            outcome = classify_probe_reply(reply);
        }
        co_return outcome;
    }

    /// The versions a protocol error lists in `data.supported`, empty when it lists none.
    static std::vector<std::string> advertised_versions(const Error& error) {
        std::vector<std::string> versions;
        if (error.data && error.data->is_object()) {
            const auto found = error.data->find("supported");
            if (found != error.data->end() && found->is_array()) {
                for (const auto& version : *found) {
                    if (version.is_string()) {
                        versions.push_back(version.get<std::string>());
                    }
                }
            }
        }
        return versions;
    }

    /// The newest version of @p ours (oldest first) that @p theirs also lists, or null.
    template <std::size_t N>
    static VersionPtr newest_mutual(const std::array<std::string_view, N>& ours,
                                    const std::vector<std::string>& theirs) {
        for (auto version = ours.rbegin(); version != ours.rend(); ++version) {
            if (std::find(theirs.begin(), theirs.end(), *version) != theirs.end()) {
                return std::make_shared<const std::string>(*version);
            }
        }
        return nullptr;
    }

    static bool lists_modern_version(const Error& error) {
        const auto versions = advertised_versions(error);
        return std::any_of(versions.begin(), versions.end(), [](const std::string& version) {
            return is_modern_protocol_version(version);
        });
    }

    void report_era(ProtocolEra era, EraReason reason,
                    std::chrono::steady_clock::time_point started) const {
        if (!options.on_era_decision) {
            return;
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started);
        try {
            options.on_era_decision(EraDecision{era, reason, elapsed});
        } catch (...) {
            // Documented: the hook observes; a failure of its own has no listener.
        }
    }

    static std::optional<nlohmann::json> initialize_params(const std::shared_ptr<Impl>& state,
                                                           const std::string& version) {
        InitializeRequest initialize_request;
        initialize_request.protocolVersion = version;
        initialize_request.capabilities = state->effective_capabilities();
        std::lock_guard lock(state->handlers_mutex);
        initialize_request.clientInfo = state->client_info;
        return nlohmann::json(std::move(initialize_request));
    }

    /// Sends initialize naming @p version. The server's refusal is returned, not thrown, so that
    /// the caller can tell a modern server that rejects initialize from a failure.
    static Task<AttemptPtr> attempt_initialize(std::shared_ptr<Impl> state, VersionPtr version,
                                               VersionsPtr advertised) {
        auto attempt = std::make_shared<InitializeAttempt>();
        nlohmann::json result_json;
        bool answered = false;
        try {
            auto params = initialize_params(state, *version);
            auto pending = request(state, "initialize", params, {});
            result_json = co_await std::move(pending);
            answered = true;
        } catch (const McpError& error) {
            // A transport failure, which throw_transport_failure() nests, is no answer from the
            // server, and goes to the caller whole.
            if (state->closed.load(std::memory_order_acquire) ||
                error.code() == mcp::constants::jsonrpc::g_REQUEST_TIMEOUT ||
                dynamic_cast<const std::nested_exception*>(&error) != nullptr) {
                throw;
            }
            attempt->error = error.error();
        }
        if (!answered) {
            co_return attempt;
        }

        auto initialize_result =
            std::make_shared<InitializeResult>(result_json.get<InitializeResult>());
        if (state->options.strict_protocol_validation &&
            !is_legacy_protocol_version(initialize_result->protocolVersion)) {
            request_close(state, "Server selected an unsupported protocol version");
            // The version is whatever the remote server put in its `initialize` result, and
            // this message reaches the application's log. Flatten and bound it, or a server
            // answering with CR/LF forges a log line the application did not write.
            throw McpError(mcp::constants::jsonrpc::g_INVALID_REQUEST,
                           "Server selected unsupported protocol version: " +
                               detail::sanitize_for_diagnostics(initialize_result->protocolVersion));
        }

        co_await notification(state, "notifications/initialized", std::nullopt);

        ConnectResult connected;
        connected.era = ProtocolEra::eLegacy;
        connected.protocolVersion = std::move(initialize_result->protocolVersion);
        connected.capabilities = std::move(initialize_result->capabilities);
        connected.serverInfo = std::move(initialize_result->serverInfo);
        connected.instructions = std::move(initialize_result->instructions);
        connected.supportedVersions = *advertised;
        attempt->result = std::move(connected);
        co_return attempt;
    }

    /// initialize for a caller that has nowhere else to go: a refusal is an error.
    static Task<ConnectResult> initialize_session(std::shared_ptr<Impl> state, VersionPtr version,
                                                  VersionsPtr advertised) {
        auto attempt =
            co_await attempt_initialize(std::move(state), std::move(version), std::move(advertised));
        if (attempt->error) {
            throw McpError(*attempt->error);
        }
        co_return std::move(*attempt->result);
    }

    static ConnectResult modern_result(const DiscoverResult& discover, const std::string& version) {
        ConnectResult connected;
        connected.era = ProtocolEra::eModern;
        connected.protocolVersion = version;
        connected.capabilities = discover.capabilities;
        connected.serverInfo = discover.serverInfo;
        connected.instructions = discover.instructions;
        connected.supportedVersions = discover.supportedVersions;
        return connected;
    }

    /// Settles on the era a DiscoverResult allows: modern when the server lists a modern version
    /// this client speaks, else legacy when it lists a legacy one, else the connect fails.
    static Task<ConnectResult> adopt_discover(std::shared_ptr<Impl> state, ProbePtr outcome,
                                              EraReason reason,
                                              std::chrono::steady_clock::time_point started) {
        const auto modern = newest_mutual(mcp::constants::protocol::g_MODERN_PROTOCOL_VERSIONS,
                                          outcome->discover->supportedVersions);
        if (modern) {
            state->become_modern(*modern);
            state->report_era(ProtocolEra::eModern, reason, started);
            co_return modern_result(*outcome->discover, *modern);
        }
        const auto legacy = newest_mutual(mcp::constants::protocol::g_LEGACY_PROTOCOL_VERSIONS,
                                          outcome->discover->supportedVersions);
        if (!legacy) {
            throw McpError(mcp::constants::jsonrpc::g_UNSUPPORTED_PROTOCOL_VERSION,
                           "The server shares no protocol version with this client",
                           nlohmann::json{{"supported", outcome->discover->supportedVersions}});
        }
        state->report_era(ProtocolEra::eLegacy, reason, started);
        co_return co_await initialize_session(
            state, legacy,
            std::make_shared<const std::vector<std::string>>(outcome->discover->supportedVersions));
    }

    /// Explains a probe that did not come back as a DiscoverResult to a caller that needs one.
    [[noreturn]] static void throw_probe_failure(const ProbeOutcome& outcome,
                                                 const std::string& version) {
        if (outcome.kind == ProbeOutcome::Kind::eModernError) {
            const auto advertised = advertised_versions(*outcome.error);
            if (outcome.error->code != mcp::constants::jsonrpc::g_UNSUPPORTED_PROTOCOL_VERSION ||
                advertised.empty()) {
                throw McpError(*outcome.error);
            }
            std::string listed;
            for (const auto& entry : advertised) {
                listed += (listed.empty() ? "" : ", ") + detail::sanitize_for_diagnostics(entry);
            }
            // A server can refuse a request whose version it lists, for instance when the
            // request is otherwise unacceptable; naming the version as unsupported would then
            // contradict the list.
            const bool listed_by_server =
                std::find(advertised.begin(), advertised.end(), version) != advertised.end();
            if (listed_by_server) {
                throw McpError(mcp::constants::jsonrpc::g_UNSUPPORTED_PROTOCOL_VERSION,
                               "The server refused protocol version " + version +
                                   " although it lists it among the versions it supports (" + listed +
                                   ")",
                               outcome.error->data);
            }
            throw McpError(mcp::constants::jsonrpc::g_UNSUPPORTED_PROTOCOL_VERSION,
                           "The server does not support protocol version " + version +
                               "; it supports " + listed +
                               ". Use ClientProtocol::automatic() to connect with a version both sides "
                               "speak",
                           outcome.error->data);
        }
        if (outcome.kind == ProbeOutcome::Kind::eClosed) {
            throw McpError(mcp::constants::jsonrpc::g_CONNECTION_CLOSED,
                           "The server closed the connection while the client probed it with "
                           "server/discover; a server that exits on an unknown request needs "
                           "ClientProtocol::legacy()");
        }
        const std::string reason = outcome.error
                                       ? ": " + detail::sanitize_for_diagnostics(outcome.error->message)
                                       : " without a usable reply";
        throw McpError(outcome.error ? outcome.error->code : mcp::constants::jsonrpc::g_INVALID_REQUEST,
                       "The server does not speak protocol version " + version +
                           "; server/discover failed" + reason +
                           ". Use ClientProtocol::automatic() or ClientProtocol::legacy() for a server "
                           "that predates it",
                       outcome.error ? outcome.error->data : std::nullopt);
    }

    /// Whether @p outcome refuses @p version as unsupported while listing it as supported.
    static bool refuses_listed_version(const ProbeOutcome& outcome, const std::string& version) {
        if (outcome.kind != ProbeOutcome::Kind::eModernError ||
            outcome.error->code != mcp::constants::jsonrpc::g_UNSUPPORTED_PROTOCOL_VERSION) {
            return false;
        }
        const auto advertised = advertised_versions(*outcome.error);
        return std::find(advertised.begin(), advertised.end(), version) != advertised.end();
    }

    /// ClientProtocol::pinned("2026-07-28"): discover for ConnectResult; never initialize.
    static Task<ConnectResult> connect_pinned_modern(std::shared_ptr<Impl> state,
                                                     std::chrono::steady_clock::time_point started) {
        auto version = std::make_shared<const std::string>(state->options.protocol.version());
        auto outcome = co_await probe(state, version, state->options.request_timeout);
        // A client whose version is refused retries with a version from the server's list that both
        // sides support. For a pinned client that can only be the pinned version, so a refusal that
        // lists it is asked again, once.
        if (refuses_listed_version(*outcome, *version)) {
            outcome = co_await probe(state, version, state->options.request_timeout);
        }
        if (outcome->kind != ProbeOutcome::Kind::eDiscover) {
            throw_probe_failure(*outcome, *version);
        }
        state->become_modern(*version);
        state->report_era(ProtocolEra::eModern, EraReason::ePinned, started);
        co_return modern_result(*outcome->discover, *version);
    }

    /// ClientProtocol::automatic().
    static Task<ConnectResult> connect_auto(std::shared_ptr<Impl> state,
                                            std::chrono::steady_clock::time_point started) {
        const auto no_versions = std::make_shared<const std::vector<std::string>>();
        const auto preferred = std::make_shared<const std::string>(
            mcp::constants::protocol::g_MODERN_PROTOCOL_VERSIONS.back());
        auto outcome = co_await probe(state, preferred, state->options.probe_timeout);
        auto reason = EraReason::eDiscoverOk;

        if (outcome->kind == ProbeOutcome::Kind::eModernError &&
            outcome->error->code == mcp::constants::jsonrpc::g_UNSUPPORTED_PROTOCOL_VERSION) {
            const auto advertised =
                std::make_shared<const std::vector<std::string>>(advertised_versions(*outcome->error));
            const auto modern =
                newest_mutual(mcp::constants::protocol::g_MODERN_PROTOCOL_VERSIONS, *advertised);
            const auto legacy =
                newest_mutual(mcp::constants::protocol::g_LEGACY_PROTOCOL_VERSIONS, *advertised);
            if (modern) {
                // The server is modern and names a version this client speaks: ask again with it.
                reason = EraReason::eModernErrorRetry;
                outcome = co_await probe(state, modern, state->options.probe_timeout);
            } else if (legacy) {
                state->report_era(ProtocolEra::eLegacy, EraReason::eModernErrorRetry, started);
                co_return co_await initialize_session(state, legacy, advertised);
            } else {
                throw McpError(mcp::constants::jsonrpc::g_UNSUPPORTED_PROTOCOL_VERSION,
                               "The server shares no protocol version with this client",
                               outcome->error->data);
            }
        }

        if (outcome->kind == ProbeOutcome::Kind::eDiscover) {
            co_return co_await adopt_discover(state, outcome, reason, started);
        }
        if (outcome->kind == ProbeOutcome::Kind::eModernError ||
            outcome->kind == ProbeOutcome::Kind::eClosed) {
            throw_probe_failure(*outcome, *preferred);
        }

        // Legacy: any other error, a reply that is not JSON, or silence.
        state->report_era(ProtocolEra::eLegacy,
                          outcome->kind == ProbeOutcome::Kind::eTimeout
                              ? EraReason::eFallbackTimeout
                              : EraReason::eFallbackNonModernError,
                          started);
        auto attempt =
            co_await attempt_initialize(state,
                                        std::make_shared<const std::string>(
                                            mcp::constants::protocol::g_LATEST_LEGACY_PROTOCOL_VERSION),
                                        no_versions);
        if (attempt->result) {
            co_return std::move(*attempt->result);
        }

        // A server that answered nothing in time can still be a modern one that was starting up. If
        // it now refuses initialize naming modern versions, probe it again, once.
        if (lists_modern_version(*attempt->error)) {
            auto reprobe = co_await probe(state, preferred, state->options.probe_timeout);
            if (reprobe->kind == ProbeOutcome::Kind::eDiscover) {
                co_return co_await adopt_discover(state, reprobe,
                                                  EraReason::eReprobeAfterInitializeRejected, started);
            }
        }
        throw McpError(*attempt->error);
    }

    static Task<ConnectResult> connect_session(std::shared_ptr<Impl> state) {
        const auto started = std::chrono::steady_clock::now();
        const auto& protocol = state->options.protocol;
        const bool pinned_legacy = protocol.kind() == ClientProtocol::Kind::eLegacy ||
                                   (protocol.kind() == ClientProtocol::Kind::eVersion &&
                                    is_legacy_protocol_version(protocol.version()));
        try {
            if (pinned_legacy) {
                state->report_era(ProtocolEra::eLegacy, EraReason::ePinned, started);
                co_return co_await initialize_session(
                    state, std::make_shared<const std::string>(protocol.version()),
                    std::make_shared<const std::vector<std::string>>());
            }
            if (protocol.kind() == ClientProtocol::Kind::eVersion) {
                co_return co_await connect_pinned_modern(state, started);
            }
            co_return co_await connect_auto(state, started);
        } catch (...) {
            request_close(state, "Client initialization failed");
            throw;
        }
    }

    static Task<void> discard_result(Task<nlohmann::json> request_task) {
        static_cast<void>(co_await std::move(request_task));
    }

    static void start_read_loop(const std::shared_ptr<Impl>& state) {
        boost::asio::co_spawn(state->strand, read_loop(state), boost::asio::detached);
    }

    static void request_close(const std::shared_ptr<Impl>& state, std::string message) {
        if (state->closed.exchange(true, std::memory_order_acq_rel)) {
            return;
        }

        boost::asio::post(state->strand, [state, message = std::move(message)]() mutable {
            fail_pending_requests(state, Error{mcp::constants::jsonrpc::g_CONNECTION_CLOSED,
                                               std::move(message), std::nullopt});
            state->transport->close();
            state->incoming_requests_wake.cancel();
            for (const auto& pause : state->poll_pauses) {
                if (auto timer = pause.lock()) {
                    timer->cancel();
                }
            }
            state->poll_pauses.clear();
        });
    }

    static std::string request_correlation_key(int64_t id) {
        return RequestId{std::to_string(id)}.correlation_key();
    }

    static Task<nlohmann::json> send_request_wire(std::shared_ptr<Impl> state,
                                                  std::shared_ptr<const std::string> wire, int64_t id,
                                                  std::chrono::milliseconds timeout, bool probe,
                                                  bool cancellable,
                                                  std::optional<CancellationToken> cancellation) {
        if (state->closed.load(std::memory_order_acquire)) {
            throw McpError(mcp::constants::jsonrpc::g_CONNECTION_CLOSED, "Client transport is closed");
        }

        // [gcc11-sso: scope-before-await] The correlation key is a short std::string, so it is
        // built in this scope and rebuilt from `id` after the suspension rather than kept across it.
        PendingRequest* pending_request = nullptr;
        CancellationRegistration cancellation_registration;
        {
            auto id_key = request_correlation_key(id);
            {
                std::lock_guard lock(state->pending_requests_mutex);
                auto [iter, inserted] = state->pending_requests.try_emplace(
                    id_key, std::make_unique<PendingRequest>(state->strand, timeout));
                if (!inserted) {
                    throw McpError(mcp::constants::jsonrpc::g_INVALID_REQUEST,
                                   "Duplicate pending request id: " + id_key);
                }
                pending_request = iter->second.get();
                pending_request->probe = probe;
            }
            if (cancellation) {
                const auto& token_state = detail::CancellationAccess::state(*cancellation);
                const auto registration =
                    token_state->add([weak_state = std::weak_ptr<Impl>(state),
                                      id](const std::optional<std::string>& reason) {
                        if (auto owner = weak_state.lock()) {
                            auto strand = owner->strand;
                            boost::asio::post(strand, [owner = std::move(owner), id, reason] {
                                cancel_request(owner, id, reason);
                            });
                        }
                    });
                if (!registration) {
                    // Cancelled before it was sent: nothing reaches the server.
                    erase_pending_request(state, id_key);
                    throw McpError(mcp::constants::jsonrpc::g_REQUEST_CANCELLED, "Request cancelled");
                }
                cancellation_registration = CancellationRegistration(token_state, *registration);
            }

            boost::asio::co_spawn(
                state->strand,
                state->writes_beside_queue()
                    ? state->writer.write_overlapping(std::move(wire))
                    : state->writer.write_message(std::move(wire),
                                                  pending_request->cancel_write_before_start),
                [state, id_key = std::move(id_key)](std::exception_ptr write_error) {
                    complete_request_write(state, id_key, std::move(write_error));
                });
        }

        // The wait ends when the timer fires, when a response or failure signals it, and when the
        // deadline is paused or resumed, which moves the timer. Only the last goes back to waiting.
        do {
            try {
                co_await pending_request->timer.async_wait(boost::asio::use_awaitable);
            } catch (const boost::system::system_error& error) {
                if (error.code() != boost::asio::error::operation_aborted) {
                    erase_pending_request(state, request_correlation_key(id));
                    throw;
                }
            }
        } while (deadline_moved(state, id));

        nlohmann::json result;
        std::optional<Error> error;
        std::exception_ptr write_error;
        bool timed_out = false;
        {
            const auto id_key = request_correlation_key(id);
            std::lock_guard lock(state->pending_requests_mutex);
            auto iter = state->pending_requests.find(id_key);
            if (iter == state->pending_requests.end()) {
                throw McpError(mcp::constants::jsonrpc::g_CONNECTION_CLOSED,
                               "Pending request was removed: " + id_key);
            }
            if (!iter->second->completed) {
                iter->second->cancel_write_before_start->store(true, std::memory_order_release);
                iter->second->error =
                    Error{mcp::constants::jsonrpc::g_REQUEST_TIMEOUT, "Request timed out",
                          nlohmann::json{{"id", std::to_string(id)}}};
                iter->second->completed = true;
                timed_out = true;
            }
            result = std::move(iter->second->result);
            error = std::move(iter->second->error);
            write_error = std::move(iter->second->write_error);
            state->pending_requests.erase(iter);
        }

        // A request the client stops waiting for is cancelled, as the specification asks of a
        // timeout, so the server stops working on it and an HTTP transport closes its response
        // stream. An era probe is given up the same way where that closes a connection; elsewhere
        // the server may predate the era.
        const bool cancels =
            probe ? state->modern_version().has_value() || state->overlapping_writes : cancellable;
        if (timed_out && cancels) {
            send_cancelled(state, id, std::string("Request timed out"));
        }

        if (error) {
            throw McpError(std::move(*error));
        }
        if (write_error) {
            // The era probe tells a refused write from an answer, so it gets the failure as it is.
            if (probe) {
                std::rethrow_exception(write_error);
            }
            throw_transport_failure(write_error);
        }
        co_return result;
    }

    /// Sends `notifications/cancelled` for the request @p id, unless the client is closed. Nothing
    /// waits for the write, and a write that fails changes nothing for the caller.
    static void send_cancelled(const std::shared_ptr<Impl>& state, int64_t id,
                               std::optional<std::string> reason) {
        if (state->closed.load(std::memory_order_acquire)) {
            return;
        }
        CancelledNotificationParams cancel_params;
        cancel_params.requestId = RequestId{std::to_string(id)};
        cancel_params.reason = std::move(reason);
        JSONRPCNotification notification;
        notification.method = "notifications/cancelled";
        notification.params = nlohmann::json(std::move(cancel_params));
        auto cancel_wire = std::make_shared<const std::string>(
            mcp::detail::dump_json(nlohmann::json(std::move(notification))));
        boost::asio::co_spawn(state->strand, send_notification_wire(state, std::move(cancel_wire)),
                              [](const std::exception_ptr&) {});
    }

    /// Runs on the strand when the CancellationToken of the request @p id is cancelled: ends the
    /// request's wait with g_REQUEST_CANCELLED and tells the server, unless it already ended.
    static void cancel_request(const std::shared_ptr<Impl>& state, int64_t id,
                               const std::optional<std::string>& reason) {
        {
            const auto id_key = request_correlation_key(id);
            std::lock_guard lock(state->pending_requests_mutex);
            const auto iter = state->pending_requests.find(id_key);
            if (iter == state->pending_requests.end() || iter->second->completed) {
                return;
            }
            // A write still queued is dropped, and a response that arrives later finds no request.
            iter->second->cancel_write_before_start->store(true, std::memory_order_release);
            iter->second->error =
                Error{mcp::constants::jsonrpc::g_REQUEST_CANCELLED, "Request cancelled"};
            iter->second->completed = true;
            signal_pending_request(*iter->second);
        }
        send_cancelled(state, id, reason);
    }

    /// Waits @p time on the strand, ending early when @p cancellation is cancelled or the client
    /// closes.
    static Task<void> pause_unless_cancelled(std::shared_ptr<Impl> state,
                                             std::chrono::milliseconds time,
                                             std::optional<CancellationToken> cancellation) {
        if (state->closed.load(std::memory_order_acquire)) {
            co_return;
        }
        auto timer = std::make_shared<boost::asio::steady_timer>(state->strand, time);
        std::erase_if(state->poll_pauses, [](const auto& pause) { return pause.expired(); });
        state->poll_pauses.push_back(timer);
        CancellationRegistration cancellation_registration;
        if (cancellation) {
            const auto& token_state = detail::CancellationAccess::state(*cancellation);
            const auto registration =
                token_state->add([weak_timer = std::weak_ptr<boost::asio::steady_timer>(timer),
                                  strand = state->strand](const std::optional<std::string>&) {
                    boost::asio::post(strand, [weak_timer] {
                        if (auto pending = weak_timer.lock()) {
                            pending->cancel();
                        }
                    });
                });
            if (!registration) {
                co_return;
            }
            cancellation_registration = CancellationRegistration(token_state, *registration);
        }
        boost::system::error_code ended;
        co_await timer->async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, ended));
    }

    static Task<void> send_notification_wire(std::shared_ptr<Impl> state,
                                             std::shared_ptr<const std::string> wire) {
        if (state->closed.load(std::memory_order_acquire)) {
            throw McpError(mcp::constants::jsonrpc::g_CONNECTION_CLOSED, "Client transport is closed");
        }

        try {
            if (state->writes_beside_queue()) {
                co_await state->writer.write_overlapping(wire);
            } else {
                co_await state->writer.write_message(wire);
            }
        } catch (...) {
            if (state->closed.load(std::memory_order_acquire)) {
                throw McpError(mcp::constants::jsonrpc::g_CONNECTION_CLOSED,
                               "Client transport is closed");
            }
            throw_transport_failure(std::current_exception());
        }
    }

    /// Two failure classes meet in this loop and only one is fatal. read_message() failing means the
    /// transport can no longer produce messages, so the session ends: pending requests fail and the
    /// transport closes; it is the only statement inside the outer try for that reason. A failure
    /// after it concerns one message already off the wire: it is reported through on_protocol_error
    /// and dropped, and the loop continues.
    static Task<void> read_loop(std::shared_ptr<Impl> state) {
        try {
            for (;;) {
                auto raw = co_await state->transport->read_message();
                dispatch_message(state, raw);
                // The server's requests run in the background, and each holds memory until its
                // answer is written. Reading stops here until they are fewer.
                while ((state->incoming_requests >=
                            mcp::detail::constants::client::g_MAX_INCOMING_REQUESTS ||
                        state->incoming_request_bytes + state->writer.queued_bytes() >=
                            state->options.max_incoming_request_bytes) &&
                       state->incoming_requests != 0 &&
                       !state->closed.load(std::memory_order_acquire)) {
                    state->incoming_requests_wake.expires_at(
                        std::chrono::steady_clock::time_point::max());
                    boost::system::error_code ignored;
                    co_await state->incoming_requests_wake.async_wait(
                        boost::asio::redirect_error(boost::asio::use_awaitable, ignored));
                }
            }
        } catch (const std::exception& error) {
            state->closed.store(true, std::memory_order_release);
            fail_pending_requests(state, Error{mcp::constants::jsonrpc::g_CONNECTION_CLOSED,
                                               "Client read loop stopped: " +
                                                   detail::sanitize_for_diagnostics(error.what()),
                                               std::nullopt});
            state->transport->close();
        } catch (...) {
            state->closed.store(true, std::memory_order_release);
            fail_pending_requests(state, Error{mcp::constants::jsonrpc::g_CONNECTION_CLOSED,
                                               "Client read loop stopped", std::nullopt});
            state->transport->close();
        }
    }

    /// Decodes and routes one received message. Never throws: an unusable message is reported
    /// and dropped so that a single bad message cannot end the session.
    static void dispatch_message(const std::shared_ptr<Impl>& state, const std::string& raw) {
        try {
            if (detail::exceeds_json_depth(raw)) {
                throw McpError(mcp::constants::jsonrpc::g_PARSE_ERROR,
                               "JSON-RPC message nests deeper than " +
                                   std::to_string(mcp::detail::constants::peer_json::g_MAX_DEPTH) +
                                   " levels");
            }
            auto json_message = nlohmann::json::parse(raw);

            if (!json_message.is_object()) {
                throw McpError(mcp::constants::jsonrpc::g_INVALID_REQUEST,
                               "JSON-RPC message must be an object");
            }
            if (state->options.strict_protocol_validation) {
                if (!json_message.contains("jsonrpc") || !json_message.at("jsonrpc").is_string()) {
                    throw McpError(mcp::constants::jsonrpc::g_INVALID_REQUEST,
                                   "JSON-RPC message is missing jsonrpc");
                }
                detail::validate_jsonrpc_version(json_message.at("jsonrpc").get<std::string>());
            }

            const bool has_id = json_message.contains("id");
            const bool has_method = json_message.contains("method");
            if (has_method && !json_message.at("method").is_string()) {
                throw McpError(mcp::constants::jsonrpc::g_INVALID_REQUEST,
                               "JSON-RPC method must be a string");
            }

            if (has_id && !has_method) {
                dispatch_response(state, json_message);
            } else if (has_id && has_method) {
                // dispatch_incoming_request is detached, so anything that throws inside it is
                // swallowed and the peer never hears back. Reject an unusable id here, where the
                // drop is reported, instead of leaving the request silently unanswered.
                const auto& id = json_message.at("id");
                if (!detail::is_request_id(id)) {
                    throw McpError(mcp::constants::jsonrpc::g_INVALID_REQUEST,
                                   "JSON-RPC request id must be a string or an integer within "
                                   "int64_t");
                }
                const auto cost = detail::json_memory_cost(json_message);
                ++state->incoming_requests;
                state->incoming_request_bytes += cost;
                boost::asio::co_spawn(state->strand,
                                      dispatch_incoming_request(state, std::move(json_message)),
                                      [state, cost](const std::exception_ptr&) {
                                          --state->incoming_requests;
                                          state->incoming_request_bytes -= cost;
                                          state->incoming_requests_wake.cancel();
                                      });
            } else if (!has_id && has_method) {
                dispatch_notification(state, json_message);
            }
        } catch (const std::exception& error) {
            fail_probe(state);
            report_protocol_error(state, mcp::constants::jsonrpc::g_PARSE_ERROR,
                                  "Dropped an undecodable message from the peer: " +
                                      detail::sanitize_for_diagnostics(error.what()));
        } catch (...) {
            fail_probe(state);
            report_protocol_error(state, mcp::constants::jsonrpc::g_PARSE_ERROR,
                                  "Dropped an undecodable message from the peer");
        }
    }

    /// A line the client cannot decode while it waits for the era probe's answer is that answer: a
    /// server that replies to server/discover with plain text is not a modern one, and waiting out
    /// the probe timeout would only delay the fallback.
    static void fail_probe(const std::shared_ptr<Impl>& state) {
        std::lock_guard lock(state->pending_requests_mutex);
        for (auto& [id, pending_request] : state->pending_requests) {
            static_cast<void>(id);
            if (pending_request->probe && !pending_request->completed) {
                pending_request->error = Error{mcp::constants::jsonrpc::g_PARSE_ERROR,
                                               "Reply to server/discover is not JSON-RPC"};
                pending_request->completed = true;
                signal_pending_request(*pending_request);
            }
        }
    }

    static void report_protocol_error(const std::shared_ptr<Impl>& state, int code,
                                      std::string message) {
        if (!state->options.on_protocol_error) {
            return;
        }
        try {
            state->options.on_protocol_error(Error{code, std::move(message), std::nullopt});
        } catch (...) {
            // The hook exists to report failures; a failure of the hook itself has no listener.
        }
    }

    static void dispatch_response(const std::shared_ptr<Impl>& state,
                                  const nlohmann::json& json_message) {
        const auto& id = json_message.at("id");
        if (!detail::is_request_id(id)) {
            return;
        }
        const auto id_key = id.get<RequestId>().correlation_key();

        std::unique_lock lock(state->pending_requests_mutex);
        auto iter = state->pending_requests.find(id_key);
        if (iter == state->pending_requests.end()) {
            lock.unlock();
            end_subscription_from_response(state, id_key, json_message);
            return;
        }
        if (std::chrono::steady_clock::now() >= iter->second->deadline) {
            return;
        }

        // Asymmetric on purpose: a null `result` is a legitimate empty result, so presence is the
        // right test for it, while a null `error` is the absence of an error and must not count.
        const bool has_error = detail::has_json_value(json_message, "error");
        const bool has_result = json_message.contains("result");
        if (has_error == has_result) {
            iter->second->error =
                Error{mcp::constants::jsonrpc::g_INVALID_REQUEST,
                      "JSON-RPC response must contain exactly one of result or error", std::nullopt};
        } else if (has_error) {
            // An undecodable `error` member still identifies the request it answers. Fail that
            // request now rather than drop the response and leave the caller waiting out its
            // deadline for an answer that has already arrived.
            try {
                iter->second->error = json_message.at("error").get<Error>();
            } catch (const std::exception& decode_error) {
                iter->second->error = Error{mcp::constants::jsonrpc::g_INVALID_REQUEST,
                                            "Malformed JSON-RPC error object: " +
                                                detail::sanitize_for_diagnostics(decode_error.what()),
                                            std::nullopt};
            }
        } else {
            iter->second->result = json_message.at("result");
        }

        iter->second->completed = true;
        signal_pending_request(*iter->second);
    }

    static void dispatch_notification(const std::shared_ptr<Impl>& state,
                                      const nlohmann::json& json_message) {
        const auto method = json_message.at("method").get<std::string>();
        state->response_cache.invalidate(method, json_message);
        if (method == "notifications/cancelled" && end_subscription_from_cancel(state, json_message)) {
            return;
        }
        route_to_subscription(state, method, json_message);
        if (method == "notifications/progress" && deliver_request_progress(state, json_message)) {
            return;
        }
        // Progress no request in flight claims goes to Client::on_progress, through one bucket for
        // the client; log messages share another.
        if ((method == "notifications/progress" && !state->progress_bucket.take()) ||
            (method == "notifications/message" && !state->log_bucket.take())) {
            state->dropped_notifications.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (method == "notifications/cancelled") {
            if (detail::has_json_value(json_message, "params")) {
                auto params = json_message.at("params").get<CancelledNotificationParams>();
                const auto id_key = params.requestId.correlation_key();

                std::lock_guard lock(state->pending_requests_mutex);
                auto iter = state->pending_requests.find(id_key);
                if (iter != state->pending_requests.end() &&
                    std::chrono::steady_clock::now() < iter->second->deadline) {
                    iter->second->error = Error{mcp::constants::jsonrpc::g_REQUEST_CANCELLED,
                                                "Request cancelled by server"};
                    iter->second->completed = true;
                    signal_pending_request(*iter->second);
                }
            }
            return;
        }

        NotificationCallback callback;
        {
            std::lock_guard lock(state->handlers_mutex);
            auto iter = state->notification_handlers.find(method);
            if (iter != state->notification_handlers.end()) {
                callback = iter->second;
            }
        }
        if (callback) {
            auto params = detail::has_json_value(json_message, "params") ? json_message.at("params")
                                                                         : nlohmann::json::object();
            // The callback is application code running on the read loop. Letting it throw past
            // here would end the session over a bug the application could otherwise handle, so
            // the failure is reported against its method and the session carries on.
            try {
                callback(params);
            } catch (const std::exception& error) {
                report_protocol_error(state, mcp::constants::jsonrpc::g_INTERNAL_ERROR,
                                      "Notification callback for " +
                                          detail::sanitize_for_diagnostics(method) +
                                          " threw: " + detail::sanitize_for_diagnostics(error.what()));
            } catch (...) {
                report_protocol_error(
                    state, mcp::constants::jsonrpc::g_INTERNAL_ERROR,
                    "Notification callback for " + detail::sanitize_for_diagnostics(method) + " threw");
            }
        }
    }

    static Task<void> dispatch_incoming_request(std::shared_ptr<Impl> state,
                                                nlohmann::json json_message) {
        std::string_view method = json_message.at("method").get_ref<const nlohmann::json::string_t&>();

        RequestHandler request_handler;
        {
            std::lock_guard lock(state->handlers_mutex);
            auto iter = state->request_handlers.find(method);
            if (iter != state->request_handlers.end()) {
                request_handler = iter->second;
            }
        }

        if (request_handler) {
            auto params = detail::has_json_value(json_message, "params") ? json_message.at("params")
                                                                         : nlohmann::json::object();
            nlohmann::json error_payload;
            nlohmann::json result;
            try {
                result = co_await request_handler(params);
            } catch (const std::exception& error) {
                error_payload = error.what();
            } catch (...) {
                error_payload = "Request handler failed";
            }

            // An answer whose text is not UTF-8 cannot be encoded. The server is answered anyway,
            // or its request would wait out its whole timeout.
            const auto id = json_message.at("id").get<RequestId>();
            std::string wire;
            try {
                wire = error_payload.is_null()
                           ? make_result_wire(id, std::move(result))
                           : make_error_wire(id, mcp::constants::jsonrpc::g_INTERNAL_ERROR,
                                             error_payload.get<std::string>());
            } catch (const nlohmann::json::exception&) {
                wire = make_error_wire(id, mcp::constants::jsonrpc::g_INTERNAL_ERROR,
                                       "The answer could not be encoded as JSON");
            }
            co_await state->writer.write_message(std::move(wire));
        } else if (method == "ping") {
            co_await state->writer.write_message(
                make_result_wire(json_message.at("id").get<RequestId>(), nlohmann::json::object()));
        } else {
            co_await state->writer.write_message(make_error_wire(
                json_message.at("id").get<RequestId>(), mcp::constants::jsonrpc::g_METHOD_NOT_FOUND,
                "Method not found: " + std::string(method)));
        }
    }

    static std::string make_result_wire(const RequestId& id, nlohmann::json result) {
        JSONRPCResultResponse response;
        response.id = id;
        response.result = std::move(result);
        return mcp::detail::dump_json(nlohmann::json(std::move(response)));
    }

    static std::string make_error_wire(const RequestId& id, int code, std::string message) {
        Error error;
        error.code = code;
        error.message = std::move(message);
        JSONRPCErrorResponse response;
        response.id = id;
        response.error = std::move(error);
        return mcp::detail::dump_json(nlohmann::json(std::move(response)));
    }

    /// Whether request `id` is still waiting on a deadline that has not come: the timer woke because
    /// the deadline was paused or resumed, not because it fired or the request finished.
    static bool deadline_moved(const std::shared_ptr<Impl>& state, int64_t id) {
        const auto id_key = request_correlation_key(id);
        std::lock_guard lock(state->pending_requests_mutex);
        const auto iter = state->pending_requests.find(id_key);
        return iter != state->pending_requests.end() && !iter->second->completed &&
               std::chrono::steady_clock::now() < iter->second->deadline;
    }

    /// The transport says the write of request `key` waits for a person, such as an OAuth consent
    /// prompt. The prompt's time is the user's, so the request's deadline stops until it ends. Runs
    /// on the strand, which owns the request's timer.
    static void on_interaction(const std::shared_ptr<Impl>& state, std::string_view key, bool waiting) {
        std::lock_guard lock(state->pending_requests_mutex);
        const auto iter = state->pending_requests.find(key);
        if (iter == state->pending_requests.end()) {
            return;
        }
        auto& pending = *iter->second;
        const auto now = std::chrono::steady_clock::now();
        if (waiting && !pending.completed && !pending.paused_remaining) {
            pending.paused_remaining = pending.deadline > now
                                           ? pending.deadline - now
                                           : std::chrono::steady_clock::duration::zero();
            pending.deadline = std::chrono::steady_clock::time_point::max();
            pending.timer.expires_at(pending.deadline);
        } else if (!waiting && pending.paused_remaining) {
            pending.deadline = now + *pending.paused_remaining;
            pending.paused_remaining.reset();
            if (!pending.completed) {
                pending.timer.expires_at(pending.deadline);
            }
        }
    }

    static void signal_pending_request(PendingRequest& pending_request) {
        pending_request.timer.expires_at(std::chrono::steady_clock::now());
    }

    static void complete_request_write(const std::shared_ptr<Impl>& state, const std::string& id_key,
                                       std::exception_ptr write_error) {
        if (!write_error) {
            return;
        }

        std::lock_guard lock(state->pending_requests_mutex);
        auto iter = state->pending_requests.find(id_key);
        if (iter == state->pending_requests.end() || iter->second->completed ||
            std::chrono::steady_clock::now() >= iter->second->deadline) {
            return;
        }

        if (state->closed.load(std::memory_order_acquire)) {
            iter->second->error =
                Error{mcp::constants::jsonrpc::g_CONNECTION_CLOSED, "Client transport is closed"};
        } else {
            iter->second->write_error = std::move(write_error);
        }
        iter->second->completed = true;
        signal_pending_request(*iter->second);
    }

    static void erase_pending_request(const std::shared_ptr<Impl>& state, const std::string& id_key) {
        std::lock_guard lock(state->pending_requests_mutex);
        state->pending_requests.erase(id_key);
    }

    static void fail_pending_requests(const std::shared_ptr<Impl>& state, const Error& error) {
        end_subscriptions(state, error);
        std::lock_guard lock(state->pending_requests_mutex);
        for (auto& [id, pending_request] : state->pending_requests) {
            static_cast<void>(id);
            if (!pending_request->completed) {
                pending_request->error = error;
                pending_request->completed = true;
            }
            signal_pending_request(*pending_request);
        }
    }

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

    void become_modern(const std::string& version) {
        std::lock_guard lock(era_mutex);
        modern_protocol_version = version;
    }

    std::optional<std::string> modern_version() const {
        std::lock_guard lock(era_mutex);
        return modern_protocol_version;
    }

    /// What this client tells a server it can do: the capabilities given to connect(), plus what
    /// its registered handlers imply. A legacy session sends it once, in initialize; a modern one
    /// reads it afresh for every request, so a handler registered after connect() counts from the
    /// next request on.
    ClientCapabilities effective_capabilities() const {
        std::lock_guard lock(handlers_mutex);
        auto capabilities = client_capabilities;
        if (request_handlers.contains("roots/list") && !capabilities.roots) {
            capabilities.roots = ClientCapabilities::RootsCapability{};
        }
        if (request_handlers.contains("sampling/createMessage") && !capabilities.sampling) {
            capabilities.sampling = ClientCapabilities::SamplingCapability{};
        }
        if (request_handlers.contains("elicitation/create") && !capabilities.elicitation) {
            capabilities.elicitation =
                ClientCapabilities::ElicitationCapability{nlohmann::json::object(), std::nullopt};
        }
        return capabilities;
    }

    /// @p params with the per-request `_meta` of a modern request merged in. The caller's own
    /// `_meta` entries (progressToken, traceparent, ...) stay as they are; the four protocol keys
    /// are the client's to set.
    /// @p declare_tasks adds the tasks extension to the declared capabilities, which the client does
    /// for every request when ClientOptions::tasks is set.
    std::optional<nlohmann::json> with_request_meta(const std::string& version,
                                                    const std::optional<nlohmann::json>& params,
                                                    const std::optional<LoggingLevel>& level,
                                                    bool declare_tasks = false) const {
        nlohmann::json merged = params ? *params : nlohmann::json::object();
        if (merged.is_null()) {
            merged = nlohmann::json::object();
        }
        if (!merged.is_object()) {
            throw McpError(mcp::constants::jsonrpc::g_INVALID_PARAMS,
                           "Request params must be an object");
        }
        nlohmann::json meta = nlohmann::json::object();
        if (const auto found = merged.find("_meta"); found != merged.end() && !found->is_null()) {
            if (!found->is_object()) {
                throw McpError(mcp::constants::jsonrpc::g_INVALID_PARAMS,
                               "Request _meta must be an object");
            }
            meta = *found;
        }
        meta[std::string(mcp::constants::meta::g_META_PROTOCOL_VERSION)] = version;
        meta[std::string(mcp::constants::meta::g_META_CLIENT_CAPABILITIES)] = effective_capabilities();
        if (declare_tasks || options.tasks == ClientTaskMode::eAuto) {
            auto& extensions =
                meta[std::string(mcp::constants::meta::g_META_CLIENT_CAPABILITIES)]["extensions"];
            if (!extensions.is_object()) {
                extensions = nlohmann::json::object();
            }
            if (!extensions.contains(std::string(mcp::constants::tasks::g_EXTENSION_ID))) {
                extensions[std::string(mcp::constants::tasks::g_EXTENSION_ID)] =
                    nlohmann::json::object();
            }
        }
        {
            std::lock_guard lock(handlers_mutex);
            meta[std::string(mcp::constants::meta::g_META_CLIENT_INFO)] = client_info;
        }
        if (level) {
            meta[std::string(mcp::constants::meta::g_META_LOG_LEVEL)] = *level;
        }
        merged["_meta"] = std::move(meta);
        return merged;
    }

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
    OutputCheck output_check_for(std::string_view name) const {
        if (!options.validate_tool_schemas) {
            return {};
        }
        std::lock_guard lock(listed_tools_mutex);
        const auto found = listed_tools.find(name);
        if (found == listed_tools.end()) {
            return {};
        }
        // A pattern this client cannot run says nothing about the server, so such a result goes
        // unchecked. Any other problem means the server declared a schema no client can use.
        const auto& problem = found->second.output_problem;
        if (problem && problem->category() != SchemaError::Category::eUnsupportedPattern) {
            return {std::nullopt, problem};
        }
        return {found->second.output_schema, std::nullopt};
    }

    /// Compiles @p tool's output schema and remembers it; the problem when it cannot be used. The
    /// input schema is the server's to enforce, so nothing here depends on it.
    std::optional<SchemaError> remember_tool(const Tool& tool) {
        ListedTool listed{
            tool.outputSchema ? std::optional<std::string>(tool.outputSchema->dump()) : std::nullopt,
            std::nullopt, std::nullopt};
        {
            std::lock_guard lock(listed_tools_mutex);
            const auto found = listed_tools.find(tool.name);
            if (found != listed_tools.end() && found->second.output_text == listed.output_text) {
                return found->second.output_problem;
            }
        }
        try {
            if (tool.outputSchema) {
                listed.output_schema = CompiledSchema::compile(*tool.outputSchema);
            }
        } catch (const SchemaError& error) {
            listed.output_problem =
                SchemaError(error.category(), std::string("outputSchema: ") + error.what());
        }
        auto problem = listed.output_problem;
        std::lock_guard lock(listed_tools_mutex);
        listed_tools.insert_or_assign(tool.name, std::move(listed));
        return problem;
    }

    /// Remembers the output schemas of @p result, reporting those that cannot be used. Every tool
    /// stays listed: whether a tool can be called is the server's to say.
    void accept_listed_tools(const ListToolsResult& result) {
        if (!options.validate_tool_schemas) {
            return;
        }
        for (const auto& tool : result.tools) {
            const auto problem = remember_tool(tool);
            if (problem && options.on_tool_schema_error) {
                try {
                    options.on_tool_schema_error(tool, *problem);
                } catch (...) {
                    // Documented: the callback reports; a failure of its own has no listener.
                }
            }
        }
    }

    static Task<ListToolsResult> accepted_tools(std::shared_ptr<Impl> state,
                                                Task<ListToolsResult> request) {
        auto result = co_await std::move(request);
        state->accept_listed_tools(result);
        co_return result;
    }
};

struct Client::OwnedLoop {
    /// An io_context whose pending handlers can be destroyed before it is.
    class Context final : public boost::asio::io_context {
       public:
        using boost::asio::execution_context::shutdown;
    };

    Context io;
    /// How long the destructor waits for the loop to finish.
    std::chrono::milliseconds close_wait{mcp::constants::client::g_CLOSE_WAIT_MS};
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work{io.get_executor()};
    std::promise<void> finished;
    std::future<void> finished_future = finished.get_future();
    std::thread thread;

    OwnedLoop() {
        thread = std::thread([this] {
            for (;;) {
                try {
                    io.run();
                    break;
                } catch (...) {
                    // A handler threw: the client's own work catches its errors, so this is one of
                    // the application's callbacks. The loop keeps running for the others.
                }
            }
            finished.set_value();
        });
    }

    OwnedLoop(const OwnedLoop&) = delete;
    OwnedLoop& operator=(const OwnedLoop&) = delete;
    OwnedLoop(OwnedLoop&&) = delete;
    OwnedLoop& operator=(OwnedLoop&&) = delete;
    ~OwnedLoop() = default;
};

namespace {

/// @throws std::invalid_argument When @p options holds a value a client cannot work with.
void check_options(const ClientOptions& options) {
    if (options.request_timeout <= std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("Client request timeout must be positive");
    }
    if (options.probe_timeout <= std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("Client probe timeout must be positive");
    }
    if (options.subscription_queue_limit == 0) {
        throw std::invalid_argument("Client subscription queue limit must be positive");
    }
    if (options.task_poll_interval <= std::chrono::milliseconds::zero() ||
        options.task_timeout <= std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("Client task poll interval and task timeout must be positive");
    }
    if (options.max_incoming_request_bytes == 0) {
        throw std::invalid_argument("Client incoming request memory limit must be positive");
    }
    detail::check_rate_limit(options.notification_rate_limit);
    if (options.response_cache.enabled &&
        (options.response_cache.max_entries == 0 ||
         options.response_cache.max_ttl <= std::chrono::milliseconds::zero())) {
        throw std::invalid_argument("Client response cache entry limit and lifetime must be positive");
    }
}

}  // namespace

Client::Client(std::shared_ptr<ITransport> transport, const Executor& executor, ClientOptions options) {
    start(std::move(transport), executor, std::move(options));
}

Client::Client(const Endpoint& endpoint, ClientOptions options) : loop_(std::make_shared<OwnedLoop>()) {
    if (endpoint.close_wait_ > std::chrono::milliseconds::zero()) {
        loop_->close_wait = std::max(
            loop_->close_wait, endpoint.close_wait_ + detail::constants::stdio_client::g_KILL_WAIT);
    }
    try {
        // Before the transport, which may start the server's process.
        check_options(options);
        const Executor executor = loop_->io.get_executor();
        start(endpoint.make_transport(executor), executor, std::move(options));
        impl_->blocking_loop = std::make_shared<const detail::BlockingLoop>(
            loop_, loop_->io.get_executor(), loop_->thread.get_id());
    } catch (...) {
        loop_->work.reset();
        loop_->io.stop();
        loop_->thread.join();
        throw;
    }
}

void Client::start(std::shared_ptr<ITransport> transport, const Executor& executor,
                   ClientOptions options) {
    if (!transport) {
        throw std::invalid_argument("Client transport must not be null");
    }
    check_options(options);
    impl_ = std::make_shared<Impl>(std::move(transport), executor.get(), std::move(options));
    impl_->executor = executor;
    impl_->transport->set_interaction_observer(
        [state = std::weak_ptr<Impl>(impl_)](std::string_view key, bool waiting) {
            if (const auto alive = state.lock()) {
                // A transport may report from any thread; the request's timer belongs to the strand.
                boost::asio::dispatch(alive->strand, [alive, key = std::string(key), waiting] {
                    Impl::on_interaction(alive, key, waiting);
                });
            }
        });
}

Executor Client::executor() const { return impl_->executor; }

namespace {

/// The params of a tools/call with JSON arguments; `{}` makes null ones, which are sent as an empty
/// object.
CallToolParams json_call_params(const std::string& name, const nlohmann::json& arguments) {
    CallToolParams call_params;
    call_params.name = name;
    call_params.arguments = arguments.is_null() ? nlohmann::json::object() : arguments;
    return call_params;
}

}  // namespace

Task<CallToolResult> Client::call_tool(const std::string& name, const nlohmann::json& arguments) {
    // [gcc11-sso: not-a-coroutine]
    return call_tool_with(json_call_params(name, arguments));
}

Task<CallToolResult> Client::call_tool(const std::string& name, const nlohmann::json& arguments,
                                       const RequestOptions& request_options) {
    // [gcc11-sso: not-a-coroutine]
    return call_tool_with(json_call_params(name, arguments), request_options);
}

Task<std::variant<CallToolResult, TaskHandle>> Client::start_tool(const std::string& name,
                                                                  const nlohmann::json& arguments) {
    // [gcc11-sso: not-a-coroutine]
    return start_tool_with(json_call_params(name, arguments));
}

Task<std::variant<CallToolResult, TaskHandle>> Client::start_tool(
    const std::string& name, const nlohmann::json& arguments, const RequestOptions& request_options) {
    // [gcc11-sso: not-a-coroutine]
    return start_tool_with(json_call_params(name, arguments), request_options);
}

Client::~Client() {
    close();
    if (!loop_) {
        return;
    }
    if (loop_->thread.get_id() == std::this_thread::get_id()) {
        // The thread cannot join itself, and the loop it would wait for is the one it runs.
        std::fputs(
            "mcp::Client: a client with its own event loop was destroyed on that loop's "
            "thread\n",
            stderr);
        std::terminate();
    }
    loop_->work.reset();
    if (loop_->finished_future.wait_for(loop_->close_wait) != std::future_status::ready) {
        loop_->io.stop();
    }
    loop_->thread.join();
    if (impl_->blocking_loop) {
        impl_->blocking_loop->mark_ended();
    }
    // The handlers a stopped loop did not run hold the client's state, which holds the loop:
    // destroying them frees both.
    loop_->io.shutdown();
    impl_.reset();
}

Task<ConnectResult> Client::connect(std::string_view name, std::string_view version) {
    Implementation info;
    info.name = std::string(name);
    info.version = std::string(version);
    return connect(info, {});
}

Task<ConnectResult> Client::connect(const Implementation& client_info,
                                    const ClientCapabilities& capabilities) {
    auto state = impl_;
    if (state->closed.load(std::memory_order_acquire)) {
        throw McpError(mcp::constants::jsonrpc::g_CONNECTION_CLOSED, "Client is closed");
    }
    if (state->read_loop_started.exchange(true, std::memory_order_acq_rel)) {
        throw McpError(mcp::constants::jsonrpc::g_INVALID_REQUEST,
                       "Client has already been initialized");
    }

    Impl::start_read_loop(state);
    {
        std::lock_guard lock(state->handlers_mutex);
        state->client_info = client_info;
        state->client_capabilities = capabilities;
    }
    return Impl::connect_session(std::move(state));
}

Task<nlohmann::json> Client::send_request(std::string_view method,
                                          const std::optional<nlohmann::json>& params) {
    return send_request(method, params, {});
}

Task<nlohmann::json> Client::send_request(std::string_view method,
                                          const std::optional<nlohmann::json>& params,
                                          const RequestOptions& request_options) {
    if (!impl_->read_loop_started.load(std::memory_order_acquire)) {
        throw McpError(mcp::constants::jsonrpc::g_INVALID_REQUEST,
                       "Client must connect before sending requests");
    }
    return Impl::request(impl_, method, params, request_options);
}

Task<Subscription> Client::listen(const SubscriptionFilter& filter) {
    return listen(filter, RequestOptions{});
}

Task<Subscription> Client::listen(const SubscriptionFilter& filter,
                                  const RequestOptions& request_options) {
    if (!impl_->read_loop_started.load(std::memory_order_acquire)) {
        throw McpError(mcp::constants::jsonrpc::g_INVALID_REQUEST,
                       "Client must connect before sending requests");
    }
    return Impl::listen(impl_, filter, request_options);
}

Task<void> Client::send_notification(std::string_view method,
                                     const std::optional<nlohmann::json>& params) {
    if (!impl_->read_loop_started.load(std::memory_order_acquire)) {
        throw McpError(mcp::constants::jsonrpc::g_INVALID_REQUEST,
                       "Client must connect before sending notifications");
    }
    return Impl::notification(impl_, method, params);
}

void Client::clear_response_cache() { impl_->response_cache.clear(); }

std::uint64_t Client::dropped_notifications() const {
    return impl_->dropped_notifications.load(std::memory_order_relaxed);
}

std::size_t Client::pending_request_count() const {
    std::lock_guard lock(impl_->pending_requests_mutex);
    return impl_->pending_requests.size();
}

void Client::close() {
    if (impl_) {
        Impl::request_close(impl_, "Client transport closed");
    }
}

Task<ListToolsResult> Client::list_tools(const std::optional<std::string>& cursor) {
    return list_tools(cursor, RequestOptions{});
}

Task<ListToolsResult> Client::list_tools(const std::optional<std::string>& cursor,
                                         const RequestOptions& request_options) {
    // [gcc11-sso: not-a-coroutine]
    return Impl::accepted_tools(
        impl_, parse_result<ListToolsResult>(Impl::cacheable_request(
                   impl_, "tools/list", make_paginated_params(cursor), request_options)));
}

Task<CallToolResult> Client::call_tool_with(CallToolParams params) {
    return call_tool_with(std::move(params), RequestOptions{});
}

Task<CallToolResult> Client::call_tool_with(CallToolParams params,
                                            const RequestOptions& request_options) {
    // [gcc11-sso: not-a-coroutine]
    auto check = impl_->output_check_for(params.name);
    if (impl_->options.tasks == ClientTaskMode::eAuto && impl_->modern_version()) {
        if (!impl_->read_loop_started.load(std::memory_order_acquire)) {
            throw McpError(mcp::constants::jsonrpc::g_INVALID_REQUEST,
                           "Client must connect before sending requests");
        }
        return Impl::call_tool_following(impl_, nlohmann::json(std::move(params)), std::move(check),
                                         request_options);
    }
    return checked_call_result(
        send_request("tools/call", nlohmann::json(std::move(params)), request_options),
        std::move(check));
}

Task<std::variant<CallToolResult, TaskHandle>> Client::start_tool_with(CallToolParams params) {
    return start_tool_with(std::move(params), RequestOptions{});
}

Task<std::variant<CallToolResult, TaskHandle>> Client::start_tool_with(
    CallToolParams params, const RequestOptions& request_options) {
    // [gcc11-sso: not-a-coroutine]
    auto check = impl_->output_check_for(params.name);
    if (!impl_->read_loop_started.load(std::memory_order_acquire)) {
        throw McpError(mcp::constants::jsonrpc::g_INVALID_REQUEST,
                       "Client must connect before sending requests");
    }
    return Impl::start_tool(impl_, nlohmann::json(std::move(params)), std::move(check),
                            request_options);
}

TaskHandle Client::task_handle(std::string id) {
    TaskData created;
    created.taskId = std::move(id);
    return TaskHandle(impl_, std::move(created), std::nullopt);
}

TaskHandle::TaskHandle(std::shared_ptr<void> client, TaskData created, std::optional<std::string> tool)
    : client_(std::move(client)), created_(std::move(created)), tool_(std::move(tool)) {}

namespace {

std::shared_ptr<void> attached_client(const std::shared_ptr<void>& client) {
    if (!client) {
        throw McpError(mcp::constants::jsonrpc::g_INVALID_REQUEST,
                       "The task handle is not attached to a task");
    }
    return client;
}

Task<DetailedTask> detailed_task(Task<nlohmann::json> finished) {
    auto reply = co_await std::move(finished);
    co_return decode_task_state(reply);
}

Task<CallToolResult> task_result(Task<nlohmann::json> finished, OutputCheck check) {
    auto reply = co_await std::move(finished);
    co_return finish_call_result(task_outcome(reply), check);
}

}  // namespace

Task<DetailedTask> TaskHandle::get(const RequestOptions& request_options) const {
    // [gcc11-sso: not-a-coroutine]
    auto state = std::static_pointer_cast<Client::Impl>(attached_client(client_));
    nlohmann::json params{{"taskId", created_.taskId}};
    return Client::Impl::task_state(std::move(state), std::move(params), request_options);
}

Task<void> TaskHandle::update(const nlohmann::json& input_responses,
                              const RequestOptions& request_options) const {
    auto state = std::static_pointer_cast<Client::Impl>(attached_client(client_));
    nlohmann::json params{{"taskId", created_.taskId}, {"inputResponses", input_responses}};
    return Client::Impl::task_ack(std::move(state), "tasks/update", std::move(params), request_options);
}

Task<void> TaskHandle::cancel(const RequestOptions& request_options) const {
    auto state = std::static_pointer_cast<Client::Impl>(attached_client(client_));
    nlohmann::json params{{"taskId", created_.taskId}};
    return Client::Impl::task_ack(std::move(state), "tasks/cancel", std::move(params), request_options);
}

Task<DetailedTask> TaskHandle::wait(const RequestOptions& request_options) const {
    auto state = std::static_pointer_cast<Client::Impl>(attached_client(client_));
    nlohmann::json params{{"taskId", created_.taskId}};
    auto finished = Client::Impl::wait_for_task(std::move(state), std::move(params), request_options);
    return detailed_task(std::move(finished));
}

Task<CallToolResult> TaskHandle::result(const RequestOptions& request_options) const {
    auto state = std::static_pointer_cast<Client::Impl>(attached_client(client_));
    auto check = tool_ ? state->output_check_for(*tool_) : OutputCheck{};
    if (check.refusal) {
        throw *check.refusal;
    }
    nlohmann::json params{{"taskId", created_.taskId}};
    auto finished = Client::Impl::wait_for_task(std::move(state), std::move(params), request_options);
    return task_result(std::move(finished), std::move(check));
}

Task<ListResourcesResult> Client::list_resources(const std::optional<std::string>& cursor) {
    return list_resources(cursor, RequestOptions{});
}

Task<ListResourcesResult> Client::list_resources(const std::optional<std::string>& cursor,
                                                 const RequestOptions& request_options) {
    return parse_result<ListResourcesResult>(Impl::cacheable_request(
        impl_, "resources/list", make_paginated_params(cursor), request_options));
}

Task<ReadResourceResult> Client::read_resource(const std::string& uri) {
    return read_resource(uri, RequestOptions{});
}

Task<ReadResourceResult> Client::read_resource(const std::string& uri,
                                               const RequestOptions& request_options) {
    ReadResourceRequestParams params;
    params.uri = uri;
    return parse_result<ReadResourceResult>(Impl::cacheable_request(
        impl_, "resources/read", nlohmann::json(std::move(params)), request_options));
}

Task<ListResourceTemplatesResult> Client::list_resource_templates(
    const std::optional<std::string>& cursor) {
    return list_resource_templates(cursor, RequestOptions{});
}

Task<ListResourceTemplatesResult> Client::list_resource_templates(
    const std::optional<std::string>& cursor, const RequestOptions& request_options) {
    return parse_result<ListResourceTemplatesResult>(Impl::cacheable_request(
        impl_, "resources/templates/list", make_paginated_params(cursor), request_options));
}

Task<ListPromptsResult> Client::list_prompts(const std::optional<std::string>& cursor) {
    return list_prompts(cursor, RequestOptions{});
}

Task<ListPromptsResult> Client::list_prompts(const std::optional<std::string>& cursor,
                                             const RequestOptions& request_options) {
    return parse_result<ListPromptsResult>(
        Impl::cacheable_request(impl_, "prompts/list", make_paginated_params(cursor), request_options));
}

Task<GetPromptResult> Client::get_prompt(const std::string& name,
                                         const std::map<std::string, std::string>& arguments) {
    return get_prompt(name, arguments, RequestOptions{});
}

Task<GetPromptResult> Client::get_prompt(const std::string& name,
                                         const std::map<std::string, std::string>& arguments,
                                         const RequestOptions& request_options) {
    GetPromptRequestParams params;
    params.name = name;
    if (!arguments.empty()) {
        params.arguments = arguments;
    }
    return parse_result<GetPromptResult>(
        send_request("prompts/get", nlohmann::json(std::move(params)), request_options));
}

Task<CompleteResult> Client::complete(const CompleteParams& params) {
    return complete(params, RequestOptions{});
}

Task<CompleteResult> Client::complete(const CompleteParams& params,
                                      const RequestOptions& request_options) {
    return parse_result<CompleteResult>(
        send_request("completion/complete", nlohmann::json(params), request_options));
}

Task<void> Client::ping() {
    if (!impl_->read_loop_started.load(std::memory_order_acquire)) {
        throw McpError(mcp::constants::jsonrpc::g_INVALID_REQUEST,
                       "Client must connect before sending requests");
    }
    return Impl::discard_result(Impl::request(impl_, "ping", std::nullopt, {}));
}

Task<void> Client::cancel(const RequestId& request_id, const std::optional<std::string>& reason) {
    // [gcc11-sso: not-a-coroutine]
    if (impl_->modern_version()) {
        // A 2026-07-28 server sends nothing more for a cancelled request, so the caller stops
        // waiting now, and a response that arrives anyway finds no request and is dropped.
        std::lock_guard lock(impl_->pending_requests_mutex);
        const auto iter = impl_->pending_requests.find(request_id.correlation_key());
        if (iter != impl_->pending_requests.end() && !iter->second->completed) {
            iter->second->cancel_write_before_start->store(true, std::memory_order_release);
            iter->second->error =
                Error{mcp::constants::jsonrpc::g_REQUEST_CANCELLED, "Request cancelled"};
            iter->second->completed = true;
            Impl::signal_pending_request(*iter->second);
        }
    }
    CancelledNotificationParams params;
    params.requestId = request_id;
    params.reason = reason;
    return send_notification("notifications/cancelled", nlohmann::json(std::move(params)));
}

void Client::on_notification(const std::string& method, NotificationCallback callback) {
    std::lock_guard lock(impl_->handlers_mutex);
    impl_->notification_handlers[method] = std::move(callback);
}

void Client::on_progress(ProgressCallback callback) {
    on_notification("notifications/progress",
                    [callback = std::move(callback)](const nlohmann::json& params) {
                        ProgressNotificationParams progress;
                        try {
                            params.get_to(progress);
                        } catch (const std::exception& error) {
                            // Reported through ClientOptions::on_protocol_error by the read loop.
                            // Naming the cause here separates a notification the peer sent wrong
                            // from a bug in the application's own progress callback.
                            throw McpError(mcp::constants::jsonrpc::g_INVALID_PARAMS,
                                           "Malformed progress notification params: " +
                                               detail::sanitize_for_diagnostics(error.what()));
                        }
                        callback(progress);
                    });
}

void Client::on_request(const std::string& method, RequestHandler handler) {
    std::lock_guard lock(impl_->handlers_mutex);
    impl_->request_handlers[method] = std::move(handler);
}

void Client::on_elicitation(std::function<Task<ElicitResult>(const ElicitRequestParams&)> handler) {
    const bool validate = impl_->options.validate_elicitation_content;
    on_request(
        "elicitation/create",
        [handler = std::move(handler), validate](const nlohmann::json& params) -> Task<nlohmann::json> {
            auto result = co_await handler(params.get<ElicitRequestParams>());
            if (validate) {
                check_elicitation_content(params, result);
            }
            co_return nlohmann::json(std::move(result));
        });
}

void Client::on_sampling(
    std::function<Task<CreateMessageResult>(const CreateMessageRequestParams&)> handler) {
    on_request("sampling/createMessage",
               [handler = std::move(handler)](const nlohmann::json& params) -> Task<nlohmann::json> {
                   auto result = co_await handler(params.get<CreateMessageRequestParams>());
                   co_return nlohmann::json(std::move(result));
               });
}

void Client::set_roots(const std::vector<Root>& roots, bool notify) {
    auto state = impl_;
    bool should_notify = false;
    {
        std::lock_guard lock(state->handlers_mutex);
        state->roots = roots;
        if (!state->request_handlers.contains("roots/list")) {
            std::weak_ptr<Impl> weak_state = state;
            state->request_handlers.emplace(
                "roots/list", [weak_state](const nlohmann::json&) -> Task<nlohmann::json> {
                    auto active_state = weak_state.lock();
                    if (!active_state) {
                        throw McpError(mcp::constants::jsonrpc::g_CONNECTION_CLOSED,
                                       "Client is closed");
                    }

                    ListRootsResult result;
                    {
                        std::lock_guard roots_lock(active_state->handlers_mutex);
                        result.roots = active_state->roots;
                    }
                    co_return nlohmann::json(std::move(result));
                });
        }

        should_notify = notify && state->client_capabilities.roots.has_value() &&
                        state->client_capabilities.roots->listChanged.value_or(false);
    }

    // A modern session has no roots notification: the server learns the roots from each request.
    if (should_notify && !state->modern_version() && !state->closed.load(std::memory_order_acquire)) {
        boost::asio::co_spawn(
            state->strand, Impl::notification(state, "notifications/roots/list_changed", std::nullopt),
            boost::asio::detached);
    }
}

void Client::on_roots_list(std::function<Task<ListRootsResult>(const nlohmann::json&)> handler) {
    on_request("roots/list",
               [handler = std::move(handler)](const nlohmann::json& params) -> Task<nlohmann::json> {
                   auto result = co_await handler(params);
                   co_return nlohmann::json(std::move(result));
               });
}

SyncClient Client::sync() {
    if (!impl_->blocking_loop) {
        throw std::logic_error(
            "mcp::Client::sync() needs a client constructed from an Endpoint; a client on the "
            "application's executor is used through its coroutines");
    }
    return SyncClient(*this);
}

void Client::run(std::function<Task<void>()> body) {
    if (!body) {
        throw std::invalid_argument("mcp::Client::run() needs a coroutine");
    }
    detail::run_blocking<void>(impl_->blocking_loop, [body = std::move(body)]() -> Task<void> {
        Task<void> task = body();
        if (!task.valid()) {
            throw std::invalid_argument(
                "mcp::Client::run() needs a coroutine; the function returned an empty Task");
        }
        co_await std::move(task);
    });
}

SyncSubscription Subscription::sync() const {
    if (!state_ || !state_->blocking_loop) {
        throw std::logic_error(
            "mcp::Subscription::sync() needs a subscription of a client constructed from an "
            "Endpoint");
    }
    return SyncSubscription(*this);
}

SyncSubscription::SyncSubscription(Subscription subscription)
    : subscription_(std::move(subscription)) {}

std::optional<SubscriptionEvent> SyncSubscription::next() {
    return detail::run_blocking<std::optional<SubscriptionEvent>>(
        subscription_.state_->blocking_loop, [this] { return subscription_.next(); });
}

void SyncSubscription::close() {
    detail::run_blocking<void>(subscription_.state_->blocking_loop,
                               [this] { return subscription_.close(); });
}

std::shared_ptr<const detail::BlockingLoop> TaskHandle::blocking_loop() const {
    return std::static_pointer_cast<Client::Impl>(attached_client(client_))->blocking_loop;
}

SyncTaskHandle TaskHandle::sync() const {
    if (!blocking_loop()) {
        throw std::logic_error(
            "mcp::TaskHandle::sync() needs a task of a client constructed from an Endpoint");
    }
    return SyncTaskHandle(*this);
}

SyncTaskHandle::SyncTaskHandle(TaskHandle handle) : handle_(std::move(handle)) {}

DetailedTask SyncTaskHandle::get(const RequestOptions& request_options) const {
    return detail::run_blocking<DetailedTask>(handle_.blocking_loop(),
                                              [&] { return handle_.get(request_options); });
}

void SyncTaskHandle::update(const nlohmann::json& input_responses,
                            const RequestOptions& request_options) const {
    detail::run_blocking<void>(handle_.blocking_loop(),
                               [&] { return handle_.update(input_responses, request_options); });
}

void SyncTaskHandle::cancel(const RequestOptions& request_options) const {
    detail::run_blocking<void>(handle_.blocking_loop(),
                               [&] { return handle_.cancel(request_options); });
}

DetailedTask SyncTaskHandle::wait(const RequestOptions& request_options) const {
    return detail::run_blocking<DetailedTask>(handle_.blocking_loop(),
                                              [&] { return handle_.wait(request_options); });
}

CallToolResult SyncTaskHandle::result(const RequestOptions& request_options) const {
    return detail::run_blocking<CallToolResult>(handle_.blocking_loop(),
                                                [&] { return handle_.result(request_options); });
}

SyncClient::SyncClient(Client& client) : client_(&client) {}

ConnectResult SyncClient::connect(std::string_view name, std::string_view version) {
    return detail::run_blocking<ConnectResult>(client_->impl_->blocking_loop,
                                               [&] { return client_->connect(name, version); });
}

ConnectResult SyncClient::connect(const Implementation& client_info,
                                  const ClientCapabilities& capabilities) {
    return detail::run_blocking<ConnectResult>(
        client_->impl_->blocking_loop, [&] { return client_->connect(client_info, capabilities); });
}

nlohmann::json SyncClient::send_request(std::string_view method,
                                        const std::optional<nlohmann::json>& params) {
    return detail::run_blocking<nlohmann::json>(client_->impl_->blocking_loop,
                                                [&] { return client_->send_request(method, params); });
}

nlohmann::json SyncClient::send_request(std::string_view method,
                                        const std::optional<nlohmann::json>& params,
                                        const RequestOptions& request_options) {
    return detail::run_blocking<nlohmann::json>(client_->impl_->blocking_loop, [&] {
        return client_->send_request(method, params, request_options);
    });
}

Subscription SyncClient::listen(const SubscriptionFilter& filter) {
    return detail::run_blocking<Subscription>(client_->impl_->blocking_loop,
                                              [&] { return client_->listen(filter); });
}

Subscription SyncClient::listen(const SubscriptionFilter& filter,
                                const RequestOptions& request_options) {
    return detail::run_blocking<Subscription>(client_->impl_->blocking_loop,
                                              [&] { return client_->listen(filter, request_options); });
}

void SyncClient::send_notification(std::string_view method,
                                   const std::optional<nlohmann::json>& params) {
    detail::run_blocking<void>(client_->impl_->blocking_loop,
                               [&] { return client_->send_notification(method, params); });
}

void SyncClient::close() { client_->close(); }

CallToolResult SyncClient::call_tool(const std::string& name, const nlohmann::json& arguments) {
    return detail::run_blocking<CallToolResult>(client_->impl_->blocking_loop,
                                                [&] { return client_->call_tool(name, arguments); });
}

CallToolResult SyncClient::call_tool(const std::string& name, const nlohmann::json& arguments,
                                     const RequestOptions& request_options) {
    return detail::run_blocking<CallToolResult>(client_->impl_->blocking_loop, [&] {
        return client_->call_tool(name, arguments, request_options);
    });
}

std::variant<CallToolResult, TaskHandle> SyncClient::start_tool(const std::string& name,
                                                                const nlohmann::json& arguments) {
    return detail::run_blocking<std::variant<CallToolResult, TaskHandle>>(
        client_->impl_->blocking_loop, [&] { return client_->start_tool(name, arguments); });
}

std::variant<CallToolResult, TaskHandle> SyncClient::start_tool(const std::string& name,
                                                                const nlohmann::json& arguments,
                                                                const RequestOptions& request_options) {
    return detail::run_blocking<std::variant<CallToolResult, TaskHandle>>(
        client_->impl_->blocking_loop,
        [&] { return client_->start_tool(name, arguments, request_options); });
}

ListToolsResult SyncClient::list_tools(const std::optional<std::string>& cursor) {
    return detail::run_blocking<ListToolsResult>(client_->impl_->blocking_loop,
                                                 [&] { return client_->list_tools(cursor); });
}

ListToolsResult SyncClient::list_tools(const std::optional<std::string>& cursor,
                                       const RequestOptions& request_options) {
    return detail::run_blocking<ListToolsResult>(
        client_->impl_->blocking_loop, [&] { return client_->list_tools(cursor, request_options); });
}

ListResourcesResult SyncClient::list_resources(const std::optional<std::string>& cursor) {
    return detail::run_blocking<ListResourcesResult>(client_->impl_->blocking_loop,
                                                     [&] { return client_->list_resources(cursor); });
}

ListResourcesResult SyncClient::list_resources(const std::optional<std::string>& cursor,
                                               const RequestOptions& request_options) {
    return detail::run_blocking<ListResourcesResult>(client_->impl_->blocking_loop, [&] {
        return client_->list_resources(cursor, request_options);
    });
}

ReadResourceResult SyncClient::read_resource(const std::string& uri) {
    return detail::run_blocking<ReadResourceResult>(client_->impl_->blocking_loop,
                                                    [&] { return client_->read_resource(uri); });
}

ReadResourceResult SyncClient::read_resource(const std::string& uri,
                                             const RequestOptions& request_options) {
    return detail::run_blocking<ReadResourceResult>(
        client_->impl_->blocking_loop, [&] { return client_->read_resource(uri, request_options); });
}

ListResourceTemplatesResult SyncClient::list_resource_templates(
    const std::optional<std::string>& cursor) {
    return detail::run_blocking<ListResourceTemplatesResult>(
        client_->impl_->blocking_loop, [&] { return client_->list_resource_templates(cursor); });
}

ListResourceTemplatesResult SyncClient::list_resource_templates(
    const std::optional<std::string>& cursor, const RequestOptions& request_options) {
    return detail::run_blocking<ListResourceTemplatesResult>(client_->impl_->blocking_loop, [&] {
        return client_->list_resource_templates(cursor, request_options);
    });
}

ListPromptsResult SyncClient::list_prompts(const std::optional<std::string>& cursor) {
    return detail::run_blocking<ListPromptsResult>(client_->impl_->blocking_loop,
                                                   [&] { return client_->list_prompts(cursor); });
}

ListPromptsResult SyncClient::list_prompts(const std::optional<std::string>& cursor,
                                           const RequestOptions& request_options) {
    return detail::run_blocking<ListPromptsResult>(
        client_->impl_->blocking_loop, [&] { return client_->list_prompts(cursor, request_options); });
}

GetPromptResult SyncClient::get_prompt(const std::string& name,
                                       const std::map<std::string, std::string>& arguments) {
    return detail::run_blocking<GetPromptResult>(client_->impl_->blocking_loop,
                                                 [&] { return client_->get_prompt(name, arguments); });
}

GetPromptResult SyncClient::get_prompt(const std::string& name,
                                       const std::map<std::string, std::string>& arguments,
                                       const RequestOptions& request_options) {
    return detail::run_blocking<GetPromptResult>(client_->impl_->blocking_loop, [&] {
        return client_->get_prompt(name, arguments, request_options);
    });
}

CompleteResult SyncClient::complete(const CompleteParams& params) {
    return detail::run_blocking<CompleteResult>(client_->impl_->blocking_loop,
                                                [&] { return client_->complete(params); });
}

CompleteResult SyncClient::complete(const CompleteParams& params,
                                    const RequestOptions& request_options) {
    return detail::run_blocking<CompleteResult>(
        client_->impl_->blocking_loop, [&] { return client_->complete(params, request_options); });
}

void SyncClient::ping() {
    detail::run_blocking<void>(client_->impl_->blocking_loop, [&] { return client_->ping(); });
}

void SyncClient::cancel(const RequestId& request_id, const std::optional<std::string>& reason) {
    detail::run_blocking<void>(client_->impl_->blocking_loop,
                               [&] { return client_->cancel(request_id, reason); });
}

}  // namespace mcp

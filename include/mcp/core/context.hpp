#pragma once

// GCC 11 SSO Coroutine Safety — see docs/contributing.rst "Known Issues" for full details.
// Do NOT store std::string in a coroutine frame across co_await (GCC bugs #107288/#100611).
// Pattern used here:
//   [scope-before-await] Build SSO-risky objects in {}, serialise to wire string, then co_await.

#include <mcp/core/async_fwd.hpp>
#include <mcp/core/constants.hpp>
#include <mcp/core/export.hpp>
#include <mcp/detail/json_dump.hpp>
#include <mcp/protocol/protocol.hpp>
#include <mcp/protocol/request_meta.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <utility>

namespace mcp {

/// @brief Type alias for a function that sends a JSON-RPC request and returns the result.
///
/// The third argument is the key a handler chose for a question of a multi round-trip request, or
/// empty when the SDK names it; senders of other requests ignore it.
using RequestSender = std::function<Task<nlohmann::json>(std::string, std::optional<nlohmann::json>,
                                                         std::optional<std::string>)>;

/// @brief How a handler's question to the client is named in a multi round-trip request.
struct InputOptions {
    /// The key of the question in the result's `inputRequests` and in the client's
    /// `inputResponses`. When unset, the SDK names it `elicitation-N`, `sampling-N` or `roots-N`,
    /// counting the questions it names. A key must not be empty, must not look like one of those
    /// names and must be unused by any other question of the request, or the call throws
    /// std::invalid_argument. Requests before 2026-07-28 send each question as a request of its
    /// own and ignore the key. Pass the options as a named variable, not a braced temporary, to a
    /// co_await expression: GCC 11 mishandles the temporary.
    std::optional<std::string> key;
    /// Used by Context::elicit() only: whether an accepted form answer is checked against the
    /// request's `requestedSchema`. When false the answer is returned as the client sent it.
    bool validate_content = true;
};

/// @brief Type alias for a function that serializes outbound notification writes.
using MessageSender = std::function<Task<void>(std::string_view)>;

/// @brief Receives the progress a handler reports, without suspending the handler (see
/// Context::report_progress_sync()).
using ProgressSink = std::function<void(double progress, std::optional<double> total,
                                        const std::optional<std::string>& message)>;

class ITransport;

namespace detail {
struct ContextRouting;
class NotificationBudget;
}  // namespace detail

/**
 * @brief Request-scoped context provided to server handlers.
 *
 * @details Each incoming request receives a Context that provides
 * utilities for interacting with the client, such as sending log
 * messages, reporting progress, checking cancellation, and performing
 * reverse RPC (e.g. sampling). The Context holds a non-owning reference
 * to the transport and a request sender callable, both of which must
 * outlive the Context.
 */
class Context {
   public:
    /**
     * @brief Construct a Context with transport only (no reverse RPC support).
     *
     * @param transport Non-owning reference to the transport for
     *                  sending notifications back to the client.
     */
    explicit Context(ITransport& transport) : transport_(transport) {}

    /**
     * @brief Construct a Context with transport and request sender.
     *
     * @param transport Non-owning reference to the transport.
     * @param sender A callable that sends a JSON-RPC request and returns the result.
     */
    Context(ITransport& transport, RequestSender sender)
        : transport_(transport), sender_(std::move(sender)) {}

    /**
     * @brief Construct a full-featured Context with cancellation, progress, and log level.
     *
     * @param transport Non-owning reference to the transport.
     * @param sender A callable that sends a JSON-RPC request and returns the result.
     * @param cancelled Shared cancellation flag set by the server on notifications/cancelled.
     * @param progress_token Optional progress token from the request's _meta.
     * @param log_level Pointer to the server's current log level (shared across all contexts).
     * @param message_sender Optional serialized notification sender. When omitted, Context writes
     *                       directly to the transport.
     * @param request_meta The per-request protocol fields of a 2026-07-28 request, or null for a
     *                     request of an earlier revision.
     * @param principal Who sent the request, or null when unknown.
     * @param replaying True while the handler re-executes work an earlier round of the request
     *                  already did, or null when it never does.
     */
    Context(ITransport& transport, RequestSender sender, std::shared_ptr<std::atomic<bool>> cancelled,
            std::optional<ProgressToken> progress_token, const std::atomic<LoggingLevel>* log_level,
            MessageSender message_sender = {},
            std::shared_ptr<const RequestMetadata> request_meta = nullptr,
            std::shared_ptr<const std::string> principal = nullptr,
            std::shared_ptr<const std::atomic<bool>> replaying = nullptr)
        : transport_(transport),
          sender_(std::move(sender)),
          cancelled_(std::move(cancelled)),
          progress_token_(std::move(progress_token)),
          log_level_(log_level),
          message_sender_(std::move(message_sender)),
          request_meta_(std::move(request_meta)),
          principal_(std::move(principal)),
          replaying_(std::move(replaying)) {}

    /**
     * @brief The per-request protocol fields of this request.
     *
     * @return The protocol version, client capabilities, client info, log level and whole `_meta`
     *         of a 2026-07-28 request, or null for a request of an earlier revision, which
     *         negotiated these at initialize.
     */
    [[nodiscard]] const RequestMetadata* request_meta() const { return request_meta_.get(); }

    /**
     * @brief Who sent this request.
     *
     * @details Over HTTP with a bearer token validator, this is the principal the validator named
     * in its TokenValidation; without one, `h:` followed by the hex SHA-256 of the request's
     * Authorization header, which never reveals the token and changes when the token does. It is
     * empty for a transport that does not authenticate its peer (stdio, WebSocket, memory), for a
     * request without an Authorization header, and for the requests of a
     * StreamableHttpSessionManager session, which are attributed to the session.
     *
     * @return The principal, or an empty view when it is unknown.
     */
    [[nodiscard]] std::string_view principal() const {
        return principal_ ? std::string_view(*principal_) : std::string_view{};
    }

    /**
     * @brief Whether the handler is re-executing work an earlier round of the request did.
     *
     * @details With sealed request state (MrtrStateMode::eSealed) every retry of a multi
     * round-trip request runs the handler again from the start, and the questions earlier rounds
     * answered return their answers at once. This is true from the start of such a run until the
     * handler has taken the last of those answers or asks a question they do not answer; what the
     * handler does after that is new. A handler that does not ask again sees true until it ends.
     * Guard side effects, such as writes or charges, with `if (!ctx.is_replay())`. It is always false
     * for a first run, for stored request state, and for every other kind of request.
     *
     * @return True while the handler is replaying.
     */
    [[nodiscard]] bool is_replay() const {
        return replaying_ && replaying_->load(std::memory_order_relaxed);
    }

    /**
     * @brief Sends an informational log message to the client.
     *
     * @deprecated doc-only: protocol revision 2026-07-28 deprecates logging; see Deprecated Features
     * on the Specification Support page for why the SDK keeps it.
     *
     * @param msg The log message to send.
     * @return A task that completes when the message is sent.
     */
    MCP_API Task<void> log_info(std::string_view msg);

    /**
     * @brief Send a log message at a specific level.
     *
     * @deprecated doc-only: protocol revision 2026-07-28 deprecates logging; see Deprecated Features
     * on the Specification Support page for why the SDK keeps it.
     *
     * @details Respects the server's current log level. Messages with a level
     * higher (less severe) than the current threshold are silently dropped.
     * LoggingLevel ordering: Emergency(0) < Alert(1) < ... < Debug(7).
     * A message is sent only if its level <= current server log level.
     *
     * A 2026-07-28 request has no session level: its threshold is the
     * `io.modelcontextprotocol/logLevel` in its `_meta`, and without one nothing is sent. A
     * message over the limit of Server::set_notification_rate_limit() is dropped.
     *
     * @param level The severity level of the log message.
     * @param msg The log message to send.
     * @param logger Optional logger name.
     * @return A task that completes when the message is sent (or immediately if filtered).
     */
    MCP_API Task<void> log(LoggingLevel level, std::string_view msg,
                           const std::optional<std::string>& logger = std::nullopt);

    /**
     * @brief Check whether the current request has been cancelled.
     *
     * @return true once the request has been cancelled: by notifications/cancelled for this
     * request's ID, by the client disconnecting while a 2026-07-28 request runs on
     * StreamableHttpSessionManager, or through the cancellation flag given to
     * Server::dispatch_request_direct(). The server reads a cancellation on its event loop, which
     * a synchronous handler running there holds up: such a handler sees it only on the executor
     * of Server::set_handler_executor().
     */
    [[nodiscard]] bool is_cancelled() const {
        if (cancelled_) {
            return cancelled_->load(std::memory_order_relaxed);
        }
        return false;
    }

    /**
     * @brief Report progress for the current request.
     *
     * @details Sends a notifications/progress notification to the client. Uses the
     * progress token extracted from the request's _meta.progressToken. If no progress
     * token was provided in the request, this is a no-op. A notification over the limit of
     * Server::set_notification_rate_limit() is dropped.
     *
     * @param progress The current progress value.
     * @param total Optional total progress value.
     * @param message Optional human-readable progress message.
     * @return A task that completes when the notification is sent.
     *
     * In a task (see Server::enable_tasks()) the progress becomes the task's status message
     * instead, and no notification is sent.
     */
    MCP_API Task<void> report_progress(double progress, std::optional<double> total = std::nullopt,
                                       const std::optional<std::string>& message = std::nullopt);

    /**
     * @brief Report progress without suspending, for a handler that is not a coroutine.
     *
     * @details In a task (see Server::enable_tasks()) the progress becomes the task's status
     * message, as report_progress() does. Elsewhere the report would need a write, which a call
     * that does not suspend cannot make, so nothing is sent: use report_progress().
     *
     * @param progress The current progress value.
     * @param total Optional total progress value.
     * @param message Optional human-readable progress message.
     * @return Whether the progress was reported.
     */
    bool report_progress_sync(double progress, std::optional<double> total = std::nullopt,
                              const std::optional<std::string>& message = std::nullopt) {
        if (!progress_sink_) {
            return false;
        }
        progress_sink_(progress, total, message);
        return true;
    }

    /**
     * @brief Sets where this context's progress goes instead of the client, which the server does
     * for the handler of a task.
     */
    void set_progress_sink(ProgressSink sink) { progress_sink_ = std::move(sink); }

    /**
     * @brief Request the client to sample an LLM (reverse RPC).
     *
     * @deprecated doc-only: protocol revision 2026-07-28 deprecates sampling; see Deprecated Features
     * on the Specification Support page for why the SDK keeps it.
     *
     * @details Sends a "sampling/createMessage" request to the client
     * and awaits the response. Requires the Context to be constructed
     * with a RequestSender.
     *
     * @param request The sampling request parameters.
     * @param options How the question is named in a multi round-trip request; see InputOptions.
     * @return A task that resolves to the client's CreateMessageResult.
     * @throws std::runtime_error If no request sender is available.
     * @throws std::invalid_argument If `options.key` is empty, reserved or already used, in a
     *         2026-07-28 request (other requests ignore the key), or if `includeContext` is other
     *         than "none" in a session whose client did not declare `sampling.context` in its
     *         initialize request.
     *
     * For a 2026-07-28 tools/call, prompts/get or resources/read, the question goes back to the
     * client in the request's `input_required` result, and the call returns once the client
     * retries the request with the answer; see Server::set_mrtr_options(). If the client did not
     * declare the capability the question needs (`sampling.context` for an `includeContext` other
     * than "none"), the request is answered with
     * `mcp::constants::jsonrpc::g_MISSING_REQUIRED_CLIENT_CAPABILITY` and the call throws, as it does
     * when the client never retries in time; the handler's own answer then goes nowhere. Any other
     * 2026-07-28 request cannot ask the client anything, and the call throws std::runtime_error.
     */
    MCP_API Task<CreateMessageResult> sample_llm(const CreateMessageRequestParams& request,
                                                 InputOptions options = {});

    /**
     * @brief Elicit additional information from the user via the client (reverse RPC).
     *
     * @details Sends an "elicitation/create" request to the client and awaits the response.
     * Supports both form mode (ElicitRequestFormParams) and URL mode (ElicitRequestURLParams)
     * via the ElicitRequestParams variant.
     *
     * Before 2026-07-28 a URL-mode request needs an `elicitationId`, and one is generated when
     * the request leaves it unset; a 2026-07-28 question carries none.
     *
     * A form-mode answer with action `accept` is checked against the request's `requestedSchema`
     * before it is returned, as the specification asks servers to do; a URL-mode answer carries no
     * content and is not checked.
     *
     * @param params The elicitation request parameters (form or URL mode).
     * @param options How the question is named in a multi round-trip request, and whether an
     *        accepted form answer is checked; see InputOptions.
     * @return A task that resolves to the client's ElicitResult.
     * @throws std::invalid_argument If `options.key` is empty, reserved or already used, in a
     *         2026-07-28 request (other requests ignore the key).
     * @throws std::runtime_error If no request sender is available, or the accepted content does
     *         not conform to the schema (the message lists the violations). A schema the SDK
     *         cannot compile leaves the content unchecked.
     *
     * For a 2026-07-28 tools/call, prompts/get or resources/read, the question goes back to the
     * client in the request's `input_required` result, and the call returns once the client
     * retries the request with the answer; see Server::set_mrtr_options(). If the client did not
     * declare the capability the question needs, the request is answered with
     * `mcp::constants::jsonrpc::g_MISSING_REQUIRED_CLIENT_CAPABILITY` and the call throws, as it does
     * when the client never retries in time; the handler's own answer then goes nowhere. Any other
     * 2026-07-28 request cannot ask the client anything, and the call throws std::runtime_error.
     */
    MCP_API Task<ElicitResult> elicit(const ElicitRequestParams& params, InputOptions options = {});

    /**
     * @brief Request the list of roots from the client (reverse RPC).
     *
     * @deprecated doc-only: protocol revision 2026-07-28 deprecates roots; see Deprecated Features
     * on the Specification Support page for why the SDK keeps it.
     *
     * @details Sends a "roots/list" request to the client and awaits
     * the response. The client provides its configured root URIs.
     *
     * @param options How the question is named in a multi round-trip request; see InputOptions.
     * @return A task that resolves to the client's ListRootsResult.
     * @throws std::runtime_error If no request sender is available.
     * @throws std::invalid_argument If `options.key` is empty, reserved or already used, in a
     *         2026-07-28 request (other requests ignore the key).
     *
     * For a 2026-07-28 tools/call, prompts/get or resources/read, the question goes back to the
     * client in the request's `input_required` result, and the call returns once the client
     * retries the request with the answer; see Server::set_mrtr_options(). If the client did not
     * declare the capability the question needs, the request is answered with
     * `mcp::constants::jsonrpc::g_MISSING_REQUIRED_CLIENT_CAPABILITY` and the call throws, as it does
     * when the client never retries in time; the handler's own answer then goes nowhere. Any other
     * 2026-07-28 request cannot ask the client anything, and the call throws std::runtime_error.
     */
    MCP_API Task<ListRootsResult> request_roots(InputOptions options = {});

   private:
    friend struct detail::ContextRouting;

    /// Throws when @p result accepts the form request @p params with content its schema rejects.
    static void check_form_content(const ElicitRequestParams& params, const ElicitResult& result) {
        const auto* form = std::get_if<ElicitRequestFormParams>(&params);
        if (form == nullptr || result.action != ElicitAction::eAccept) {
            return;
        }
        ValidationResult validation;
        try {
            validation = validate_elicitation_content(form->requestedSchema, result.content);
        } catch (const SchemaError&) {
            return;
        }
        if (!validation.valid) {
            throw std::runtime_error("The elicitation content does not match the requested schema: " +
                                     describe_violations(validation));
        }
    }

    /// A fresh, process-unique identifier for a legacy URL-mode elicitation.
    static std::string generate_elicitation_id() {
        static std::atomic<std::uint64_t> s_counter{0};
        static const std::uint64_t s_seed = std::random_device{}();
        return "elicit-" + std::to_string(s_seed) + "-" + std::to_string(s_counter.fetch_add(1));
    }

    ITransport& transport_;
    RequestSender sender_;
    std::shared_ptr<std::atomic<bool>> cancelled_;
    std::optional<ProgressToken> progress_token_;
    const std::atomic<LoggingLevel>* log_level_ = nullptr;
    MessageSender message_sender_;
    std::shared_ptr<const RequestMetadata> request_meta_;
    std::shared_ptr<const std::string> principal_;
    std::shared_ptr<const std::atomic<bool>> replaying_;
    ProgressSink progress_sink_;
    /// The progress and log notifications this request may still send; null for no limit.
    std::shared_ptr<detail::NotificationBudget> budget_;
};

}  // namespace mcp

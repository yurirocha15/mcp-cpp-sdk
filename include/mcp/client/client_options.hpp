#pragma once

#include <mcp/client/client_protocol.hpp>
#include <mcp/core/constants.hpp>
#include <mcp/core/notification_rate_limit.hpp>
#include <mcp/protocol/base.hpp>
#include <mcp/protocol/schema.hpp>
#include <mcp/protocol/tools.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace mcp {

/** @brief Whether the client takes part in the tasks extension (see ClientOptions::tasks). */
enum class ClientTaskMode : std::uint8_t {
    /// The client does not declare the extension, so a server never answers it with a task.
    eOff,
    /// The client declares the extension with every request of a 2026-07-28 session, and
    /// Client::call_tool() follows a task the server starts until it finishes.
    eAuto,
};

/**
 * @brief How a client keeps the results a server says it may reuse.
 *
 * @details A 2026-07-28 server marks the results of `tools/list`, `prompts/list`,
 * `resources/list`, `resources/templates/list` and `resources/read` with `ttlMs`, how long the
 * client may treat them as fresh. With the cache on, list_tools(), list_prompts(), list_resources(),
 * list_resource_templates() and read_resource() keep each result whose `ttlMs` is positive, under
 * its method and parameters (the cursor of a page, the URI of a resource), and answer the same call
 * from it while it is fresh, without asking the server. A result without `ttlMs`, as from an older
 * server, is not kept. A result the client got by answering the server's `input_required`
 * questions is not kept either. A `list_changed` notification drops the lists it names, and
 * `notifications/resources/updated` the reads of its URI. The cache belongs to one Client and
 * serves no other, so a `"private"` result never reaches another authorization context; a client
 * whose credentials change to another user's should call Client::clear_response_cache().
 */
struct ResponseCacheOptions {
    /// Whether the client keeps results. Off unless set.
    bool enabled{false};
    /// Most results kept at once; the one closest to going stale makes room. Must be positive.
    std::size_t max_entries{mcp::constants::client::g_DEFAULT_RESPONSE_CACHE_ENTRIES};
    /// Longest a result is kept, whatever its `ttlMs` says. Must be positive.
    std::chrono::milliseconds max_ttl{mcp::constants::client::g_DEFAULT_RESPONSE_CACHE_MAX_TTL};
};

/** @brief How one cacheable call uses the response cache; see ResponseCacheOptions. */
enum class CacheMode : std::uint8_t {
    eUse,      ///< Answer from a fresh result if there is one, else ask the server and keep its answer.
    eRefresh,  ///< Ask the server and keep its answer.
    eBypass,   ///< Ask the server and leave the cache as it is.
};

/** @brief Client-wide runtime behavior. */
struct ClientOptions {
    /**
     * @brief Which protocol to speak; automatic() unless set.
     *
     * @details See ClientProtocol. Construction throws std::invalid_argument for a value that
     * names a version the SDK does not know.
     */
    ClientProtocol protocol{ClientProtocol::automatic()};
    /**
     * @brief How long ClientProtocol::automatic() waits for the answer to server/discover.
     *
     * @details The timer starts when the client starts probing, once the transport is ready.
     * A legacy server that ignores the unknown request is the case it exists for. Must be
     * positive. A modern server that needs longer to start than this is not misclassified for
     * good: when it then rejects initialize naming modern versions, the client probes once more.
     */
    std::chrono::milliseconds probe_timeout{mcp::constants::client::g_DEFAULT_PROBE_TIMEOUT};
    /**
     * @brief Invoked each time connect() decides an era.
     *
     * @details Runs on the connecting coroutine, so it must not block. It can run twice in one
     * connect(): a probe that timed out decides legacy, and a later re-probe may decide modern.
     * An exception thrown from it is discarded.
     */
    std::function<void(const EraDecision&)> on_era_decision;
    /**
     * @brief Default time limit for outgoing requests. Thirty seconds unless set.
     *
     * Every request the client issues carries this deadline, including the typed helpers such
     * as call_tool() and read_resource(), which accept no per-request override. A request that
     * outlives it fails with McpError mcp::constants::jsonrpc::g_REQUEST_TIMEOUT while the peer may
     * still be running it, so raise this for a client whose tools legitimately take longer. Must be
     * positive: the constructor throws std::invalid_argument otherwise, and there is no value that
     * disables the deadline.
     */
    std::chrono::milliseconds request_timeout{mcp::constants::client::g_DEFAULT_REQUEST_TIMEOUT};
    /**
     * @brief Memory the server's requests being answered, and the answers not yet written, may
     * take before the client stops reading. 256 MiB unless set; must be positive.
     *
     * A request is counted as its parsed JSON takes memory, which can be far more than its text.
     * The client also stops reading while it answers 1024 requests of the server.
     */
    std::size_t max_incoming_request_bytes =
        mcp::constants::client::g_DEFAULT_MAX_INCOMING_REQUEST_BYTES;
    /** @brief Reject malformed JSON-RPC envelopes received from the peer. */
    bool strict_protocol_validation{true};
    /**
     * @brief Invoked when an incoming message is discarded instead of dispatched.
     *
     * Reports two things, neither of which ends the session and neither of which the
     * application can otherwise observe:
     * - a message the peer sent that the client could not decode, as
     * mcp::constants::jsonrpc::g_PARSE_ERROR;
     * - an exception thrown by an application notification callback, as
     * mcp::constants::jsonrpc::g_INTERNAL_ERROR, with the notification method in the message.
     *
     * Runs on the client's read loop, so it must not block. An exception thrown from it is
     * discarded: there is nowhere left to report a failure of the reporting path itself.
     */
    std::function<void(const Error&)> on_protocol_error;
    /**
     * @brief Whether the client declares the tasks extension and follows its tasks; off unless set.
     *
     * @details With ClientTaskMode::eAuto, a 2026-07-28 session declares
     * `io.modelcontextprotocol/tasks` in the capabilities of each request, and call_tool() that
     * is answered with a task polls `tasks/get` until the task finishes, honoring the server's
     * `pollIntervalMs`, answers the task's `inputRequests` with the registered elicitation,
     * sampling and roots handlers, and returns the tool's result as if the call had run in the
     * request. A task that failed throws the McpError it failed with, and one that was cancelled
     * throws an McpError. start_tool() and task_handle() take part in the extension whatever this
     * is set to. A legacy session has no tasks and ignores it.
     */
    ClientTaskMode tasks{ClientTaskMode::eOff};
    /**
     * @brief How often the client polls a task whose state names no interval. One second unless
     * set; must be positive.
     */
    std::chrono::milliseconds task_poll_interval{mcp::constants::client::g_DEFAULT_TASK_POLL_INTERVAL};
    /**
     * @brief How long call_tool() and TaskHandle::wait() follow one task before they give up with
     * mcp::constants::jsonrpc::g_REQUEST_TIMEOUT. Thirty minutes unless set; must be positive.
     *
     * @details A TaskHandle::wait() that gives up leaves the task running on the server, and the
     * handle can pick it up again. A call_tool() that gives up, for this timeout or because a
     * handler for the task's input requests failed, sends `tasks/cancel` for the task before it
     * throws, since nothing else holds the task; the cancel is best effort. Each poll is limited
     * by request_timeout like any other request, and call_tool() takes no per-request options.
     */
    std::chrono::milliseconds task_timeout{mcp::constants::client::g_DEFAULT_TASK_TIMEOUT};
    /**
     * @brief Check the schemas of the tools the server lists.
     *
     * list_tools() lists every tool the server names and compiles each `outputSchema`, reporting
     * one it cannot use through on_tool_schema_error. call_tool() then checks the
     * `structuredContent` of a successful result against the output schema from the most recent
     * list_tools() result that included the tool, and throws StructuredContentError when it does
     * not conform. A call of a tool whose output schema is invalid, in an unsupported dialect or
     * has a `$ref` outside the document throws that SchemaError. A schema that only uses a pattern
     * the matcher cannot run, such as a lookahead, leaves that tool's results unchecked. A tool the
     * client has not seen listed is not checked. The server enforces `inputSchema`, so the client
     * does not compile it.
     */
    bool validate_tool_schemas{true};
    /**
     * @brief Invoked for each tool whose output schema list_tools() found it cannot use.
     *
     * Runs inside list_tools(), so it must not block. An exception thrown from it is discarded.
     */
    std::function<void(const Tool&, const SchemaError&)> on_tool_schema_error;
    /**
     * @brief Check the content an on_elicitation() handler accepts against the request's schema.
     *
     * @details When the handler returns action `accept` for a form-mode request, its `content` is
     * validated against the request's `requestedSchema` before anything is sent, in both
     * protocol eras. Content that does not conform is not sent: the handler's call fails with
     * ElicitationContentError. A legacy `elicitation/create` request is then answered with a
     * JSON-RPC error, and a 2026-07-28 request fails the call that asked the question with that
     * exception. A schema the SDK cannot compile leaves the content unchecked. Applies to handlers
     * registered with on_elicitation(), not to a raw on_request("elicitation/create") handler.
     * Turn it off for an application that validates the content itself.
     */
    bool validate_elicitation_content{true};
    /**
     * @brief How many notifications a Subscription holds for the application to read.
     *
     * @details A subscription queues the notifications of its stream until Subscription::next()
     * takes them. When the queue is full the oldest one is dropped and counted by
     * Subscription::dropped_events(). Must be positive: the constructor throws
     * std::invalid_argument otherwise.
     */
    std::size_t subscription_queue_limit{mcp::constants::client::g_DEFAULT_SUBSCRIPTION_QUEUE_LIMIT};
    /// @brief Whether and how the client keeps results the server says it may reuse.
    ResponseCacheOptions response_cache;
    /**
     * @brief Limit on the progress and log notifications the client hands to its callbacks.
     *
     * @details The progress of each request with RequestOptions::on_progress has its own bucket;
     * progress for Client::on_progress shares one, and `notifications/message` another. A
     * notification over the limit is dropped and counted in Client::dropped_notifications(). By
     * default there is no limit. The constructor throws std::invalid_argument when `per_second` is
     * negative or not a number.
     */
    NotificationRateLimit notification_rate_limit;
};

}  // namespace mcp

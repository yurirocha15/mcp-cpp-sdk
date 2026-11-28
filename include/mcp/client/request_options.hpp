#pragma once

#include <mcp/client/cancellation_token.hpp>
#include <mcp/client/client_options.hpp>
#include <mcp/core/constants.hpp>
#include <mcp/protocol/notification.hpp>

#include <chrono>
#include <functional>
#include <optional>

namespace mcp {

/** @brief Overrides for one outgoing request. */
struct RequestOptions {
    /**
     * @brief Per-request deadline; uses ClientOptions::request_timeout when absent.
     *
     * A write still queued at the deadline is canceled before transmission. If
     * transport I/O has already started, remote execution may be ambiguous even
     * though the caller receives a timeout, as with any network request deadline.
     */
    std::optional<std::chrono::milliseconds> timeout;
    /**
     * @brief Minimum log level the server should emit for this request.
     *
     * @deprecated Protocol revision 2026-07-28 deprecates logging (SEP-2577); the specification may
     * remove it in the first revision released on or after 2027-07-28. See the Deprecations guide.
     *
     * @details Sent as `io.modelcontextprotocol/logLevel` in the request's `_meta` when the
     * session is modern. A legacy session sets its level with logging/setLevel instead, and this
     * member has no effect there. It carries no deprecation attribute, which would warn wherever a
     * RequestOptions is built; the client reports its use at run time instead, see
     * ClientOptions::deprecation_handler.
     */
    std::optional<LoggingLevel> log_level;
    /**
     * @brief Most retries one modern tools/call, prompts/get or resources/read may make.
     *
     * @details When the server answers such a request with an `input_required` result, the client
     * runs its registered elicitation, sampling and roots handlers for the questions and sends the
     * request again with the answers. Each retry counts against this limit; a server that still asks
     * after the last one fails the call with an McpError. Legacy sessions ignore it.
     */
    int max_input_rounds = mcp::constants::client::g_DEFAULT_MAX_INPUT_ROUNDS;
    /**
     * @brief Most questions one `input_required` result may ask.
     *
     * @details Each question runs a registered handler, which may mean asking a person. A server
     * that asks more fails the call with an McpError.
     */
    int max_input_requests = mcp::constants::client::g_DEFAULT_MAX_INPUT_REQUESTS;
    /// @brief Cancels the request when cancelled; see CancellationToken.
    std::optional<CancellationToken> cancellation;
    /**
     * @brief Receives the progress the server reports for this request.
     *
     * @details When set, the request carries a `progressToken` in its `_meta`: the one the params
     * name, or else a new string token, which differs from the integer tokens an application
     * writes into other requests. Each `notifications/progress` with that token goes to this
     * callback instead of Client::on_progress until the answer arrives. A tool call the client
     * follows as a task (ClientTaskMode::eAuto) keeps the token until the task's result arrives;
     * after start_tool() returns a TaskHandle, the task's progress goes to Client::on_progress.
     * The callback runs on the client's read loop, as notification callbacks do, so it must not
     * block on the client. Client::listen() does not use it.
     */
    std::function<void(const ProgressNotificationParams&)> on_progress;
    /**
     * @brief How a cacheable call uses the response cache, when ClientOptions::response_cache is
     * on. Other calls ignore it.
     */
    CacheMode cache_mode{CacheMode::eUse};
};

}  // namespace mcp

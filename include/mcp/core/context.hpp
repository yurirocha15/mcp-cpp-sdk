#pragma once

// GCC 11 SSO Coroutine Safety — see docs/contributing.rst "Known Issues" for full details.
// Do NOT store std::string in a coroutine frame across co_await (GCC bugs #107288/#100611).
// Pattern used here:
//   [scope-before-await] Build SSO-risky objects in {}, serialise to wire string, then co_await.

#include <mcp/core/core.hpp>
#include <mcp/protocol/protocol.hpp>
#include <mcp/protocol/request_meta.hpp>
#include <mcp/transport/transport.hpp>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace mcp {

/// @brief Type alias for a function that sends a JSON-RPC request and returns the result.
using RequestSender = std::function<Task<nlohmann::json>(std::string, std::optional<nlohmann::json>)>;

/// @brief Type alias for a function that serializes outbound notification writes.
using MessageSender = std::function<Task<void>(std::string_view)>;

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
     */
    Context(ITransport& transport, RequestSender sender, std::shared_ptr<std::atomic<bool>> cancelled,
            std::optional<ProgressToken> progress_token, const std::atomic<LoggingLevel>* log_level,
            MessageSender message_sender = {},
            std::shared_ptr<const RequestMetadata> request_meta = nullptr,
            std::shared_ptr<const std::string> principal = nullptr)
        : transport_(transport),
          sender_(std::move(sender)),
          cancelled_(std::move(cancelled)),
          progress_token_(std::move(progress_token)),
          log_level_(log_level),
          message_sender_(std::move(message_sender)),
          request_meta_(std::move(request_meta)),
          principal_(std::move(principal)) {}

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
     * @brief Sends an informational log message to the client.
     *
     * @param msg The log message to send.
     * @return A task that completes when the message is sent.
     */
    Task<void> log_info(std::string_view msg) { co_await log(LoggingLevel::eInfo, msg); }

    /**
     * @brief Send a log message at a specific level.
     *
     * @details Respects the server's current log level. Messages with a level
     * higher (less severe) than the current threshold are silently dropped.
     * LoggingLevel ordering: Emergency(0) < Alert(1) < ... < Debug(7).
     * A message is sent only if its level <= current server log level.
     *
     * A 2026-07-28 request has no session level: its threshold is the
     * `io.modelcontextprotocol/logLevel` in its `_meta`, and without one nothing is sent.
     *
     * @param level The severity level of the log message.
     * @param msg The log message to send.
     * @param logger Optional logger name.
     * @return A task that completes when the message is sent (or immediately if filtered).
     */
    Task<void> log(LoggingLevel level, std::string_view msg,
                   const std::optional<std::string>& logger = std::nullopt) {
        if (request_meta_) {
            if (!request_meta_->logLevel ||
                static_cast<uint8_t>(level) > static_cast<uint8_t>(*request_meta_->logLevel)) {
                co_return;
            }
        } else if (log_level_ != nullptr) {
            auto current = log_level_->load(std::memory_order_relaxed);
            if (static_cast<uint8_t>(level) > static_cast<uint8_t>(current)) {
                co_return;
            }
        }

        // [gcc11-sso: scope-before-await]
        std::shared_ptr<const std::string> wire;
        {
            LoggingMessageNotificationParams params;
            params.level = level;
            params.data = std::string(msg);
            params.logger = logger;

            JSONRPCNotification notification;
            notification.method = "notifications/message";
            notification.params = nlohmann::json(std::move(params));
            wire = std::make_shared<const std::string>(nlohmann::json(std::move(notification)).dump());
        }
        if (message_sender_) {
            co_await message_sender_(*wire);
        } else {
            co_await transport_.write_message(*wire);
        }
    }

    /**
     * @brief Check whether the current request has been cancelled.
     *
     * @return true once the request has been cancelled: by notifications/cancelled for this
     * request's ID, by the client disconnecting while a 2026-07-28 request runs on
     * StreamableHttpSessionManager, or through the cancellation flag given to
     * Server::dispatch_request_direct().
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
     * token was provided in the request, this is a no-op.
     *
     * @param progress The current progress value.
     * @param total Optional total progress value.
     * @param message Optional human-readable progress message.
     * @return A task that completes when the notification is sent.
     */
    Task<void> report_progress(double progress, std::optional<double> total = std::nullopt,
                               const std::optional<std::string>& message = std::nullopt) {
        if (!progress_token_) {
            co_return;
        }

        // [gcc11-sso: scope-before-await]
        std::shared_ptr<const std::string> wire;
        {
            ProgressNotificationParams params;
            params.progressToken = *progress_token_;
            params.progress = progress;
            params.total = total;
            params.message = message;

            JSONRPCNotification notification;
            notification.method = "notifications/progress";
            notification.params = nlohmann::json(std::move(params));
            wire = std::make_shared<const std::string>(nlohmann::json(std::move(notification)).dump());
        }
        if (message_sender_) {
            co_await message_sender_(*wire);
        } else {
            co_await transport_.write_message(*wire);
        }
    }

    /**
     * @brief Request the client to sample an LLM (reverse RPC).
     *
     * @details Sends a "sampling/createMessage" request to the client
     * and awaits the response. Requires the Context to be constructed
     * with a RequestSender.
     *
     * @param request The sampling request parameters.
     * @return A task that resolves to the client's CreateMessageResult.
     * @throws std::runtime_error If no request sender is available. Always throws for a
     * 2026-07-28 request (request_meta() is not null): that revision has no server-to-client
     * requests.
     */
    Task<CreateMessageResult> sample_llm(const CreateMessageRequestParams& request) {
        if (!sender_) {
            throw std::runtime_error("Context has no request sender for reverse RPC");
        }

        nlohmann::json params = request;
        auto result_json = co_await sender_("sampling/createMessage", std::move(params));
        co_return result_json.get<CreateMessageResult>();
    }

    /**
     * @brief Elicit additional information from the user via the client (reverse RPC).
     *
     * @details Sends an "elicitation/create" request to the client and awaits the response.
     * Supports both form mode (ElicitRequestFormParams) and URL mode (ElicitRequestURLParams)
     * via the ElicitRequestParams variant.
     *
     * @param params The elicitation request parameters (form or URL mode).
     * @return A task that resolves to the client's ElicitResult.
     * @throws std::runtime_error If no request sender is available. Always throws for a
     * 2026-07-28 request (request_meta() is not null): that revision has no server-to-client
     * requests.
     */
    Task<ElicitResult> elicit(const ElicitRequestParams& params) {
        if (!sender_) {
            throw std::runtime_error("Context has no request sender for reverse RPC");
        }

        nlohmann::json json_params = params;
        auto result_json = co_await sender_("elicitation/create", std::move(json_params));
        co_return result_json.get<ElicitResult>();
    }

    /**
     * @brief Request the list of roots from the client (reverse RPC).
     *
     * @details Sends a "roots/list" request to the client and awaits
     * the response. The client provides its configured root URIs.
     *
     * @return A task that resolves to the client's ListRootsResult.
     * @throws std::runtime_error If no request sender is available. Always throws for a
     * 2026-07-28 request (request_meta() is not null): that revision has no server-to-client
     * requests.
     */
    Task<ListRootsResult> request_roots() {
        if (!sender_) {
            throw std::runtime_error("Context has no request sender for reverse RPC");
        }

        auto result_json = co_await sender_("roots/list", std::nullopt);
        co_return result_json.get<ListRootsResult>();
    }

   private:
    ITransport& transport_;
    RequestSender sender_;
    std::shared_ptr<std::atomic<bool>> cancelled_;
    std::optional<ProgressToken> progress_token_;
    const std::atomic<LoggingLevel>* log_level_ = nullptr;
    MessageSender message_sender_;
    std::shared_ptr<const RequestMetadata> request_meta_;
    std::shared_ptr<const std::string> principal_;
};

}  // namespace mcp

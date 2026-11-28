#pragma once

#include <mcp/core/async_fwd.hpp>
#include <mcp/core/export.hpp>
#include <mcp/protocol/base.hpp>

#include <cstdint>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace mcp {

/**
 * @brief The notification types a subscriptions/listen stream asks for (protocol revision
 * 2026-07-28 and later).
 *
 * @details A member left at its default asks for nothing of that type. The server answers with the
 * subset it honors, see Subscription::acknowledged().
 */
struct SubscriptionFilter {
    bool toolsListChanged{false};      ///< notifications/tools/list_changed
    bool promptsListChanged{false};    ///< notifications/prompts/list_changed
    bool resourcesListChanged{false};  ///< notifications/resources/list_changed
    /// URIs for which notifications/resources/updated is wanted.
    std::vector<std::string> resourceSubscriptions;
    /// Tasks for which notifications/tasks is wanted. Asking needs the tasks extension, which the
    /// request then declares whatever ClientOptions::tasks is set to.
    std::vector<std::string> taskIds;

    /// Filters are equal when they ask for the same notifications and the same URIs, in order.
    bool operator==(const SubscriptionFilter&) const = default;
};

/** @brief Serializes only the members that ask for something, as the wire `notifications` object. */
MCP_API void to_json(nlohmann::json& json_obj, const SubscriptionFilter& filter);

/** @brief Reads a wire `notifications` object; a member that is absent asks for nothing. */
MCP_API void from_json(const nlohmann::json& json_obj, SubscriptionFilter& filter);

/** @brief How a subscription ended. */
enum class SubscriptionEnd : std::uint8_t {
    eServerClosed,  ///< The server ended it with the empty result: a graceful end.
    eCancelled,     ///< Subscription::close() ended it.
    eDropped,       ///< The connection ended without that result: an unexpected disconnect.
    eFailed,        ///< The server answered the request with an error after acknowledging it.
};

/** @brief One notification delivered on a subscription. */
struct SubscriptionEvent {
    std::string method;     ///< The notification method, such as "notifications/tools/list_changed".
    nlohmann::json params;  ///< Its params, including `_meta` with the subscription id.
};

namespace detail {
struct SubscriptionState;
}  // namespace detail

class SyncSubscription;

/**
 * @brief A subscriptions/listen stream opened with Client::listen().
 *
 * @details A copy refers to the same stream. Notifications that carry this stream's id arrive
 * through next(); a notification of another stream never does. The handle does not close the
 * stream when it is destroyed: call close(), or the stream stays open (and queues notifications up
 * to ClientOptions::subscription_queue_limit) until the connection ends.
 *
 * Reconnecting is the application's job: after a transport drop, connect a new client and call
 * Client::listen() again; the server keeps no subscription across connections.
 */
class MCP_API Subscription {
   public:
    /** @brief A handle that is not attached to a stream; it reads as cancelled and next() yields
     * nothing. */
    Subscription();

    /** @brief For Client::listen(); applications do not call this. */
    explicit Subscription(std::shared_ptr<detail::SubscriptionState> state);

    /** @brief The JSON-RPC id of the subscriptions/listen request, which is the subscription id. */
    [[nodiscard]] RequestId id() const;

    /** @brief The filter that was asked for. */
    [[nodiscard]] SubscriptionFilter requested() const;

    /** @brief The subset the server agreed to honor, from its acknowledgment. */
    [[nodiscard]] SubscriptionFilter acknowledged() const;

    /**
     * @brief The part of requested() that acknowledged() leaves out.
     *
     * @details Empty (equal to a default SubscriptionFilter) when the server honors everything.
     */
    [[nodiscard]] SubscriptionFilter unhonored() const;

    /**
     * @brief Wait for the next notification of this stream.
     *
     * @details Notifications are returned in arrival order, including those that arrived while
     * no next() call was waiting. When the stream has ended and the queue is empty, resolves to
     * nothing; end_reason() then says how it ended. Only one next() may be outstanding at a time.
     */
    Task<std::optional<SubscriptionEvent>> next();

    /**
     * @brief End the stream by sending `notifications/cancelled` for the listen request.
     *
     * @details The server answers a cancelled stream with nothing. Does nothing when the stream
     * has already ended. A next() that is waiting resolves to nothing.
     */
    Task<void> close();

    /// @brief Blocking calls to this stream, for a subscription of a Client constructed from an
    /// Endpoint (see Client::sync()).
    /// @details The subscription may outlive its Client. Once that Client has been destroyed,
    /// each blocking call throws McpError (mcp::constants::jsonrpc::g_CONNECTION_CLOSED), since
    /// the event loop it would run on has ended.
    /// @throws std::logic_error If the client runs on the application's executor.
    [[nodiscard]] SyncSubscription sync() const;

    /** @brief How the stream ended, or nothing while it is open. */
    [[nodiscard]] std::optional<SubscriptionEnd> end_reason() const;

    /** @brief The error behind SubscriptionEnd::eFailed or eDropped, when there is one. */
    [[nodiscard]] std::optional<Error> end_error() const;

    /** @brief How many notifications were discarded because the queue was full. */
    [[nodiscard]] std::uint64_t dropped_events() const;

   private:
    friend class SyncSubscription;

    std::shared_ptr<detail::SubscriptionState> state_;
};

}  // namespace mcp

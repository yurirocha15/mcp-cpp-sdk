#pragma once

#include <cstddef>

namespace mcp {

/**
 * @brief A cap on how fast notifications of one kind go out or are taken in, as a token bucket.
 *
 * @details The specification asks both sides to rate-limit progress and log notifications so a
 * peer cannot flood the other. Up to `burst` notifications pass at once; after that, `per_second`
 * more each second. A notification over the limit is dropped and counted. The default, a `burst`
 * of zero, sets no limit. Used by Server::set_notification_rate_limit() and
 * ClientOptions::notification_rate_limit.
 */
struct NotificationRateLimit {
    /// Notifications that may pass at once. Zero sets no limit.
    std::size_t burst{0};
    /// Notifications that may pass each second once the burst is spent. Zero lets none pass after
    /// the burst. Must not be negative.
    double per_second{0.0};
};

}  // namespace mcp

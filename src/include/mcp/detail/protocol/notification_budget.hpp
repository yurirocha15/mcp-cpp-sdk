#pragma once

#include <mcp/core/notification_rate_limit.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>

namespace mcp::detail {

/// Throws std::invalid_argument for a limit whose rate is negative or not a number.
inline void check_rate_limit(const NotificationRateLimit& limit) {
    if (!std::isfinite(limit.per_second) || limit.per_second < 0.0) {
        throw std::invalid_argument(
            "Notification rate limit per_second must be a finite number of zero or more");
    }
}

/// A token bucket for one kind of notification; see NotificationRateLimit. Safe from any thread.
class TokenBucket {
   public:
    using Clock = std::chrono::steady_clock;

    explicit TokenBucket(const NotificationRateLimit& limit, Clock::time_point now = Clock::now())
        : limit_(limit), tokens_(static_cast<double>(limit.burst)), refilled_(now) {}

    /// Whether one more notification may pass at @p now; takes a token when it may.
    bool take(Clock::time_point now = Clock::now()) {
        if (limit_.burst == 0) {
            return true;
        }
        std::scoped_lock lock(mutex_);
        const auto elapsed = std::chrono::duration<double>(now - refilled_).count();
        if (elapsed > 0.0) {
            tokens_ =
                std::min(static_cast<double>(limit_.burst), tokens_ + (elapsed * limit_.per_second));
            refilled_ = now;
        }
        if (tokens_ < 1.0) {
            return false;
        }
        tokens_ -= 1.0;
        return true;
    }

   private:
    NotificationRateLimit limit_;
    std::mutex mutex_;
    double tokens_;
    Clock::time_point refilled_;
};

/// The progress and log notifications one request may send, and where the dropped ones are counted.
class NotificationBudget {
   public:
    NotificationBudget(const NotificationRateLimit& limit,
                       std::shared_ptr<std::atomic<std::uint64_t>> dropped)
        : progress_(limit), log_(limit), dropped_(std::move(dropped)) {}

    /// Whether a progress notification may go; counts it as dropped when not.
    bool allow_progress() { return allow(progress_); }

    /// Whether a log notification may go; counts it as dropped when not.
    bool allow_log() { return allow(log_); }

   private:
    bool allow(TokenBucket& bucket) {
        if (bucket.take()) {
            return true;
        }
        if (dropped_) {
            dropped_->fetch_add(1, std::memory_order_relaxed);
        }
        return false;
    }

    TokenBucket progress_;
    TokenBucket log_;
    std::shared_ptr<std::atomic<std::uint64_t>> dropped_;
};

}  // namespace mcp::detail

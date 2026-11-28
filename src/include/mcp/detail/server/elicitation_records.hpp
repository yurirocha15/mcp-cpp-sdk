#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

namespace mcp::detail {

/**
 * @brief The URL-mode elicitations the sessions of a server sent, so that the notification that
 *        one completed goes to the session that sent it and to no other.
 *
 * @details A record maps an elicitation id to the session that sent the elicitation, a @p Target
 * with `bool same_session(const Target&) const` and `bool alive() const`. It ends when it is taken,
 * when it expires, when its session ends, and when a cap is reached: a session that holds
 * `max_per_session` records loses its oldest to the next, and the server's oldest record goes when
 * all sessions together hold `max_records`. Every member is safe to call from any thread.
 */
template <typename Target>
class ElicitationRecords {
   public:
    using Clock = std::chrono::steady_clock;

    ElicitationRecords(std::size_t max_records, std::size_t max_per_session, Clock::duration ttl)
        : max_records_(max_records), max_per_session_(max_per_session), ttl_(ttl) {}

    /**
     * @brief Records that @p target sent the elicitation @p id.
     *
     * @details A record of the same session with that id is replaced, so the session may send the
     * elicitation again.
     * @throws std::invalid_argument If another session's record holds @p id.
     */
    void record(const std::string& id, const Target& target, Clock::time_point now) {
        std::scoped_lock lock(mutex_);
        purge(now);
        if (const auto found = entries_.find(id); found != entries_.end()) {
            if (!found->second.target.same_session(target)) {
                throw std::invalid_argument(
                    "another session has an elicitation in progress with this elicitationId");
            }
            entries_.erase(found);
        }
        std::size_t of_session = 0;
        for (const auto& [key, entry] : entries_) {
            static_cast<void>(key);
            of_session += entry.target.same_session(target) ? 1 : 0;
        }
        if (of_session >= max_per_session_) {
            erase_oldest([&target](const Entry& entry) { return entry.target.same_session(target); });
        }
        if (entries_.size() >= max_records_) {
            erase_oldest([](const Entry&) { return true; });
        }
        entries_.emplace(id, Entry{.target = target, .expiry = now + ttl_, .order = next_order_++});
    }

    /// Takes the record of @p id: the session that sent it, or none when no live record holds it.
    std::optional<Target> take(const std::string& id, Clock::time_point now) {
        std::scoped_lock lock(mutex_);
        purge(now);
        const auto found = entries_.find(id);
        if (found == entries_.end()) {
            return std::nullopt;
        }
        auto target = std::move(found->second.target);
        entries_.erase(found);
        return target;
    }

    /// Takes the record of @p id only when @p target's session holds it; returns whether it did.
    bool take_own(const std::string& id, const Target& target, Clock::time_point now) {
        std::scoped_lock lock(mutex_);
        purge(now);
        const auto found = entries_.find(id);
        if (found == entries_.end() || !found->second.target.same_session(target)) {
            return false;
        }
        entries_.erase(found);
        return true;
    }

    /// Ends every record of @p target's session.
    void forget(const Target& target) {
        std::scoped_lock lock(mutex_);
        std::erase_if(entries_,
                      [&target](const auto& item) { return item.second.target.same_session(target); });
    }

    [[nodiscard]] std::size_t size() const {
        std::scoped_lock lock(mutex_);
        return entries_.size();
    }

   private:
    struct Entry {
        Target target;
        Clock::time_point expiry;
        std::uint64_t order{0};
    };

    /// Ends the records that expired or whose session ended.
    void purge(Clock::time_point now) {
        std::erase_if(entries_, [now](const auto& item) {
            return item.second.expiry <= now || !item.second.target.alive();
        });
    }

    template <typename Predicate>
    void erase_oldest(Predicate matches) {
        auto oldest = entries_.end();
        for (auto entry = entries_.begin(); entry != entries_.end(); ++entry) {
            if (matches(entry->second) &&
                (oldest == entries_.end() || entry->second.order < oldest->second.order)) {
                oldest = entry;
            }
        }
        if (oldest != entries_.end()) {
            entries_.erase(oldest);
        }
    }

    mutable std::mutex mutex_;
    std::unordered_map<std::string, Entry> entries_;
    std::uint64_t next_order_{0};
    std::size_t max_records_;
    std::size_t max_per_session_;
    Clock::duration ttl_;
};

}  // namespace mcp::detail

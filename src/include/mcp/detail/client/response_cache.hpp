#pragma once

#include <algorithm>
#include <mcp/client/client.hpp>
#include <mcp/core/constants.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace mcp::detail {

/// The results a client keeps for its cacheable calls; see ResponseCacheOptions.
class ResponseCache {
   public:
    using Clock = std::chrono::steady_clock;

    explicit ResponseCache(const ResponseCacheOptions& options) : options_(options) {}

    [[nodiscard]] bool enabled() const noexcept { return options_.enabled; }

    /// The key a call is kept under: its method and its parameters.
    [[nodiscard]] static std::string key(std::string_view method,
                                         const std::optional<nlohmann::json>& params) {
        std::string result(method);
        result += '\n';
        if (params) {
            result += params->dump();
        }
        return result;
    }

    /// The result kept under @p key while it is fresh at @p now.
    [[nodiscard]] std::optional<nlohmann::json> find(const std::string& key,
                                                     Clock::time_point now = Clock::now()) {
        std::scoped_lock lock(mutex_);
        const auto found = entries_.find(key);
        if (found == entries_.end()) {
            return std::nullopt;
        }
        if (now >= found->second.expires) {
            entries_.erase(found);
            return std::nullopt;
        }
        return found->second.result;
    }

    /// Read before a call whose result may be kept; store() keeps nothing when a notification
    /// dropped results since.
    [[nodiscard]] std::uint64_t generation() const {
        std::scoped_lock lock(mutex_);
        return generation_;
    }

    /// Keeps @p result of @p method with @p params, received at @p received, for as long as its
    /// `ttlMs` says, capped by ResponseCacheOptions::max_ttl. A result that names no positive
    /// `ttlMs` replaces what was kept under @p key with nothing.
    void store(const std::string& key, std::string_view method,
               const std::optional<nlohmann::json>& params, const nlohmann::json& result,
               std::uint64_t since, Clock::time_point received = Clock::now()) {
        const auto ttl = fresh_for(result);
        std::scoped_lock lock(mutex_);
        if (ttl <= std::chrono::milliseconds::zero()) {
            entries_.erase(key);
            return;
        }
        if (generation_ != since) {
            return;
        }
        std::erase_if(entries_,
                      [received](const auto& entry) { return received >= entry.second.expires; });
        if (entries_.size() >= options_.max_entries && !entries_.contains(key)) {
            entries_.erase(std::min_element(entries_.begin(), entries_.end(),
                                            [](const auto& left, const auto& right) {
                                                return left.second.expires < right.second.expires;
                                            }));
        }
        const auto room =
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::time_point::max() - received);
        Entry entry;
        entry.method = std::string(method);
        if (params && params->is_object()) {
            const auto uri = params->find("uri");
            if (uri != params->end() && uri->is_string()) {
                entry.uri = uri->get<std::string>();
            }
        }
        entry.result = result;
        entry.expires = ttl >= room ? Clock::time_point::max() : received + ttl;
        entries_[key] = std::move(entry);
    }

    /// Drops what the notification @p method in @p message makes stale: the lists a
    /// `list_changed` names, the reads of the URI `notifications/resources/updated` names.
    void invalidate(std::string_view method, const nlohmann::json& message) {
        if (method == mcp::constants::methods::g_NOTIFICATIONS_RESOURCES_UPDATED) {
            const auto params = message.find("params");
            if (params == message.end() || !params->is_object()) {
                return;
            }
            const auto uri = params->find("uri");
            if (uri == params->end() || !uri->is_string()) {
                return;
            }
            const auto& name = uri->get_ref<const std::string&>();
            std::scoped_lock lock(mutex_);
            ++generation_;
            std::erase_if(entries_, [&name](const auto& entry) {
                return entry.second.method == mcp::constants::methods::g_RESOURCES_READ &&
                       entry.second.uri == name;
            });
            return;
        }
        std::string_view first;
        std::string_view second;
        if (method == mcp::constants::methods::g_NOTIFICATIONS_TOOLS_LIST_CHANGED) {
            first = mcp::constants::methods::g_TOOLS_LIST;
        } else if (method == mcp::constants::methods::g_NOTIFICATIONS_PROMPTS_LIST_CHANGED) {
            first = mcp::constants::methods::g_PROMPTS_LIST;
        } else if (method == mcp::constants::methods::g_NOTIFICATIONS_RESOURCES_LIST_CHANGED) {
            first = mcp::constants::methods::g_RESOURCES_LIST;
            second = mcp::constants::methods::g_RESOURCES_TEMPLATES_LIST;
        } else {
            return;
        }
        std::scoped_lock lock(mutex_);
        ++generation_;
        std::erase_if(entries_, [first, second](const auto& entry) {
            return entry.second.method == first || entry.second.method == second;
        });
    }

    /// Drops every result of @p method, as when a server refuses a cursor it handed out: its
    /// pages may no longer fit together.
    void forget(std::string_view method) {
        std::scoped_lock lock(mutex_);
        ++generation_;
        std::erase_if(entries_, [method](const auto& entry) { return entry.second.method == method; });
    }

    /// Drops everything.
    void clear() {
        std::scoped_lock lock(mutex_);
        ++generation_;
        entries_.clear();
    }

    [[nodiscard]] std::size_t size() const {
        std::scoped_lock lock(mutex_);
        return entries_.size();
    }

   private:
    struct Entry {
        std::string method;
        /// The URI a resources/read names; empty for a list.
        std::string uri;
        nlohmann::json result;
        Clock::time_point expires;
    };

    /// How long @p result is fresh: its `ttlMs` capped by max_ttl, or zero when it names none, a
    /// negative one or zero.
    [[nodiscard]] std::chrono::milliseconds fresh_for(const nlohmann::json& result) const {
        if (!result.is_object()) {
            return std::chrono::milliseconds::zero();
        }
        const auto ttl = result.find("ttlMs");
        if (ttl == result.end() || !ttl->is_number_integer()) {
            return std::chrono::milliseconds::zero();
        }
        std::int64_t milliseconds = 0;
        if (ttl->is_number_unsigned()) {
            const auto value = ttl->get<std::uint64_t>();
            constexpr auto most = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
            milliseconds = static_cast<std::int64_t>(std::min(value, most));
        } else {
            milliseconds = ttl->get<std::int64_t>();
        }
        if (milliseconds <= 0) {
            return std::chrono::milliseconds::zero();
        }
        return std::min(std::chrono::milliseconds(milliseconds), options_.max_ttl);
    }

    ResponseCacheOptions options_;
    mutable std::mutex mutex_;
    std::map<std::string, Entry, std::less<>> entries_;
    std::uint64_t generation_{0};
};

}  // namespace mcp::detail

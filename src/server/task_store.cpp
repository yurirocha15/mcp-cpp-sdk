#include <mcp/server/task_store.hpp>
#include "../detail/text/secure_random.hpp"

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace mcp {

namespace {

// Days since 1970-01-01 to a civil date (Howard Hinnant's algorithm), so formatting needs neither
// gmtime nor the platform's thread-safe variants of it.
void civil_from_days(std::int64_t days, std::int64_t& year, unsigned& month, unsigned& day) {
    days += 719468;
    const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const auto day_of_era = static_cast<unsigned>(days - era * 146097);
    const unsigned year_of_era =
        (day_of_era - day_of_era / 1460 + day_of_era / 36524 - day_of_era / 146096) / 365;
    year = static_cast<std::int64_t>(year_of_era) + era * 400;
    const unsigned day_of_year = day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    const unsigned shifted_month = (5 * day_of_year + 2) / 153;
    day = day_of_year - (153 * shifted_month + 2) / 5 + 1;
    month = shifted_month < 10 ? shifted_month + 3 : shifted_month - 9;
    year += month <= 2 ? 1 : 0;
}

std::string two_digits(unsigned value) {
    return std::string{static_cast<char>('0' + value / 10 % 10), static_cast<char>('0' + value % 10)};
}

}  // namespace

std::string format_task_time(std::chrono::system_clock::time_point time) {
    using namespace std::chrono;
    const auto total_ms = duration_cast<milliseconds>(time.time_since_epoch()).count();
    std::int64_t seconds = total_ms / 1000;
    std::int64_t millis = total_ms % 1000;
    if (millis < 0) {
        millis += 1000;
        --seconds;
    }
    std::int64_t days = seconds / 86400;
    std::int64_t second_of_day = seconds % 86400;
    if (second_of_day < 0) {
        second_of_day += 86400;
        --days;
    }
    std::int64_t year = 0;
    unsigned month = 0;
    unsigned day = 0;
    civil_from_days(days, year, month, day);

    std::string text = std::to_string(year);
    while (text.size() < 4) {
        text.insert(text.begin(), '0');
    }
    text += '-' + two_digits(month) + '-' + two_digits(day) + 'T';
    text += two_digits(static_cast<unsigned>(second_of_day / 3600)) + ':' +
            two_digits(static_cast<unsigned>(second_of_day / 60 % 60)) + ':' +
            two_digits(static_cast<unsigned>(second_of_day % 60)) + '.';
    text += static_cast<char>('0' + millis / 100);
    text += static_cast<char>('0' + millis / 10 % 10);
    text += static_cast<char>('0' + millis % 10);
    text += 'Z';
    return text;
}

DetailedTask TaskRecord::to_detailed() const {
    DetailedTask detailed;
    detailed.task.taskId = id;
    detailed.task.status = status;
    detailed.task.statusMessage = status_message;
    detailed.task.createdAt = format_task_time(created);
    detailed.task.lastUpdatedAt = format_task_time(updated);
    if (ttl) {
        detailed.task.ttlMs = ttl->count();
    }
    if (poll_interval) {
        detailed.task.pollIntervalMs = poll_interval->count();
    }
    switch (status) {
        case TaskStatus::eInputRequired:
            detailed.inputRequests = input_requests;
            break;
        case TaskStatus::eCompleted:
            detailed.result = result ? *result : nlohmann::json::object();
            break;
        case TaskStatus::eFailed:
            detailed.error = error ? *error : nlohmann::json::object();
            break;
        case TaskStatus::eWorking:
        case TaskStatus::eCancelled:
            break;
    }
    return detailed;
}

struct InMemoryTaskStore::Impl {
    explicit Impl(InMemoryTaskStoreOptions opts) : options(std::move(opts)) {}

    std::chrono::system_clock::time_point now() const {
        return options.clock ? options.clock() : std::chrono::system_clock::now();
    }

    static bool expired(const TaskRecord& record, std::chrono::system_clock::time_point at) {
        return record.ttl && at >= record.created + *record.ttl;
    }

    // Drops every expired task. Called with `mutex` held.
    void purge_expired(std::chrono::system_clock::time_point at) {
        for (auto iter = tasks.begin(); iter != tasks.end();) {
            if (expired(iter->second, at)) {
                iter = tasks.erase(iter);
            } else {
                ++iter;
            }
        }
    }

    // The live task @p id, dropping it first when it expired. Called with `mutex` held.
    TaskRecord* find(std::string_view id) {
        const auto iter = tasks.find(std::string(id));
        if (iter == tasks.end()) {
            return nullptr;
        }
        if (expired(iter->second, now())) {
            tasks.erase(iter);
            return nullptr;
        }
        return &iter->second;
    }

    // Erases the oldest finished task that @p owner matches (every owner when null). Returns
    // whether there was one. Called with `mutex` held.
    bool evict_finished(const std::string* owner) {
        auto oldest = tasks.end();
        for (auto iter = tasks.begin(); iter != tasks.end(); ++iter) {
            if (!iter->second.terminal() || (owner != nullptr && iter->second.owner != *owner)) {
                continue;
            }
            if (oldest == tasks.end() || iter->second.created < oldest->second.created) {
                oldest = iter;
            }
        }
        if (oldest == tasks.end()) {
            return false;
        }
        tasks.erase(oldest);
        return true;
    }

    // Applies @p change to the unfinished task @p id and returns its state afterwards, or nullopt
    // when there is no such task or it has finished.
    template <typename Change>
    std::optional<TaskRecord> change_open(std::string_view id, Change&& change) {
        std::lock_guard lock(mutex);
        auto* record = find(id);
        if (record == nullptr || record->terminal()) {
            return std::nullopt;
        }
        if (!change(*record)) {
            return std::nullopt;
        }
        record->updated = now();
        return *record;
    }

    InMemoryTaskStoreOptions options;
    std::mutex mutex;
    std::unordered_map<std::string, TaskRecord> tasks;  // guarded by mutex
};

InMemoryTaskStore::InMemoryTaskStore(InMemoryTaskStoreOptions options)
    : impl_(std::make_shared<Impl>(std::move(options))) {}

std::optional<TaskRecord> InMemoryTaskStore::create(
    std::string_view owner, std::optional<std::chrono::milliseconds> ttl,
    std::optional<std::chrono::milliseconds> poll_interval) {
    const std::string owner_text(owner);
    std::string id = mcp::detail::generate_secure_session_id();

    std::lock_guard lock(impl_->mutex);
    const auto at = impl_->now();
    impl_->purge_expired(at);
    const auto owned = static_cast<std::size_t>(
        std::count_if(impl_->tasks.begin(), impl_->tasks.end(),
                      [&owner_text](const auto& entry) { return entry.second.owner == owner_text; }));
    if (owned >= impl_->options.max_tasks_per_owner && !impl_->evict_finished(&owner_text)) {
        return std::nullopt;
    }
    if (impl_->tasks.size() >= impl_->options.max_tasks && !impl_->evict_finished(nullptr)) {
        return std::nullopt;
    }

    TaskRecord record;
    record.owner = owner_text;
    record.id = id;
    record.created = at;
    record.updated = at;
    record.ttl = ttl;
    record.poll_interval = poll_interval;
    impl_->tasks.emplace(std::move(id), record);
    return record;
}

std::optional<TaskRecord> InMemoryTaskStore::get(std::string_view owner, std::string_view id) {
    std::lock_guard lock(impl_->mutex);
    const auto* record = impl_->find(id);
    if (record == nullptr || record->owner != owner) {
        return std::nullopt;
    }
    return *record;
}

std::optional<TaskRecord> InMemoryTaskStore::set_status_message(std::string_view id,
                                                                std::string message) {
    return impl_->change_open(id, [&message](TaskRecord& record) {
        record.status_message = std::move(message);
        return true;
    });
}

std::optional<TaskRecord> InMemoryTaskStore::add_input_request(std::string_view id,
                                                               std::string_view key,
                                                               nlohmann::json request) {
    return impl_->change_open(id, [&key, &request](TaskRecord& record) {
        record.input_requests[std::string(key)] = std::move(request);
        record.status = TaskStatus::eInputRequired;
        return true;
    });
}

std::optional<TaskRecord> InMemoryTaskStore::submit_input_responses(std::string_view id,
                                                                    const nlohmann::json& responses) {
    if (!responses.is_object()) {
        return std::nullopt;
    }
    return impl_->change_open(id, [&responses](TaskRecord& record) {
        bool recorded = false;
        for (const auto& [key, response] : responses.items()) {
            const auto outstanding = record.input_requests.find(key);
            if (outstanding == record.input_requests.end()) {
                continue;
            }
            record.input_responses[key] = response;
            record.input_requests.erase(outstanding);
            recorded = true;
        }
        if (recorded && record.input_requests.empty() && record.status == TaskStatus::eInputRequired) {
            record.status = TaskStatus::eWorking;
        }
        return recorded;
    });
}

std::optional<nlohmann::json> InMemoryTaskStore::take_input_response(std::string_view id,
                                                                     std::string_view key) {
    std::lock_guard lock(impl_->mutex);
    auto* record = impl_->find(id);
    if (record == nullptr) {
        return std::nullopt;
    }
    const auto found = record->input_responses.find(std::string(key));
    if (found == record->input_responses.end()) {
        return std::nullopt;
    }
    nlohmann::json response = std::move(*found);
    record->input_responses.erase(found);
    return response;
}

std::optional<TaskRecord> InMemoryTaskStore::complete(std::string_view id, nlohmann::json result) {
    return impl_->change_open(id, [&result](TaskRecord& record) {
        record.status = TaskStatus::eCompleted;
        record.result = std::move(result);
        record.input_requests = nlohmann::json::object();
        return true;
    });
}

std::optional<TaskRecord> InMemoryTaskStore::fail(std::string_view id, nlohmann::json error,
                                                  std::string message) {
    return impl_->change_open(id, [&error, &message](TaskRecord& record) {
        record.status = TaskStatus::eFailed;
        record.error = std::move(error);
        record.status_message = std::move(message);
        record.input_requests = nlohmann::json::object();
        return true;
    });
}

std::optional<TaskRecord> InMemoryTaskStore::cancel(std::string_view id, std::string message) {
    return impl_->change_open(id, [&message](TaskRecord& record) {
        record.status = TaskStatus::eCancelled;
        record.status_message = std::move(message);
        record.input_requests = nlohmann::json::object();
        return true;
    });
}

std::size_t InMemoryTaskStore::size() {
    std::lock_guard lock(impl_->mutex);
    impl_->purge_expired(impl_->now());
    return impl_->tasks.size();
}

}  // namespace mcp

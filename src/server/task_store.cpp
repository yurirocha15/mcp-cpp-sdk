#include <mcp/server/task_store.hpp>
#include "mcp/detail/text/secure_random.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

namespace mcp {

namespace {

/// Appends @p value in decimal, with leading zeros to @p width digits.
void append_padded(std::string& text, long long value, std::size_t width) {
    const std::string digits = std::to_string(value);
    if (digits.size() < width) {
        text.append(width - digits.size(), '0');
    }
    text += digits;
}

}  // namespace

std::string format_task_time(std::chrono::system_clock::time_point time) {
    using namespace std::chrono;
    const auto day = floor<days>(time);
    const year_month_day date{day};
    const hh_mm_ss clock{floor<milliseconds>(time - day)};
    std::string text;
    append_padded(text, static_cast<int>(date.year()), 4);
    text += '-';
    append_padded(text, static_cast<unsigned>(date.month()), 2);
    text += '-';
    append_padded(text, static_cast<unsigned>(date.day()), 2);
    text += 'T';
    append_padded(text, clock.hours().count(), 2);
    text += ':';
    append_padded(text, clock.minutes().count(), 2);
    text += ':';
    append_padded(text, clock.seconds().count(), 2);
    text += '.';
    append_padded(text, clock.subseconds().count(), 3);
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
        std::scoped_lock lock(mutex);
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

    std::scoped_lock lock(impl_->mutex);
    const auto at = impl_->now();
    impl_->purge_expired(at);
    const auto owned = static_cast<std::size_t>(std::ranges::count_if(
        impl_->tasks, [&owner_text](const auto& entry) { return entry.second.owner == owner_text; }));
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
    std::scoped_lock lock(impl_->mutex);
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
    std::scoped_lock lock(impl_->mutex);
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
    std::scoped_lock lock(impl_->mutex);
    impl_->purge_expired(impl_->now());
    return impl_->tasks.size();
}

}  // namespace mcp

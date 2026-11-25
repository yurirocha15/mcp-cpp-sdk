#include <algorithm>
#include <mcp/client/subscription.hpp>

#include "client_impl.hpp"

namespace mcp {

void to_json(nlohmann::json& json_obj, const SubscriptionFilter& filter) {
    json_obj = nlohmann::json::object();
    if (filter.toolsListChanged) {
        json_obj["toolsListChanged"] = true;
    }
    if (filter.promptsListChanged) {
        json_obj["promptsListChanged"] = true;
    }
    if (filter.resourcesListChanged) {
        json_obj["resourcesListChanged"] = true;
    }
    if (!filter.resourceSubscriptions.empty()) {
        json_obj["resourceSubscriptions"] = filter.resourceSubscriptions;
    }
    if (!filter.taskIds.empty()) {
        json_obj["taskIds"] = filter.taskIds;
    }
}

void from_json(const nlohmann::json& json_obj, SubscriptionFilter& filter) {
    filter = SubscriptionFilter{};
    filter.toolsListChanged = json_obj.value("toolsListChanged", false);
    filter.promptsListChanged = json_obj.value("promptsListChanged", false);
    filter.resourcesListChanged = json_obj.value("resourcesListChanged", false);
    if (const auto found = json_obj.find("resourceSubscriptions");
        found != json_obj.end() && !found->is_null()) {
        found->get_to(filter.resourceSubscriptions);
    }
    if (const auto found = json_obj.find("taskIds"); found != json_obj.end() && !found->is_null()) {
        found->get_to(filter.taskIds);
    }
}

Subscription::Subscription() = default;

Subscription::Subscription(std::shared_ptr<detail::SubscriptionState> state)
    : state_(std::move(state)) {}

RequestId Subscription::id() const { return state_ ? state_->id : RequestId{}; }

SubscriptionFilter Subscription::requested() const {
    return state_ ? state_->requested : SubscriptionFilter{};
}

SubscriptionFilter Subscription::acknowledged() const {
    if (!state_) {
        return {};
    }
    std::scoped_lock lock(state_->mutex);
    return state_->acknowledged;
}

SubscriptionFilter Subscription::unhonored() const {
    const auto honored = acknowledged();
    const auto asked = requested();
    SubscriptionFilter missing;
    missing.toolsListChanged = asked.toolsListChanged && !honored.toolsListChanged;
    missing.promptsListChanged = asked.promptsListChanged && !honored.promptsListChanged;
    missing.resourcesListChanged = asked.resourcesListChanged && !honored.resourcesListChanged;
    for (const auto& uri : asked.resourceSubscriptions) {
        if (std::ranges::find(honored.resourceSubscriptions, uri) ==
            honored.resourceSubscriptions.end()) {
            missing.resourceSubscriptions.push_back(uri);
        }
    }
    for (const auto& id : asked.taskIds) {
        if (std::ranges::find(honored.taskIds, id) == honored.taskIds.end()) {
            missing.taskIds.push_back(id);
        }
    }
    return missing;
}

Task<std::optional<SubscriptionEvent>> Subscription::next() {
    if (!state_) {
        return no_event();
    }
    return detail::spawn_awaitable(state_->wake.get_executor(),
                                   detail::SubscriptionState::wait_next(state_));
}

Task<void> Subscription::close() {
    if (!state_) {
        return nothing_to_send();
    }
    return detail::spawn_awaitable(state_->wake.get_executor(),
                                   detail::SubscriptionState::cancel(state_));
}

std::optional<SubscriptionEnd> Subscription::end_reason() const {
    if (!state_) {
        return SubscriptionEnd::eCancelled;
    }
    std::scoped_lock lock(state_->mutex);
    return state_->ended;
}

std::optional<Error> Subscription::end_error() const {
    if (!state_) {
        return std::nullopt;
    }
    std::scoped_lock lock(state_->mutex);
    return state_->failure;
}

std::uint64_t Subscription::dropped_events() const {
    if (!state_) {
        return 0;
    }
    std::scoped_lock lock(state_->mutex);
    return state_->dropped;
}

SyncSubscription Subscription::sync() const {
    if (!state_ || !state_->blocking_loop) {
        throw std::logic_error(
            "mcp::Subscription::sync() needs a subscription of a client constructed from an "
            "Endpoint");
    }
    return SyncSubscription(*this);
}

}  // namespace mcp

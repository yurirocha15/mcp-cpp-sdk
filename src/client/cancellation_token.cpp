#include <mcp/client/cancellation_token.hpp>

#include "client_impl.hpp"

namespace mcp {

CancellationToken::CancellationToken() : state_(std::make_shared<detail::CancellationState>()) {}

void CancellationToken::cancel(std::optional<std::string> reason) const {
    std::map<std::uint64_t, detail::CancellationState::Callback> callbacks;
    {
        std::scoped_lock lock(state_->mutex);
        if (state_->cancelled.load(std::memory_order_relaxed)) {
            return;
        }
        state_->reason = std::move(reason);
        state_->cancelled.store(true, std::memory_order_release);
        callbacks.swap(state_->callbacks);
    }
    // Run outside the lock: a callback may end a wait that removes its own registration.
    for (auto& [registration, callback] : callbacks) {
        callback(state_->reason);
    }
}

bool CancellationToken::is_cancelled() const noexcept {
    return state_->cancelled.load(std::memory_order_acquire);
}

}  // namespace mcp

#pragma once

// Linux only: the gate is driven by a getaddrinfo() definition in the test binary (see
// resolve_gate.cpp), which is how a test parks a real Asio resolve past its cancel check.
#ifdef __linux__

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>

/// Holds one getaddrinfo() call for an armed port inside the call, which is past the only point at
/// which Asio's resolver thread looks at its cancel token. Calls for any other port pass straight
/// through, so only the test that arms the gate is affected. Every wait is bounded.
class ResolveGate final {
   public:
    void arm(unsigned short port) {
        std::lock_guard lock(mutex_);
        entered_ = false;
        released_ = false;
        armed_port_.store(port);
    }

    /// Let a held call go and stop holding new ones.
    void release() {
        armed_port_.store(0);
        {
            std::lock_guard lock(mutex_);
            released_ = true;
        }
        changed_.notify_all();
    }

    [[nodiscard]] bool wait_until_entered(std::chrono::seconds limit) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, limit, [this]() { return entered_; });
    }

    void hold_if_armed(const char* service) {
        const auto port = armed_port_.load();
        if (port == 0 || service == nullptr || std::to_string(port) != service) {
            return;
        }
        std::unique_lock lock(mutex_);
        entered_ = true;
        changed_.notify_all();
        // Longer than any wait in the test, and still bounded: a test that fails without
        // releasing cannot leave the resolver thread parked forever.
        changed_.wait_for(lock, std::chrono::seconds(30), [this]() { return released_; });
    }

   private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::atomic<int> armed_port_{0};
    bool entered_{false};
    bool released_{false};
};

/// The one gate the binary's getaddrinfo() consults.
ResolveGate& resolve_gate();

#endif  // __linux__

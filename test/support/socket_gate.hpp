#pragma once

// Linux only: the gate is driven by a socket() definition in the test binary (see
// socket_gate.cpp), which is how a test parks an Asio connect at the point its socket is opened.
#ifdef __linux__

#include <sys/socket.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>

/// Holds one socket() call for a TCP socket inside the call, once armed. Inside async_connect()
/// that call is the socket being opened, so a held exchange has passed its last look at the abort
/// latch and has nothing open yet. Calls before and after the held one pass straight through.
/// Every wait is bounded.
class SocketGate final {
   public:
    /// Hold the TCP socket() call that follows `calls_to_pass` others.
    void arm(int calls_to_pass) {
        std::lock_guard lock(mutex_);
        entered_ = false;
        released_ = false;
        countdown_.store(calls_to_pass + 1);
    }

    /// Let a held call go and stop holding new ones.
    void release() {
        countdown_.store(0);
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

    void hold_if_armed(int domain, int type) {
        if (domain != AF_INET || (type & ~(SOCK_NONBLOCK | SOCK_CLOEXEC)) != SOCK_STREAM) {
            return;
        }
        auto countdown = countdown_.load();
        do {
            if (countdown == 0) {
                return;
            }
        } while (!countdown_.compare_exchange_weak(countdown, countdown - 1));
        if (countdown != 1) {
            return;
        }
        std::unique_lock lock(mutex_);
        entered_ = true;
        changed_.notify_all();
        // Longer than any wait in the test, and still bounded: a test that fails without
        // releasing cannot leave an io thread parked forever.
        changed_.wait_for(lock, std::chrono::seconds(30), [this]() { return released_; });
    }

   private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::atomic<int> countdown_{0};
    bool entered_{false};
    bool released_{false};
};

/// The one gate the binary's socket() consults.
SocketGate& socket_gate();

#endif  // __linux__

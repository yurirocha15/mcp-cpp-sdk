#pragma once

#include <cstddef>
#include <utility>
#include <vector>

namespace mcp::detail {

/// A first-in, first-out queue that allocates nothing until its first push. libstdc++'s
/// std::deque, and so std::queue, allocates a map and a 512-byte block when it is constructed,
/// which an object built per session pays even while it queues nothing. The elements live in one
/// vector read from an index; the consumed front is dropped when the queue empties, or once it is
/// at least half the vector, so a queue that never empties holds at most about twice its length.
template <typename T>
class Fifo {
   public:
    [[nodiscard]] bool empty() const noexcept { return head_ == items_.size(); }
    [[nodiscard]] std::size_t size() const noexcept { return items_.size() - head_; }

    T& front() noexcept { return items_[head_]; }

    template <typename... Args>
    void emplace_back(Args&&... args) {
        items_.emplace_back(std::forward<Args>(args)...);
    }

    void pop_front() {
        items_[head_] = T();
        ++head_;
        if (head_ == items_.size()) {
            clear();
        } else if (head_ >= g_compact_after && head_ * 2 >= items_.size()) {
            items_.erase(items_.begin(), items_.begin() + static_cast<std::ptrdiff_t>(head_));
            head_ = 0;
        }
    }

    void clear() noexcept {
        items_.clear();
        head_ = 0;
    }

    auto begin() noexcept { return items_.begin() + static_cast<std::ptrdiff_t>(head_); }
    auto end() noexcept { return items_.end(); }

   private:
    static constexpr std::size_t g_compact_after = 16;

    std::vector<T> items_;
    std::size_t head_ = 0;
};

}  // namespace mcp::detail

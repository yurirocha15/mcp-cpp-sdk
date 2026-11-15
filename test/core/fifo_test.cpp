#include "../../src/detail/fifo.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

namespace {

using mcp::detail::Fifo;

TEST(Fifo, KeepsOrderAcrossInterleavedPushesAndPops) {
    Fifo<std::string> queue;
    EXPECT_TRUE(queue.empty());
    int next_in = 0;
    int next_out = 0;
    // The queue holds between 1 and 40 elements for a long time, so its consumed front is dropped
    // while elements are still queued, many times over.
    for (int round = 0; round < 200; ++round) {
        const int pushes = round % 2 == 0 ? 7 : 3;
        for (int i = 0; i < pushes && next_in < 600; ++i) {
            queue.emplace_back(std::to_string(next_in++));
        }
        for (int i = 0; i < 4 && !queue.empty(); ++i) {
            EXPECT_EQ(queue.front(), std::to_string(next_out++));
            queue.pop_front();
        }
        EXPECT_EQ(queue.size(), static_cast<std::size_t>(next_in - next_out));
    }
    while (!queue.empty()) {
        EXPECT_EQ(queue.front(), std::to_string(next_out++));
        queue.pop_front();
    }
    EXPECT_EQ(next_out, 600);
}

TEST(Fifo, IteratesOverTheQueuedElementsOnly) {
    Fifo<int> queue;
    for (int i = 0; i < 5; ++i) {
        queue.emplace_back(i);
    }
    queue.pop_front();
    queue.pop_front();
    std::vector<int> seen(queue.begin(), queue.end());
    EXPECT_EQ(seen, (std::vector<int>{2, 3, 4}));
    queue.clear();
    EXPECT_TRUE(queue.empty());
    EXPECT_EQ(queue.begin(), queue.end());
}

// A popped element is released at once, not when the queue next empties.
TEST(Fifo, PopReleasesTheElement) {
    Fifo<std::shared_ptr<int>> queue;
    auto first = std::make_shared<int>(1);
    queue.emplace_back(first);
    queue.emplace_back(std::make_shared<int>(2));
    EXPECT_EQ(first.use_count(), 2);
    queue.pop_front();
    EXPECT_EQ(first.use_count(), 1);
    EXPECT_EQ(*queue.front(), 2);
}

}  // namespace

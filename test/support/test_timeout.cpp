/// Holds every test to the per-test timeout. ctest runs a whole suite in one process (see
/// cmake/McpCppSdkTestSuites.cmake), so its own timeout covers the suite; this ends the process,
/// naming the test, when one test runs longer than MCP_CPP_SDK_TEST_TIMEOUT_SECONDS.

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#ifndef MCP_CPP_SDK_TEST_TIMEOUT_SECONDS
#define MCP_CPP_SDK_TEST_TIMEOUT_SECONDS 120
#endif

namespace {

constexpr std::chrono::seconds g_test_timeout{MCP_CPP_SDK_TEST_TIMEOUT_SECONDS};

struct WatchState {
    std::mutex mutex;
    std::condition_variable changed;
    std::string test;
    std::chrono::steady_clock::time_point deadline;
    bool running = false;
    bool stopping = false;
};

void watch(const std::shared_ptr<WatchState>& state) {
    std::unique_lock<std::mutex> lock(state->mutex);
    while (!state->stopping) {
        if (!state->running) {
            state->changed.wait(lock);
            continue;
        }
        if (state->changed.wait_until(lock, state->deadline) == std::cv_status::timeout &&
            state->running && std::chrono::steady_clock::now() >= state->deadline) {
            std::fprintf(stderr, "\n%s ran longer than %lld seconds\n", state->test.c_str(),
                         static_cast<long long>(g_test_timeout.count()));
            std::fflush(stderr);
            std::_Exit(EXIT_FAILURE);
        }
    }
}

// The watcher thread is detached and shares its state, so destroying the listener never touches
// a thread. A child that a test forks inherits the listener but not the thread, and gtest deletes
// the listener when the child exits.
class TestTimeout : public ::testing::EmptyTestEventListener {
   public:
    void OnTestProgramStart(const ::testing::UnitTest& /*unit_test*/) override {
        std::thread([state = state_] { watch(state); }).detach();
    }

    void OnTestStart(const ::testing::TestInfo& info) override {
        const std::lock_guard<std::mutex> lock(state_->mutex);
        state_->test = std::string(info.test_suite_name()) + "." + info.name();
        state_->deadline = std::chrono::steady_clock::now() + g_test_timeout;
        state_->running = true;
        state_->changed.notify_one();
    }

    void OnTestEnd(const ::testing::TestInfo& /*info*/) override {
        const std::lock_guard<std::mutex> lock(state_->mutex);
        state_->running = false;
        state_->changed.notify_one();
    }

    void OnTestProgramEnd(const ::testing::UnitTest& /*unit_test*/) override {
        const std::lock_guard<std::mutex> lock(state_->mutex);
        state_->stopping = true;
        state_->changed.notify_one();
    }

   private:
    std::shared_ptr<WatchState> state_ = std::make_shared<WatchState>();
};

// gtest owns and deletes the listener.
[[maybe_unused]] const bool g_test_timeout_registered = [] {
    ::testing::UnitTest::GetInstance()->listeners().Append(new TestTimeout);
    return true;
}();

}  // namespace

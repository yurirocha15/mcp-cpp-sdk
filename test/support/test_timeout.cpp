/// Holds every test to the per-test timeout. ctest runs a whole suite in one process (see
/// cmake/McpCppSdkTestSuites.cmake), so its own timeout covers the suite; this ends the process,
/// naming the test, when one test runs longer than MCP_CPP_SDK_TEST_TIMEOUT_SECONDS.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

#ifdef _WIN32
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#else
#include <signal.h>
#include <unistd.h>

#include <array>
#include <cstring>
#endif

#ifndef MCP_CPP_SDK_TEST_TIMEOUT_SECONDS
#define MCP_CPP_SDK_TEST_TIMEOUT_SECONDS 120
#endif

namespace {

constexpr std::chrono::seconds g_test_timeout{MCP_CPP_SDK_TEST_TIMEOUT_SECONDS};

std::string overrun_message(const ::testing::TestInfo& info) {
    return "\n" + std::string(info.test_suite_name()) + "." + info.name() + " ran longer than " +
           std::to_string(g_test_timeout.count()) + " seconds\n";
}

#ifdef _WIN32

struct WatchState {
    std::mutex mutex;
    std::condition_variable changed;
    std::string message;
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
            std::fputs(state->message.c_str(), stderr);
            std::fflush(stderr);
            std::_Exit(EXIT_FAILURE);
        }
    }
}

// The watcher thread is detached and shares its state, so destroying the listener never touches
// a thread.
class TestTimeout : public ::testing::EmptyTestEventListener {
   public:
    void OnTestProgramStart(const ::testing::UnitTest& /*unit_test*/) override {
        std::thread([state = state_] { watch(state); }).detach();
    }

    void OnTestStart(const ::testing::TestInfo& info) override {
        const std::lock_guard<std::mutex> lock(state_->mutex);
        state_->message = overrun_message(info);
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

#else

// POSIX uses alarm() instead of a watcher thread, so the listener adds no thread to a process
// that a test forks: ThreadSanitizer stops a child of a multi-threaded process when the child
// starts a thread. A forked child does not inherit the alarm.
std::array<char, 1024> g_overrun_message{};
volatile sig_atomic_t g_overrun_length = 0;

extern "C" void on_test_overrun(int /*signal*/) {
    [[maybe_unused]] const auto written =
        write(STDERR_FILENO, g_overrun_message.data(), static_cast<size_t>(g_overrun_length));
    _exit(EXIT_FAILURE);
}

class TestTimeout : public ::testing::EmptyTestEventListener {
   public:
    void OnTestProgramStart(const ::testing::UnitTest& /*unit_test*/) override {
        struct sigaction action{};
        action.sa_handler = on_test_overrun;
        sigemptyset(&action.sa_mask);
        sigaction(SIGALRM, &action, nullptr);
    }

    void OnTestStart(const ::testing::TestInfo& info) override {
        const std::string message = overrun_message(info);
        const size_t length = std::min(message.size(), g_overrun_message.size());
        std::memcpy(g_overrun_message.data(), message.data(), length);
        g_overrun_length = static_cast<sig_atomic_t>(length);
        alarm(static_cast<unsigned int>(g_test_timeout.count()));
    }

    void OnTestEnd(const ::testing::TestInfo& /*info*/) override { alarm(0); }
};

#endif

// gtest owns and deletes the listener.
[[maybe_unused]] const bool g_test_timeout_registered = [] {
    ::testing::UnitTest::GetInstance()->listeners().Append(new TestTimeout);
    return true;
}();

}  // namespace

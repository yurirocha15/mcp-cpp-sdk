/// Holds every test to the per-test timeout. ctest runs a whole suite in one process (see
/// cmake/McpCppSdkTestSuites.cmake), so its own timeout covers the suite; this ends the process,
/// naming the test, when one test runs longer than MCP_CPP_SDK_TEST_TIMEOUT_SECONDS.

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>

#ifndef MCP_CPP_SDK_TEST_TIMEOUT_SECONDS
#define MCP_CPP_SDK_TEST_TIMEOUT_SECONDS 120
#endif

namespace {

constexpr std::chrono::seconds g_test_timeout{MCP_CPP_SDK_TEST_TIMEOUT_SECONDS};

class TestTimeout : public ::testing::EmptyTestEventListener {
   public:
    void OnTestProgramStart(const ::testing::UnitTest& /*unit_test*/) override {
        watcher_ = std::thread([this] { watch(); });
    }

    void OnTestStart(const ::testing::TestInfo& info) override {
        const std::lock_guard<std::mutex> lock(mutex_);
        test_ = std::string(info.test_suite_name()) + "." + info.name();
        deadline_ = std::chrono::steady_clock::now() + g_test_timeout;
        running_ = true;
        changed_.notify_one();
    }

    void OnTestEnd(const ::testing::TestInfo& /*info*/) override {
        const std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
        changed_.notify_one();
    }

    void OnTestProgramEnd(const ::testing::UnitTest& /*unit_test*/) override {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
            changed_.notify_one();
        }
        if (watcher_.joinable()) {
            watcher_.join();
        }
    }

   private:
    void watch() {
        std::unique_lock<std::mutex> lock(mutex_);
        while (!stopping_) {
            if (!running_) {
                changed_.wait(lock);
                continue;
            }
            if (changed_.wait_until(lock, deadline_) == std::cv_status::timeout && running_ &&
                std::chrono::steady_clock::now() >= deadline_) {
                std::fprintf(stderr, "\n%s ran longer than %lld seconds\n", test_.c_str(),
                             static_cast<long long>(g_test_timeout.count()));
                std::fflush(stderr);
                std::_Exit(EXIT_FAILURE);
            }
        }
    }

    std::mutex mutex_;
    std::condition_variable changed_;
    std::thread watcher_;
    std::string test_;
    std::chrono::steady_clock::time_point deadline_;
    bool running_ = false;
    bool stopping_ = false;
};

// gtest owns and deletes the listener.
[[maybe_unused]] const bool g_test_timeout_registered = [] {
    ::testing::UnitTest::GetInstance()->listeners().Append(new TestTimeout);
    return true;
}();

}  // namespace

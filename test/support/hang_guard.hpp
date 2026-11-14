#pragma once

#include <chrono>

// CMake sets this from MCP_CPP_SDK_TEST_TIMEOUT, the seconds ctest gives each test.
#ifndef MCP_CPP_SDK_TEST_TIMEOUT_SECONDS
#define MCP_CPP_SDK_TEST_TIMEOUT_SECONDS 120
#endif

/// How long a test waits for something that should happen before it reports a hang. It is not a
/// measure of speed: it follows the ctest timeout, which builds that run the tests many times slower
/// raise, and stays below it, so that a hung test reports where it stopped before ctest stops it.
inline constexpr std::chrono::seconds g_hang_guard{MCP_CPP_SDK_TEST_TIMEOUT_SECONDS - 20};

/// How long a gate or a held handler waits for the test to release it. It is longer than any wait in
/// a test, so the test sees what it waits for first, and still bounded, so that a test that fails
/// without releasing does not leave a thread parked for good.
inline constexpr std::chrono::seconds g_release_guard{MCP_CPP_SDK_TEST_TIMEOUT_SECONDS - 10};

static_assert(g_hang_guard.count() > 0, "MCP_CPP_SDK_TEST_TIMEOUT must be more than 20 seconds");

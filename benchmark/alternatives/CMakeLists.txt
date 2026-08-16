cmake_minimum_required(VERSION 3.23)
project(mcp-cpp-sdk-alternative-benchmark LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Match the production load's 50 VUs at the top-level request/handler boundary.
# cpp-httplib workers own keep-alive connections, so a smaller pool would limit
# active connections rather than normalize simultaneous request handling. Define
# both values because newer releases may grow the pool up to MAX_COUNT.
add_compile_definitions(
  CPPHTTPLIB_THREAD_POOL_COUNT=50 CPPHTTPLIB_THREAD_POOL_MAX_COUNT=50
  MCP_BENCHMARK_REQUEST_CONCURRENCY=50)

set(BENCHMARK_SDK
    ""
    CACHE STRING "SDK adapter id")
set(BENCHMARK_SDK_SOURCE
    ""
    CACHE PATH "Path to the SDK source")
if(NOT BENCHMARK_SDK OR NOT EXISTS "${BENCHMARK_SDK_SOURCE}/CMakeLists.txt")
  message(
    FATAL_ERROR "BENCHMARK_SDK and a valid BENCHMARK_SDK_SOURCE are required")
endif()

find_package(CURL REQUIRED)
find_package(Threads REQUIRED)
find_library(HIREDIS_LIBRARY NAMES hiredis REQUIRED)
find_path(HIREDIS_INCLUDE_DIR NAMES hiredis/hiredis.h REQUIRED)

add_library(benchmark-workload STATIC common/benchmark_workload.cpp)
target_include_directories(
  benchmark-workload
  PUBLIC common
  PRIVATE "${HIREDIS_INCLUDE_DIR}")
target_link_libraries(benchmark-workload PUBLIC CURL::libcurl Threads::Threads
                                                "${HIREDIS_LIBRARY}")

if(BENCHMARK_SDK STREQUAL "ours")
  set(BUILD_TESTING
      OFF
      CACHE BOOL "" FORCE)
  set(BUILD_EXAMPLES
      OFF
      CACHE BOOL "" FORCE)
  set(BUILD_DOCS
      OFF
      CACHE BOOL "" FORCE)
  set(MCP_CPP_SDK_BUILD_CONFORMANCE
      OFF
      CACHE BOOL "" FORCE)
  set(MCP_CPP_SDK_BUILD_SHARED
      OFF
      CACHE BOOL "" FORCE)
  set(MCP_CPP_SDK_BUILD_STATIC
      ON
      CACHE BOOL "" FORCE)
  set(MCP_CPP_SDK_DEFAULT_LINKAGE
      static
      CACHE STRING "" FORCE)
  add_subdirectory("${BENCHMARK_SDK_SOURCE}" sdk)
  add_executable(alternative-benchmark-server adapters/ours.cpp)
  target_link_libraries(alternative-benchmark-server PRIVATE mcp-cpp-sdk
                                                             benchmark-workload)
elseif(BENCHMARK_SDK STREQUAL "hkr04")
  set(MCP_BUILD_TESTS
      OFF
      CACHE BOOL "" FORCE)
  set(MCP_MAX_SESSIONS
      0
      CACHE STRING "" FORCE)
  set(MCP_SESSION_TIMEOUT
      0
      CACHE STRING "" FORCE)
  add_subdirectory("${BENCHMARK_SDK_SOURCE}" sdk)
  add_executable(alternative-benchmark-server adapters/hkr04.cpp)
  target_include_directories(
    alternative-benchmark-server PRIVATE "${BENCHMARK_SDK_SOURCE}/include"
                                         "${BENCHMARK_SDK_SOURCE}/common")
  target_link_libraries(alternative-benchmark-server PRIVATE mcp
                                                             benchmark-workload)
elseif(BENCHMARK_SDK STREQUAL "fastmcpp")
  set(FASTMCPP_BUILD_TESTS
      OFF
      CACHE BOOL "" FORCE)
  set(FASTMCPP_BUILD_EXAMPLES
      OFF
      CACHE BOOL "" FORCE)
  set(FASTMCPP_ENABLE_POST_STREAMING
      OFF
      CACHE BOOL "" FORCE)
  set(FASTMCPP_ENABLE_SAMPLING_HTTP_HANDLERS
      OFF
      CACHE BOOL "" FORCE)
  add_subdirectory("${BENCHMARK_SDK_SOURCE}" sdk)
  add_executable(alternative-benchmark-server adapters/fastmcpp.cpp)
  target_link_libraries(alternative-benchmark-server PRIVATE fastmcpp_core
                                                             benchmark-workload)
elseif(BENCHMARK_SDK STREQUAL "neumann")
  set(MCP_BUILD_TESTS
      OFF
      CACHE BOOL "" FORCE)
  set(MCP_BUILD_EXAMPLES
      OFF
      CACHE BOOL "" FORCE)
  set(MCP_WARNINGS_AS_ERRORS
      OFF
      CACHE BOOL "" FORCE)
  set(MCP_ENABLE_HTTP
      ON
      CACHE BOOL "" FORCE)
  set(MCP_HTTP_NO_TLS
      ON
      CACHE BOOL "" FORCE)
  add_subdirectory("${BENCHMARK_SDK_SOURCE}" sdk)
  add_executable(alternative-benchmark-server adapters/neumann.cpp)
  target_link_libraries(alternative-benchmark-server PRIVATE mcp::http
                                                             benchmark-workload)
elseif(BENCHMARK_SDK STREQUAL "cxxmcp")
  set(BUILD_TESTING
      OFF
      CACHE BOOL "" FORCE)
  set(CXXMCP_BUILD_SDK
      ON
      CACHE BOOL "" FORCE)
  set(CXXMCP_BUILD_EXAMPLES
      OFF
      CACHE BOOL "" FORCE)
  set(CXXMCP_BUILD_TESTS
      OFF
      CACHE BOOL "" FORCE)
  set(CXXMCP_BUILD_BENCHMARKS
      OFF
      CACHE BOOL "" FORCE)
  set(CXXMCP_BUILD_DOCS
      OFF
      CACHE BOOL "" FORCE)
  set(CXXMCP_ENABLE_HTTP
      ON
      CACHE BOOL "" FORCE)
  set(CXXMCP_ENABLE_OPENSSL
      OFF
      CACHE BOOL "" FORCE)
  set(CXXMCP_ENABLE_AUTH
      OFF
      CACHE BOOL "" FORCE)
  set(CXXMCP_ENABLE_WEBSOCKET
      OFF
      CACHE BOOL "" FORCE)
  add_subdirectory("${BENCHMARK_SDK_SOURCE}" sdk)
  add_executable(alternative-benchmark-server adapters/cxxmcp.cpp)
  target_link_libraries(alternative-benchmark-server PRIVATE cxxmcp::sdk
                                                             benchmark-workload)
else()
  message(FATAL_ERROR "No benchmark adapter for '${BENCHMARK_SDK}'")
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(alternative-benchmark-server PRIVATE -O3)
endif()

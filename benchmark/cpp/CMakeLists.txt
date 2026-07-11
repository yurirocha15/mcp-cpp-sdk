cmake_minimum_required(VERSION 3.20)
project(mcp-benchmark-server CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(-fdiagnostics-color=always)
endif()

set(MCP_ROOT_DIR "${CMAKE_SOURCE_DIR}")
if(NOT EXISTS "${MCP_ROOT_DIR}/include/mcp"
   OR NOT EXISTS "${MCP_ROOT_DIR}/src/version.cpp")
  set(MCP_ROOT_DIR "${CMAKE_SOURCE_DIR}/../..")
endif()

set(BUILD_TESTING
    OFF
    CACHE BOOL "Disable root tests for benchmark build" FORCE)
set(BUILD_EXAMPLES
    OFF
    CACHE BOOL "Disable root examples for benchmark build" FORCE)
add_subdirectory("${MCP_ROOT_DIR}" "${CMAKE_BINARY_DIR}/mcp-cpp-sdk-root")

add_executable(benchmark-server benchmark_server.cpp)
target_link_libraries(benchmark-server PRIVATE mcp-cpp-sdk)

if(CMAKE_BUILD_TYPE MATCHES Release)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(benchmark-server PRIVATE -O3 -march=native)
  elseif(MSVC)
    target_compile_options(benchmark-server PRIVATE /O2)
  endif()
endif()

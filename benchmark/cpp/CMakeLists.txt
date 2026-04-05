cmake_minimum_required(VERSION 3.20)
project(mcp-benchmark-server CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(-fdiagnostics-color=always)
endif()

find_package(Boost REQUIRED COMPONENTS headers system)
find_package(nlohmann_json REQUIRED)
find_package(OpenSSL REQUIRED)

set(MCP_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/include")
if(NOT EXISTS "${MCP_INCLUDE_DIR}/mcp")
  set(MCP_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/../../include")
endif()

add_executable(benchmark-server benchmark_server.cpp)
target_link_libraries(
  benchmark-server PRIVATE Boost::headers Boost::system
                           nlohmann_json::nlohmann_json OpenSSL::Crypto)
target_include_directories(benchmark-server PRIVATE ${MCP_INCLUDE_DIR})

if(CMAKE_BUILD_TYPE MATCHES Release)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(benchmark-server PRIVATE -O3 -march=native)
  elseif(MSVC)
    target_compile_options(benchmark-server PRIVATE /O2)
  endif()
endif()

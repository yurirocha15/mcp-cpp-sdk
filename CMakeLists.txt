cmake_minimum_required(VERSION 3.20)
project(mcp-cpp-sdk CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(-fdiagnostics-color=always)
endif()

find_package(Boost REQUIRED COMPONENTS headers system)
find_package(nlohmann_json REQUIRED)
find_package(GTest REQUIRED)

add_library(mcp-cpp-sdk INTERFACE)
target_link_libraries(mcp-cpp-sdk INTERFACE Boost::headers Boost::system
                                            nlohmann_json::nlohmann_json)
target_include_directories(mcp-cpp-sdk INTERFACE include)

# Documentation target
find_package(Doxygen QUIET)
if(DOXYGEN_FOUND)
  set(DOXYGEN_OUTPUT_DIR "${CMAKE_BINARY_DIR}/doxygen")
  configure_file(${CMAKE_SOURCE_DIR}/docs/Doxyfile.in
                 ${CMAKE_BINARY_DIR}/Doxyfile @ONLY)
  add_custom_target(
    docs
    COMMAND ${DOXYGEN_EXECUTABLE} ${CMAKE_BINARY_DIR}/Doxyfile
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Generating Doxygen XML"
    VERBATIM)
endif()

# Build Tests
enable_testing()
set(TEST_SRCS
    test/client_core_test.cpp
    test/client_features_test.cpp
    test/concepts_test.cpp
    test/core_test.cpp
    test/protocol_test.cpp
    test/sampling_test.cpp
    test/server_core_test.cpp
    test/server_handlers_test.cpp
    test/transport_test.cpp
    test/transport_stdio_test.cpp
    test/transport_ws_test.cpp)
add_executable(mcp-sdk-tests ${TEST_SRCS})
target_link_libraries(mcp-sdk-tests PRIVATE mcp-cpp-sdk GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(mcp-sdk-tests)

option(ENABLE_COVERAGE "Enable coverage reporting" OFF)
if(ENABLE_COVERAGE)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(mcp-sdk-tests PRIVATE --coverage -O0 -g)
    target_link_options(mcp-sdk-tests PRIVATE --coverage)
  endif()
endif()

# Examples
option(BUILD_EXAMPLES "Build example programs" ON)
if(BUILD_EXAMPLES)
  add_executable(example-server-stdio examples/server_stdio.cpp)
  target_link_libraries(example-server-stdio PRIVATE mcp-cpp-sdk)

  add_executable(example-client-stdio examples/client_stdio.cpp)
  target_link_libraries(example-client-stdio PRIVATE mcp-cpp-sdk)

  add_executable(example-server-sampling examples/server_with_sampling.cpp)
  target_link_libraries(example-server-sampling PRIVATE mcp-cpp-sdk)

  add_executable(example-echo-websocket examples/echo_websocket.cpp)
  target_link_libraries(example-echo-websocket PRIVATE mcp-cpp-sdk)
endif()

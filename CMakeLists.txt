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
find_package(OpenSSL REQUIRED)
add_library(mcp-cpp-sdk STATIC)
target_link_libraries(
  mcp-cpp-sdk PUBLIC Boost::headers Boost::system nlohmann_json::nlohmann_json
                     OpenSSL::Crypto)
target_include_directories(mcp-cpp-sdk PUBLIC include)
target_sources(
  mcp-cpp-sdk
  PRIVATE src/version.cpp
          src/runtime.cpp
          src/stdio_transport.cpp
          src/server_stdio.cpp
          src/server_http.cpp
          src/server_tool.cpp
          src/server.cpp
          src/http_server_transport.cpp
          src/http_client_transport.cpp
          src/transport_factories.cpp)

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
option(BUILD_TESTING "Build tests" ON)
if(BUILD_TESTING)
  find_package(GTest REQUIRED)
  enable_testing()
  set(TEST_SRCS
      test/auth_integration_test.cpp
      test/auth_oauth_test.cpp
      test/client_core_test.cpp
      test/client_features_test.cpp
      test/client_notifications_test.cpp
      test/concepts_test.cpp
      test/core_test.cpp
      test/protocol_test.cpp
      test/roots_test.cpp
      test/sampling_test.cpp
      test/server_core_test.cpp
      test/server_cancellation_test.cpp
      test/server_elicitation_test.cpp
      test/server_handlers_test.cpp
      test/server_logging_test.cpp
      test/server_middleware_test.cpp
      test/server_progress_test.cpp
      test/server_subscriptions_test.cpp
      test/transport_test.cpp
      test/transport_stdio_test.cpp
      test/transport_ws_test.cpp
      test/transport_http_test.cpp
      test/transport_http_resumability_test.cpp
      test/transport_http_session_manager_test.cpp
      test/transport_memory_test.cpp
      test/runtime_test.cpp
      test/server_stdio_test.cpp
      test/server_http_test.cpp
      test/server_tool_test.cpp
      test/transport_factory_test.cpp)
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

  add_executable(example-http-loopback examples/http_loopback.cpp)
  target_link_libraries(example-http-loopback PRIVATE mcp-cpp-sdk)

  add_executable(example-llama-mcp examples/llama/llama_server.cpp)
  target_link_libraries(example-llama-mcp PRIVATE mcp-cpp-sdk)
endif()

option(BUILD_LLDB_EXAMPLE
       "Build LLDB debugger MCP server example (requires liblldb)" OFF)
if(BUILD_LLDB_EXAMPLE)
  find_package(LLDB CONFIG QUIET)
  if(NOT LLDB_FOUND)
    find_library(LLDB_LIB lldb)
    find_path(LLDB_INCLUDE_DIR lldb/API/LLDB.h)
    if(NOT LLDB_LIB OR NOT LLDB_INCLUDE_DIR)
      message(
        FATAL_ERROR
          "LLDB development libraries not found. Install liblldb-dev/llvm.")
    endif()
  endif()
  add_executable(example-debugger-server examples/debugger/debugger_server.cpp)
  if(LLDB_FOUND)
    target_link_libraries(example-debugger-server PRIVATE mcp-cpp-sdk lldb)
  else()
    target_include_directories(example-debugger-server
                               PRIVATE ${LLDB_INCLUDE_DIR}/..)
    target_link_libraries(example-debugger-server PRIVATE mcp-cpp-sdk
                                                          ${LLDB_LIB})
  endif()
endif()

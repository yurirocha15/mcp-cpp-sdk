cmake_minimum_required(VERSION 3.20)
project(mcp-cpp-sdk-test-package LANGUAGES CXX)

set(MCP_CPP_SDK_TEST_COMPONENT
    ""
    CACHE STRING "Optional mcp-cpp-sdk component to exercise")
set(MCP_CPP_SDK_EXPECTED_VERSION
    ""
    CACHE STRING "The mcp-cpp-sdk version under test, found exactly")
if(NOT MCP_CPP_SDK_EXPECTED_VERSION)
  message(
    FATAL_ERROR "Set MCP_CPP_SDK_EXPECTED_VERSION to the version under test")
endif()
if(MCP_CPP_SDK_TEST_COMPONENT)
  find_package(mcp-cpp-sdk "${MCP_CPP_SDK_EXPECTED_VERSION}" EXACT CONFIG
               REQUIRED COMPONENTS "${MCP_CPP_SDK_TEST_COMPONENT}")
else()
  find_package(mcp-cpp-sdk "${MCP_CPP_SDK_EXPECTED_VERSION}" EXACT CONFIG
               REQUIRED)
endif()

add_executable(mcp-cpp-sdk-test-package test_package.cpp)
target_compile_features(mcp-cpp-sdk-test-package PRIVATE cxx_std_20)
target_link_libraries(mcp-cpp-sdk-test-package PRIVATE mcp::sdk)

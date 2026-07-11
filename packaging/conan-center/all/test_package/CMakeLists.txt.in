cmake_minimum_required(VERSION 3.20)
project(mcp_cpp_sdk_test_package LANGUAGES CXX)

find_package(mcp-cpp-sdk CONFIG REQUIRED)
add_executable(mcp-cpp-sdk-test-package test_package.cpp)
target_compile_features(mcp-cpp-sdk-test-package PRIVATE cxx_std_20)
target_link_libraries(mcp-cpp-sdk-test-package PRIVATE mcp::sdk)

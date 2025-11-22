cmake_minimum_required(VERSION 3.20)
project(mcp-cpp-sdk CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Boost REQUIRED COMPONENTS headers system)
find_package(nlohmann_json REQUIRED)
find_package(GTest REQUIRED)

add_library(mcp-cpp-sdk INTERFACE)
target_link_libraries(mcp-cpp-sdk INTERFACE
    Boost::headers
    Boost::system
    nlohmann_json::nlohmann_json
)
target_include_directories(mcp-cpp-sdk INTERFACE include)

enable_testing()

add_executable(mcp-sdk-tests test/main.cpp test/core_test.cpp)
target_link_libraries(mcp-sdk-tests PRIVATE
    mcp-cpp-sdk
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(mcp-sdk-tests)

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
target_link_libraries(mcp-cpp-sdk INTERFACE
    Boost::headers
    Boost::system
    nlohmann_json::nlohmann_json
)
target_include_directories(mcp-cpp-sdk INTERFACE include)


# Build Tests
enable_testing()
set(TEST_SRCS
    test/concepts_test.cpp
    test/core_test.cpp
    test/protocol_test.cpp
    test/transport_test.cpp
    test/transport_stdio_test.cpp
)
add_executable(mcp-sdk-tests ${TEST_SRCS})
target_link_libraries(mcp-sdk-tests PRIVATE
    mcp-cpp-sdk
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(mcp-sdk-tests)

option(ENABLE_COVERAGE "Enable coverage reporting" OFF)
if(ENABLE_COVERAGE)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(mcp-sdk-tests PRIVATE --coverage -O0 -g)
        target_link_options(mcp-sdk-tests PRIVATE --coverage)
    endif()
endif()

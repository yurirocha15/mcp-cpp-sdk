cmake_minimum_required(VERSION 3.20)

if(POLICY CMP0167)
  cmake_policy(SET CMP0167 NEW)
endif()

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" MCP_CPP_SDK_VERSION_FULL
     LIMIT_COUNT 1)
string(STRIP "${MCP_CPP_SDK_VERSION_FULL}" MCP_CPP_SDK_VERSION_FULL)
if(NOT MCP_CPP_SDK_VERSION_FULL MATCHES
   "^([0-9]+)\.([0-9]+)\.([0-9]+)(-rc\.([1-9][0-9]*))?$")
  message(
    FATAL_ERROR
      "VERSION must contain MAJOR.MINOR.PATCH or MAJOR.MINOR.PATCH-rc.NUMBER")
endif()

set(MCP_CPP_SDK_VERSION_MAJOR "${CMAKE_MATCH_1}")
set(MCP_CPP_SDK_VERSION_MINOR "${CMAKE_MATCH_2}")
set(MCP_CPP_SDK_VERSION_PATCH "${CMAKE_MATCH_3}")
set(MCP_CPP_SDK_VERSION_PRERELEASE "${CMAKE_MATCH_4}")
foreach(version_part IN
        ITEMS MCP_CPP_SDK_VERSION_MAJOR MCP_CPP_SDK_VERSION_MINOR
              MCP_CPP_SDK_VERSION_PATCH)
  if(${version_part} MATCHES "^0[0-9]+$")
    message(FATAL_ERROR "VERSION numeric fields must not contain leading zeros")
  endif()
endforeach()

string(CONCAT MCP_CPP_SDK_VERSION_CORE "${MCP_CPP_SDK_VERSION_MAJOR}."
              "${MCP_CPP_SDK_VERSION_MINOR}." "${MCP_CPP_SDK_VERSION_PATCH}")
project(
  mcp-cpp-sdk
  VERSION "${MCP_CPP_SDK_VERSION_CORE}"
  LANGUAGES CXX)

include(CMakePackageConfigHelpers)
include(GNUInstallDirs)
include(cmake/McpCppSdkPkgConfig.cmake)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(-fdiagnostics-color=always)
endif()

find_package(Boost 1.74 REQUIRED)
find_package(nlohmann_json 3.10.5 CONFIG REQUIRED)
find_package(OpenSSL 3.0 REQUIRED COMPONENTS Crypto)

option(MCP_CPP_SDK_BUILD_SHARED "Build the shared SDK library" ON)
option(MCP_CPP_SDK_BUILD_STATIC "Build the static SDK library" ON)
option(MCP_CPP_SDK_STATIC_PIC
       "Build the static SDK with position-independent code" ON)
set(MCP_CPP_SDK_DEFAULT_LINKAGE
    "shared"
    CACHE STRING "Library selected by the mcp::sdk compatibility target")
set_property(CACHE MCP_CPP_SDK_DEFAULT_LINKAGE PROPERTY STRINGS shared static)

if(NOT MCP_CPP_SDK_BUILD_SHARED AND NOT MCP_CPP_SDK_BUILD_STATIC)
  message(FATAL_ERROR "At least one SDK library variant must be enabled")
endif()
if(NOT MCP_CPP_SDK_DEFAULT_LINKAGE MATCHES "^(shared|static)$")
  message(FATAL_ERROR "MCP_CPP_SDK_DEFAULT_LINKAGE must be shared or static")
endif()
if(MCP_CPP_SDK_DEFAULT_LINKAGE STREQUAL "shared" AND NOT
                                                     MCP_CPP_SDK_BUILD_SHARED)
  set(MCP_CPP_SDK_DEFAULT_LINKAGE "static")
elseif(MCP_CPP_SDK_DEFAULT_LINKAGE STREQUAL "static"
       AND NOT MCP_CPP_SDK_BUILD_STATIC)
  set(MCP_CPP_SDK_DEFAULT_LINKAGE "shared")
endif()

if(MCP_CPP_SDK_VERSION_MAJOR EQUAL 0)
  set(MCP_CPP_SDK_SOVERSION
      "${MCP_CPP_SDK_VERSION_MAJOR}.${MCP_CPP_SDK_VERSION_MINOR}")
else()
  set(MCP_CPP_SDK_SOVERSION "${MCP_CPP_SDK_VERSION_MAJOR}")
endif()

set(MCP_CPP_SDK_GENERATED_INCLUDE_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/generated/include")
configure_file(
  cmake/version.hpp.in
  "${MCP_CPP_SDK_GENERATED_INCLUDE_DIR}/mcp/core/version.hpp" @ONLY)

set(MCP_CPP_SDK_SOURCES
    src/core/version.cpp
    src/core/runtime.cpp
    src/core/secure_random.cpp
    src/core/serialized_transport_writer.cpp
    src/protocol/tools.cpp
    src/auth/oauth.cpp
    src/client/client.cpp
    src/server/server_stdio.cpp
    src/server/server_http.cpp
    src/server/server_tool.cpp
    src/server/server.cpp
    src/transport/stdio.cpp
    src/transport/memory.cpp
    src/transport/http_types.cpp
    src/transport/http_server.cpp
    src/transport/http_client.cpp
    src/transport/http_session_manager.cpp
    src/transport/websocket.cpp
    src/transport/transport_factories.cpp)

# Configure one installed SDK linkage variant.
function(mcp_cpp_sdk_configure_library target)
  target_sources(${target} PRIVATE ${MCP_CPP_SDK_SOURCES})
  target_compile_features(${target} PUBLIC cxx_std_20)
  target_include_directories(
    ${target}
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
           $<BUILD_INTERFACE:${MCP_CPP_SDK_GENERATED_INCLUDE_DIR}>
           $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
  target_link_libraries(
    ${target} PUBLIC Boost::headers nlohmann_json::nlohmann_json
                     OpenSSL::Crypto)
  if(WIN32)
    target_compile_definitions(${target} PUBLIC _WIN32_WINNT=0x0A00)
  endif()
endfunction()

set(MCP_CPP_SDK_INSTALL_TARGETS)
if(MCP_CPP_SDK_BUILD_SHARED)
  add_library(mcp-cpp-sdk-shared SHARED)
  mcp_cpp_sdk_configure_library(mcp-cpp-sdk-shared)
  target_compile_definitions(
    mcp-cpp-sdk-shared
    PRIVATE MCP_BUILD_DLL
    INTERFACE MCP_DLL)
  set_target_properties(
    mcp-cpp-sdk-shared
    PROPERTIES EXPORT_NAME sdk_shared
               OUTPUT_NAME mcp-cpp-sdk
               VERSION "${PROJECT_VERSION}"
               SOVERSION "${MCP_CPP_SDK_SOVERSION}"
               CXX_VISIBILITY_PRESET hidden
               VISIBILITY_INLINES_HIDDEN YES)
  if(WIN32)
    set_property(
      TARGET mcp-cpp-sdk-shared PROPERTY RUNTIME_OUTPUT_NAME
                                         "mcp-cpp-sdk-${MCP_CPP_SDK_SOVERSION}")
  endif()
  add_library(mcp::sdk_shared ALIAS mcp-cpp-sdk-shared)
  list(APPEND MCP_CPP_SDK_INSTALL_TARGETS mcp-cpp-sdk-shared)
endif()

if(MCP_CPP_SDK_BUILD_STATIC)
  add_library(mcp-cpp-sdk-static STATIC)
  mcp_cpp_sdk_configure_library(mcp-cpp-sdk-static)
  target_compile_definitions(mcp-cpp-sdk-static PUBLIC MCP_STATIC)
  set_target_properties(
    mcp-cpp-sdk-static
    PROPERTIES EXPORT_NAME sdk_static OUTPUT_NAME mcp-cpp-sdk-static
               POSITION_INDEPENDENT_CODE "${MCP_CPP_SDK_STATIC_PIC}")
  add_library(mcp::sdk_static ALIAS mcp-cpp-sdk-static)
  list(APPEND MCP_CPP_SDK_INSTALL_TARGETS mcp-cpp-sdk-static)
endif()

add_library(mcp-cpp-sdk INTERFACE)
target_link_libraries(mcp-cpp-sdk
                      INTERFACE "mcp-cpp-sdk-${MCP_CPP_SDK_DEFAULT_LINKAGE}")
add_library(mcp::sdk ALIAS mcp-cpp-sdk)

# Documentation target
option(BUILD_DOCS "Add the Doxygen documentation target" ON)
if(BUILD_DOCS)
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
endif()

# Build Tests
option(BUILD_TESTING "Build tests" ON)
if(BUILD_TESTING)
  find_package(GTest REQUIRED)
  enable_testing()
  set(MCP_CPP_SDK_CMAKE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
  set(MCP_CPP_SDK_CONFIG_VERSION_TEMPLATE
      "${MCP_CPP_SDK_CMAKE_DIR}/mcp-cpp-sdk-config-version.cmake.in")
  set(MCP_CPP_SDK_CONFIG_VERSION_PRERELEASE_TEST
      "${MCP_CPP_SDK_CMAKE_DIR}/tests/verify-config-version-prerelease.cmake")
  string(CONCAT MCP_CPP_SDK_CONFIG_VERSION_TEMPLATE_ARGUMENT
                "-DMCP_CPP_SDK_CONFIG_VERSION_TEMPLATE="
                "${MCP_CPP_SDK_CONFIG_VERSION_TEMPLATE}")
  add_test(
    NAME packaging-pkgconfig-prefix
    COMMAND
      ${CMAKE_COMMAND} -P
      "${CMAKE_CURRENT_SOURCE_DIR}/cmake/tests/verify-pkgconfig-prefix.cmake")
  add_test(
    NAME packaging-config-version-prerelease
    COMMAND ${CMAKE_COMMAND} "${MCP_CPP_SDK_CONFIG_VERSION_TEMPLATE_ARGUMENT}"
            -P "${MCP_CPP_SDK_CONFIG_VERSION_PRERELEASE_TEST}")
  find_program(MCP_CPP_SDK_PKG_CONFIG_EXECUTABLE pkg-config)
  if(MCP_CPP_SDK_BUILD_STATIC
     AND MCP_CPP_SDK_PKG_CONFIG_EXECUTABLE
     AND CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux"
     AND NOT CMAKE_CROSSCOMPILING)
    set(MCP_CPP_SDK_PC_TEST_SOURCE
        "${CMAKE_CURRENT_SOURCE_DIR}/test_package/test_package.cpp")
    string(
      CONCAT MCP_CPP_SDK_PC_BOOST_INCLUDES "$<JOIN:"
             "$<TARGET_PROPERTY:Boost::headers,INTERFACE_INCLUDE_DIRECTORIES>"
             ",|>")
    string(CONCAT MCP_CPP_SDK_PC_JSON_INCLUDES "$<JOIN:"
                  "$<TARGET_PROPERTY:nlohmann_json::nlohmann_json,"
                  "INTERFACE_INCLUDE_DIRECTORIES>" ",|>")
    string(CONCAT MCP_CPP_SDK_PC_TEST_LINK_OPTIONS "$<JOIN:"
                  "$<TARGET_PROPERTY:mcp-cpp-sdk-static,"
                  "INTERFACE_LINK_OPTIONS>" ",|>")
    set(MCP_CPP_SDK_PC_TEST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/cmake/tests")
    set(MCP_CPP_SDK_PC_TEST_SCRIPT
        "${MCP_CPP_SDK_PC_TEST_DIR}/verify-pkgconfig-static-consumer.cmake")
    add_test(
      NAME packaging-pkgconfig-static-consumer
      COMMAND
        ${CMAKE_COMMAND} "-DMCP_CPP_SDK_BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}"
        "-DMCP_CPP_SDK_TEST_SOURCE=${MCP_CPP_SDK_PC_TEST_SOURCE}"
        "-DMCP_CPP_SDK_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
        "-DMCP_CPP_SDK_INSTALL_LIBDIR=${CMAKE_INSTALL_LIBDIR}"
        "-DMCP_CPP_SDK_BOOST_INCLUDE_DIRS=${MCP_CPP_SDK_PC_BOOST_INCLUDES}"
        "-DMCP_CPP_SDK_NLOHMANN_INCLUDE_DIRS=${MCP_CPP_SDK_PC_JSON_INCLUDES}"
        "-DMCP_CPP_SDK_TEST_LINK_OPTIONS=${MCP_CPP_SDK_PC_TEST_LINK_OPTIONS}"
        -P "${MCP_CPP_SDK_PC_TEST_SCRIPT}")
  endif()
  set(TEST_SRCS
      test/auth/auth_integration_test.cpp
      test/auth/auth_oauth_test.cpp
      test/client/client_core_test.cpp
      test/client/client_features_test.cpp
      test/client/client_notifications_test.cpp
      test/core/concepts_test.cpp
      test/core/core_test.cpp
      test/core/protocol_test.cpp
      test/core/serialized_transport_writer_test.cpp
      test/server/roots_test.cpp
      test/server/sampling_test.cpp
      test/server/server_core_test.cpp
      test/server/server_cancellation_test.cpp
      test/server/server_elicitation_test.cpp
      test/server/server_handlers_test.cpp
      test/server/server_logging_test.cpp
      test/server/server_middleware_test.cpp
      test/server/server_progress_test.cpp
      test/server/server_subscriptions_test.cpp
      test/transport/transport_test.cpp
      test/transport/transport_stdio_test.cpp
      test/transport/transport_ws_test.cpp
      test/transport/transport_http_test.cpp
      test/transport/transport_http_resumability_test.cpp
      test/transport/transport_http_session_manager_test.cpp
      test/transport/transport_memory_test.cpp
      test/e2e/e2e_integration_test.cpp
      test/e2e/e2e_features_test.cpp
      test/server/runtime_test.cpp
      test/server/server_stdio_test.cpp
      test/server/server_http_test.cpp
      test/server/server_tool_test.cpp
      test/transport/transport_factory_test.cpp
      test/core/gcc11_sso_crash_test.cpp)
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

  option(ENABLE_SANITIZERS
         "Enable AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
  if(ENABLE_SANITIZERS)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
      set(SANITIZER_FLAGS -fsanitize=address,undefined -fno-omit-frame-pointer
                          -g)
      foreach(sdk_target IN LISTS MCP_CPP_SDK_INSTALL_TARGETS)
        target_compile_options(${sdk_target} PUBLIC ${SANITIZER_FLAGS})
        target_link_options(${sdk_target} PUBLIC -fsanitize=address,undefined)
      endforeach()
      target_compile_options(mcp-sdk-tests PRIVATE ${SANITIZER_FLAGS})
      target_link_options(mcp-sdk-tests PRIVATE -fsanitize=address,undefined)
    else()
      message(WARNING "ENABLE_SANITIZERS is only supported with GCC or Clang")
    endif()
  endif()
endif()

set(MCP_CPP_SDK_INSTALL_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake/mcp-cpp-sdk")
if(MCP_CPP_SDK_BUILD_SHARED)
  install(
    TARGETS mcp-cpp-sdk-shared
    EXPORT mcp-cpp-sdk-shared-targets
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT Runtime
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
            COMPONENT Runtime
            NAMELINK_COMPONENT Development
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT Development
    INCLUDES
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
  install(
    EXPORT mcp-cpp-sdk-shared-targets
    FILE mcp-cpp-sdk-shared-targets.cmake
    NAMESPACE mcp::
    DESTINATION "${MCP_CPP_SDK_INSTALL_CMAKEDIR}"
    COMPONENT Development)
endif()
if(MCP_CPP_SDK_BUILD_STATIC)
  install(
    TARGETS mcp-cpp-sdk-static
    EXPORT mcp-cpp-sdk-static-targets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT StaticDevelopment
    INCLUDES
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
  install(
    EXPORT mcp-cpp-sdk-static-targets
    FILE mcp-cpp-sdk-static-targets.cmake
    NAMESPACE mcp::
    DESTINATION "${MCP_CPP_SDK_INSTALL_CMAKEDIR}"
    COMPONENT StaticDevelopment)
endif()
install(
  DIRECTORY include/mcp
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
  COMPONENT Development)
install(
  FILES "${MCP_CPP_SDK_GENERATED_INCLUDE_DIR}/mcp/core/version.hpp"
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/mcp/core"
  COMPONENT Development)
install(
  FILES LICENSE
  DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/mcp-cpp-sdk"
  COMPONENT Runtime)

configure_package_config_file(
  cmake/mcp-cpp-sdk-config.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/mcp-cpp-sdk-config.cmake"
  INSTALL_DESTINATION "${MCP_CPP_SDK_INSTALL_CMAKEDIR}")
configure_file(
  cmake/mcp-cpp-sdk-config-version.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/mcp-cpp-sdk-config-version.cmake" @ONLY)
install(
  FILES "${CMAKE_CURRENT_BINARY_DIR}/mcp-cpp-sdk-config.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/mcp-cpp-sdk-config-version.cmake"
  DESTINATION "${MCP_CPP_SDK_INSTALL_CMAKEDIR}"
  COMPONENT Development)

set(MCP_CPP_SDK_PC_REQUIRES "nlohmann_json libcrypto")
set(MCP_CPP_SDK_PC_REQUIRES_PRIVATE "")
set(MCP_CPP_SDK_PC_LIBS_PRIVATE "")
mcp_cpp_sdk_pkgconfig_prefix_from_libdir(MCP_CPP_SDK_PC_PREFIX_FROM_PCFILE
                                         "${CMAKE_INSTALL_LIBDIR}")
if(MCP_CPP_SDK_BUILD_SHARED)
  set(MCP_CPP_SDK_PC_LIBRARY "mcp-cpp-sdk")
  set(MCP_CPP_SDK_PC_DESCRIPTION "MCP C++ SDK (shared library)")
  set(MCP_CPP_SDK_PC_CFLAGS "-DMCP_DLL")
  configure_file(cmake/mcp-cpp-sdk.pc.in
                 "${CMAKE_CURRENT_BINARY_DIR}/mcp-cpp-sdk.pc" @ONLY)
  configure_file(cmake/mcp-cpp-sdk.pc.in
                 "${CMAKE_CURRENT_BINARY_DIR}/mcp-cpp-sdk-shared.pc" @ONLY)
  install(
    FILES "${CMAKE_CURRENT_BINARY_DIR}/mcp-cpp-sdk.pc"
          "${CMAKE_CURRENT_BINARY_DIR}/mcp-cpp-sdk-shared.pc"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig"
    COMPONENT Development)
endif()
if(MCP_CPP_SDK_BUILD_STATIC)
  set(MCP_CPP_SDK_PC_LIBRARY "mcp-cpp-sdk-static")
  set(MCP_CPP_SDK_PC_DESCRIPTION "MCP C++ SDK (static library)")
  set(MCP_CPP_SDK_PC_CFLAGS "-DMCP_STATIC")
  configure_file(cmake/mcp-cpp-sdk.pc.in
                 "${CMAKE_CURRENT_BINARY_DIR}/mcp-cpp-sdk-static.pc" @ONLY)
  install(
    FILES "${CMAKE_CURRENT_BINARY_DIR}/mcp-cpp-sdk-static.pc"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig"
    COMPONENT StaticDevelopment)
endif()

# Examples
option(BUILD_EXAMPLES "Build example programs" ON)
if(BUILD_EXAMPLES)
  add_executable(example-server-stdio examples/servers/stdio/server_stdio.cpp)
  target_link_libraries(example-server-stdio PRIVATE mcp-cpp-sdk)

  add_executable(example-client-stdio examples/clients/stdio/client_stdio.cpp)
  target_link_libraries(example-client-stdio PRIVATE mcp-cpp-sdk)

  add_executable(example-interactive-client
                 examples/clients/stdio/interactive_client.cpp)
  target_link_libraries(example-interactive-client PRIVATE mcp-cpp-sdk)

  add_executable(example-server-sampling
                 examples/advanced/server_with_sampling.cpp)
  target_link_libraries(example-server-sampling PRIVATE mcp-cpp-sdk)

  add_executable(example-echo-websocket examples/advanced/echo_websocket.cpp)
  target_link_libraries(example-echo-websocket PRIVATE mcp-cpp-sdk)

  add_executable(example-http-loopback examples/advanced/http_loopback.cpp)
  target_link_libraries(example-http-loopback PRIVATE mcp-cpp-sdk)

  add_executable(example-server-simple examples/servers/stdio/server_simple.cpp)
  target_link_libraries(example-server-simple PRIVATE mcp-cpp-sdk)

  add_executable(example-llama-mcp examples/advanced/llama/llama_server.cpp)
  target_link_libraries(example-llama-mcp PRIVATE mcp-cpp-sdk)

  # Feature examples
  add_executable(example-feature-progress-cancellation
                 examples/features/progress_cancellation.cpp)
  target_link_libraries(example-feature-progress-cancellation
                        PRIVATE mcp-cpp-sdk)

  add_executable(example-feature-notifications-subscriptions
                 examples/features/notifications_subscriptions.cpp)
  target_link_libraries(example-feature-notifications-subscriptions
                        PRIVATE mcp-cpp-sdk)

  add_executable(example-feature-middleware examples/features/middleware.cpp)
  target_link_libraries(example-feature-middleware PRIVATE mcp-cpp-sdk)

  add_executable(example-feature-completions examples/features/completions.cpp)
  target_link_libraries(example-feature-completions PRIVATE mcp-cpp-sdk)

  add_executable(example-feature-pagination examples/features/pagination.cpp)
  target_link_libraries(example-feature-pagination PRIVATE mcp-cpp-sdk)

  add_executable(example-feature-elicitation examples/features/elicitation.cpp)
  target_link_libraries(example-feature-elicitation PRIVATE mcp-cpp-sdk)

  add_executable(example-feature-roots examples/features/roots.cpp)
  target_link_libraries(example-feature-roots PRIVATE mcp-cpp-sdk)

  add_executable(example-feature-error-handling
                 examples/features/error_handling.cpp)
  target_link_libraries(example-feature-error-handling PRIVATE mcp-cpp-sdk)

  add_executable(example-feature-transport-memory
                 examples/features/transport_memory.cpp)
  target_link_libraries(example-feature-transport-memory PRIVATE mcp-cpp-sdk)

  add_executable(example-feature-graceful-shutdown
                 examples/features/graceful_shutdown.cpp)
  target_link_libraries(example-feature-graceful-shutdown PRIVATE mcp-cpp-sdk)

  add_executable(example-feature-http-server-convenience
                 examples/features/http_server_convenience.cpp)
  target_link_libraries(example-feature-http-server-convenience
                        PRIVATE mcp-cpp-sdk)

  # Benchmark targets
  add_executable(example-benchmark-stdio examples/benchmark/benchmark_stdio.cpp)
  target_link_libraries(example-benchmark-stdio PRIVATE mcp-cpp-sdk)

  add_executable(example-benchmark-http examples/benchmark/benchmark_http.cpp)
  target_link_libraries(example-benchmark-http PRIVATE mcp-cpp-sdk)

  add_executable(example-benchmark-websocket
                 examples/benchmark/benchmark_websocket.cpp)
  target_link_libraries(example-benchmark-websocket PRIVATE mcp-cpp-sdk)

  add_executable(example-feature-oauth-flow examples/features/oauth_flow.cpp)
  target_link_libraries(example-feature-oauth-flow PRIVATE mcp-cpp-sdk)

  # LLDB debugger example (links LLDB when available, uses stubs otherwise)
  add_executable(example-debugger-server
                 examples/advanced/debugger/debugger_server.cpp)
  find_package(LLDB CONFIG QUIET)
  if(NOT LLDB_FOUND)
    find_library(LLDB_LIB NAMES lldb liblldb)
    find_path(LLDB_INCLUDE_DIR lldb/API/LLDB.h)
  endif()
  if(LLDB_FOUND)
    target_link_libraries(example-debugger-server PRIVATE mcp-cpp-sdk lldb)
  elseif(LLDB_LIB AND LLDB_INCLUDE_DIR)
    target_include_directories(example-debugger-server
                               PRIVATE ${LLDB_INCLUDE_DIR}/..)
    target_link_libraries(example-debugger-server PRIVATE mcp-cpp-sdk
                                                          ${LLDB_LIB})
  else()
    target_link_libraries(example-debugger-server PRIVATE mcp-cpp-sdk)
  endif()
endif()

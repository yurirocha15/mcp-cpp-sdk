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

# Tests, examples and docs default ON only when this is the top-level project,
# so a FetchContent or add_subdirectory consumer does not need GTest or Doxygen.
# PROJECT_IS_TOP_LEVEL needs CMake 3.21.
if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
  set(MCP_CPP_SDK_IS_TOP_LEVEL ON)
else()
  set(MCP_CPP_SDK_IS_TOP_LEVEL OFF)
endif()

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
  # SemVer 0.x does not promise binary compatibility between releases.  Give
  # every stable 0.x release its own loader identity so an old consumer can
  # never silently load a different patch-level ABI.
  set(MCP_CPP_SDK_SOVERSION "${MCP_CPP_SDK_VERSION_CORE}")
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
    src/core/context.cpp
    src/core/runtime.cpp
    src/core/regex.cpp
    src/core/secure_random.cpp
    src/core/json_dump.cpp
    src/core/json_parse.cpp
    src/core/serialized_transport_writer.cpp
    src/protocol/native_schema.cpp
    src/protocol/schema.cpp
    src/protocol/base.cpp
    src/protocol/capabilities.cpp
    src/protocol/content.cpp
    src/protocol/completion.cpp
    src/protocol/notification.cpp
    src/protocol/request_meta.cpp
    src/protocol/roots.cpp
    src/protocol/resources.cpp
    src/protocol/prompts.cpp
    src/protocol/tasks.cpp
    src/protocol/elicitation.cpp
    src/protocol/tools.cpp
    src/protocol/sampling.cpp
    src/protocol/skills.cpp
    src/protocol/apps.cpp
    src/auth/challenge.cpp
    src/auth/client_identity.cpp
    src/auth/metadata_policy.cpp
    src/auth/oauth.cpp
    src/auth/grant_flow.cpp
    src/auth/client_authentication.cpp
    src/auth/client_credentials.cpp
    src/auth/enterprise_authorization.cpp
    src/client/client.cpp
    src/client/endpoint.cpp
    src/client/restarting_stdio_transport.cpp
    src/detail/sealed_state.cpp
    src/client/stdio_session.cpp
    src/client/apps.cpp
    src/client/skills.cpp
    src/server/server_stdio.cpp
    src/server/run_control.cpp
    src/server/server_http.cpp
    src/server/server_tool.cpp
    src/server/apps.cpp
    src/server/skills.cpp
    src/server/mrtr.cpp
    src/server/task_run.cpp
    src/server/task_store.cpp
    src/server/server.cpp
    src/transport/stdio.cpp
    src/transport/transport.cpp
    src/transport/stdio_client.cpp
    src/transport/memory.cpp
    src/transport/http_types.cpp
    src/transport/http_server.cpp
    src/transport/http_client.cpp
    src/transport/http_session_manager.cpp
    src/transport/modern_exchange.cpp
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
  # Vendored jsoncons, used only by src/protocol/schema.cpp. PRIVATE keeps it
  # out of the installed and exported interface; SYSTEM keeps its warnings out
  # of the SDK's own.
  target_include_directories(
    ${target} SYSTEM
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/jsoncons/include")
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
  elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    # Keeps Beast's error categories and nlohmann_json's exception types shared
    # with the caller; see the headers.
    set(MCP_CPP_SDK_BEAST_VISIBILITY_HEADER
        "${CMAKE_CURRENT_SOURCE_DIR}/src/detail/beast_error_visibility.hpp")
    set(MCP_CPP_SDK_JSON_VISIBILITY_HEADER
        "${CMAKE_CURRENT_SOURCE_DIR}/src/detail/json_error_visibility.hpp")
    target_compile_options(
      mcp-cpp-sdk-shared
      PRIVATE "SHELL:-include ${MCP_CPP_SDK_BEAST_VISIBILITY_HEADER}"
              "SHELL:-include ${MCP_CPP_SDK_JSON_VISIBILITY_HEADER}")
  endif()
  if(NOT WIN32 AND NOT APPLE)
    set(MCP_CPP_SDK_VENDORED_SYMBOLS_MAP
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/vendored-symbols.map")
    # --exclude-libs keeps the symbols of static archives linked into the
    # library, such as a static OpenSSL or zlib, out of its exports. Exported,
    # they take the place of the same functions for other code in the process: a
    # system libssl loaded by libcurl, for example, then calls this copy's
    # libcrypto functions with objects its own libcrypto built.
    target_link_options(
      mcp-cpp-sdk-shared PRIVATE
      "LINKER:--version-script=${MCP_CPP_SDK_VENDORED_SYMBOLS_MAP}"
      "LINKER:--exclude-libs,ALL")
    set_property(TARGET mcp-cpp-sdk-shared APPEND
                 PROPERTY LINK_DEPENDS "${MCP_CPP_SDK_VENDORED_SYMBOLS_MAP}")
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

option(
  MCP_CPP_SDK_BUILD_CONFORMANCE
  "Build client and server fixtures for the official MCP conformance runner"
  OFF)
if(MCP_CPP_SDK_BUILD_CONFORMANCE)
  add_subdirectory(conformance)
endif()

# Documentation target
option(BUILD_DOCS "Add the Doxygen documentation target"
       ${MCP_CPP_SDK_IS_TOP_LEVEL})
if(BUILD_DOCS)
  find_package(Doxygen QUIET)
  if(DOXYGEN_FOUND)
    set(DOXYGEN_OUTPUT_DIR "${PROJECT_BINARY_DIR}/doxygen")
    configure_file(${PROJECT_SOURCE_DIR}/docs/Doxyfile.in
                   ${PROJECT_BINARY_DIR}/Doxyfile @ONLY)
    add_custom_target(
      docs
      COMMAND ${DOXYGEN_EXECUTABLE} ${PROJECT_BINARY_DIR}/Doxyfile
      WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
      COMMENT "Generating Doxygen XML"
      VERBATIM)
  endif()
endif()

# Build Tests
option(BUILD_TESTING "Build tests" ${MCP_CPP_SDK_IS_TOP_LEVEL})

# ENABLE_SANITIZERS is declared and applied inside the block below, so with
# testing off the flag is accepted and does nothing: the build lands in a
# directory named for sanitizers without being sanitized. Refuse rather than
# hand back a clean run that proved nothing.
if(ENABLE_SANITIZERS AND NOT BUILD_TESTING)
  message(
    FATAL_ERROR
      "ENABLE_SANITIZERS requires BUILD_TESTING=ON; with tests off the "
      "sanitizer flags are never applied and the build is not sanitized")
endif()

if(ENABLE_TSAN AND NOT BUILD_TESTING)
  message(
    FATAL_ERROR
      "ENABLE_TSAN requires BUILD_TESTING=ON; with tests off the "
      "sanitizer flags are never applied and the build is not sanitized")
endif()

if(BUILD_TESTING)
  find_package(GTest REQUIRED)
  enable_testing()
  set(MCP_CPP_SDK_CMAKE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
  set(MCP_CPP_SDK_CONFIG_VERSION_TEMPLATE
      "${MCP_CPP_SDK_CMAKE_DIR}/mcp-cpp-sdk-config-version.cmake.in")
  set(MCP_CPP_SDK_CONFIG_VERSION_PRERELEASE_TEST
      "${MCP_CPP_SDK_CMAKE_DIR}/tests/verify-config-version-prerelease.cmake")
  set(MCP_CPP_SDK_CONFIG_VERSION_STABLE_TEST
      "${MCP_CPP_SDK_CMAKE_DIR}/tests/verify-config-version-stable.cmake")
  string(CONCAT MCP_CPP_SDK_CONFIG_VERSION_TEMPLATE_ARGUMENT
                "-DMCP_CPP_SDK_CONFIG_VERSION_TEMPLATE="
                "${MCP_CPP_SDK_CONFIG_VERSION_TEMPLATE}")
  set(MCP_CPP_SDK_CONFIG_VERSION_TEST_OUTPUT_ROOT
      "${CMAKE_CURRENT_BINARY_DIR}/cmake-config-version-tests")
  set(MCP_CPP_SDK_CONFIG_VERSION_PRERELEASE_OUTPUT
      "${MCP_CPP_SDK_CONFIG_VERSION_TEST_OUTPUT_ROOT}/prerelease")
  set(MCP_CPP_SDK_CONFIG_VERSION_STABLE_OUTPUT
      "${MCP_CPP_SDK_CONFIG_VERSION_TEST_OUTPUT_ROOT}/stable")
  string(CONCAT MCP_CPP_SDK_CONFIG_VERSION_PRERELEASE_OUTPUT_ARGUMENT
                "-DMCP_CPP_SDK_CONFIG_VERSION_TEST_OUTPUT_DIR="
                "${MCP_CPP_SDK_CONFIG_VERSION_PRERELEASE_OUTPUT}")
  string(CONCAT MCP_CPP_SDK_CONFIG_VERSION_STABLE_OUTPUT_ARGUMENT
                "-DMCP_CPP_SDK_CONFIG_VERSION_TEST_OUTPUT_DIR="
                "${MCP_CPP_SDK_CONFIG_VERSION_STABLE_OUTPUT}")
  add_test(
    NAME packaging-pkgconfig-prefix
    COMMAND
      ${CMAKE_COMMAND} -P
      "${CMAKE_CURRENT_SOURCE_DIR}/cmake/tests/verify-pkgconfig-prefix.cmake")
  if(NOT CMAKE_CROSSCOMPILING)
    string(REPLACE ";" "|" MCP_CPP_SDK_SUBPROJECT_PREFIX_PATH
                   "${CMAKE_PREFIX_PATH}")
    set(MCP_CPP_SDK_SUBPROJECT_TEST_DIR
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/tests")
    add_test(
      NAME packaging-subproject-defaults
      COMMAND
        ${CMAKE_COMMAND} "-DMCP_CPP_SDK_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
        "-DMCP_CPP_SDK_TEST_OUTPUT_DIR=${PROJECT_BINARY_DIR}/subproject-test"
        "-DMCP_CPP_SDK_GENERATOR=${CMAKE_GENERATOR}"
        "-DMCP_CPP_SDK_GENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}"
        "-DMCP_CPP_SDK_GENERATOR_TOOLSET=${CMAKE_GENERATOR_TOOLSET}"
        "-DMCP_CPP_SDK_CMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
        "-DMCP_CPP_SDK_CMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
        "-DMCP_CPP_SDK_CMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}"
        "-DMCP_CPP_SDK_CMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}"
        "-DMCP_CPP_SDK_PREFIX_PATH=${MCP_CPP_SDK_SUBPROJECT_PREFIX_PATH}" -P
        "${MCP_CPP_SDK_SUBPROJECT_TEST_DIR}/verify-subproject-defaults.cmake")
  endif()
  add_test(
    NAME packaging-config-version-prerelease
    COMMAND
      ${CMAKE_COMMAND} "${MCP_CPP_SDK_CONFIG_VERSION_TEMPLATE_ARGUMENT}"
      "${MCP_CPP_SDK_CONFIG_VERSION_PRERELEASE_OUTPUT_ARGUMENT}" -P
      "${MCP_CPP_SDK_CONFIG_VERSION_PRERELEASE_TEST}")
  add_test(
    NAME packaging-config-version-stable
    COMMAND
      ${CMAKE_COMMAND} "${MCP_CPP_SDK_CONFIG_VERSION_TEMPLATE_ARGUMENT}"
      "${MCP_CPP_SDK_CONFIG_VERSION_STABLE_OUTPUT_ARGUMENT}" -P
      "${MCP_CPP_SDK_CONFIG_VERSION_STABLE_TEST}")
  if(CMAKE_NM
     AND CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux"
     AND NOT CMAKE_CROSSCOMPILING)
    set(MCP_CPP_SDK_VENDORED_SYMBOLS_ARGUMENTS
        "-DMCP_CPP_SDK_NM=${CMAKE_NM}"
        "-DMCP_CPP_SDK_BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}")
    if(MCP_CPP_SDK_BUILD_STATIC)
      list(APPEND MCP_CPP_SDK_VENDORED_SYMBOLS_ARGUMENTS
           "-DMCP_CPP_SDK_STATIC_LIBRARY=$<TARGET_FILE:mcp-cpp-sdk-static>")
    endif()
    if(MCP_CPP_SDK_BUILD_SHARED)
      list(APPEND MCP_CPP_SDK_VENDORED_SYMBOLS_ARGUMENTS
           "-DMCP_CPP_SDK_SHARED_LIBRARY=$<TARGET_FILE:mcp-cpp-sdk-shared>")
    endif()
    add_test(
      NAME packaging-vendored-jsoncons-private
      COMMAND
        ${CMAKE_COMMAND} ${MCP_CPP_SDK_VENDORED_SYMBOLS_ARGUMENTS} -P
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/tests/verify-vendored-symbols.cmake")
  endif()
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
      test/auth/auth_authorization_test.cpp
      test/auth/auth_challenge_test.cpp
      test/auth/auth_client_credentials_test.cpp
      test/auth/auth_client_identity_test.cpp
      test/auth/auth_enterprise_authorization_test.cpp
      test/auth/auth_integration_test.cpp
      test/auth/auth_oauth_test.cpp
      test/auth/auth_token_endpoint_auth_test.cpp
      test/client/client_core_test.cpp
      test/client/client_endpoint_test.cpp
      test/client/client_listen_test.cpp
      test/client/client_modern_test.cpp
      test/client/client_mrtr_test.cpp
      test/client/client_tasks_test.cpp
      test/client/client_elicitation_test.cpp
      test/client/client_features_test.cpp
      test/client/client_notifications_test.cpp
      test/client/client_sync_test.cpp
      test/core/capabilities_extensions_test.cpp
      test/core/concepts_test.cpp
      test/core/elicitation_schema_test.cpp
      test/core/executor_test.cpp
      test/core/core_test.cpp
      test/core/fifo_test.cpp
      test/core/json_dump_test.cpp
      test/core/json_parse_test.cpp
      test/core/jsonrpc_envelope_test.cpp
      test/core/json_schema_engines_test.cpp
      test/core/json_schema_test.cpp
      test/core/protocol_test.cpp
      test/core/task_class_test.cpp
      test/core/tasks_test.cpp
      test/core/regex_test.cpp
      test/core/sealed_state_test.cpp
      test/core/serialized_transport_writer_test.cpp
      test/core/strand_executor_test.cpp
      test/server/roots_test.cpp
      test/server/sampling_test.cpp
      test/server/server_apps_test.cpp
      test/server/server_core_test.cpp
      test/server/server_extension_test.cpp
      test/server/server_skills_test.cpp
      test/server/server_cancellation_test.cpp
      test/server/server_elicitation_test.cpp
      test/server/server_limits_test.cpp
      test/server/server_handlers_test.cpp
      test/server/server_logging_test.cpp
      test/server/server_middleware_test.cpp
      test/server/server_modern_era_test.cpp
      test/server/server_mrtr_test.cpp
      test/server/server_tasks_test.cpp
      test/server/server_listen_test.cpp
      test/server/server_param_headers_test.cpp
      test/server/server_progress_test.cpp
      test/server/server_subscriptions_test.cpp
      test/server/server_simple_registration_test.cpp
      test/server/server_run_control_test.cpp
      test/transport/transport_stdio_test.cpp
      test/transport/transport_stdio_client_test.cpp
      test/transport/transport_ws_test.cpp
      test/transport/transport_http_test.cpp
      test/transport/transport_http_resumability_test.cpp
      test/transport/transport_http_legacy_stream_test.cpp
      test/transport/transport_http_session_manager_test.cpp
      test/transport/transport_http_modern_test.cpp
      test/transport/transport_http_server_modern_test.cpp
      test/transport/transport_http_server_stream_test.cpp
      test/transport/transport_http_client_modern_test.cpp
      test/transport/transport_http_principal_test.cpp
      test/transport/transport_http_scope_test.cpp
      test/transport/transport_http_mrtr_test.cpp
      test/transport/transport_http_tasks_test.cpp
      test/transport/transport_http_stream_test.cpp
      test/transport/transport_http_listen_test.cpp
      test/transport/transport_memory_test.cpp
      test/transport/socket_deadline_test.cpp
      test/transport/transport_accept_retry_test.cpp
      test/transport/transport_http_limits_test.cpp
      test/transport/transport_http_version_header_test.cpp
      test/transport/http_response_write_test.cpp
      test/e2e/e2e_matrix_test.cpp
      test/e2e/e2e_features_test.cpp
      test/server/runtime_test.cpp
      test/server/server_stdio_test.cpp
      test/server/server_http_test.cpp
      test/server/server_tool_test.cpp
      test/transport/transport_factory_test.cpp
      test/core/gcc11_sso_crash_test.cpp
      test/core/json_peer_input_matrix_test.cpp
      test/support/spawn.cpp
      test/support/test_timeout.cpp)
  # The sealed-state codec is internal and not exported from the shared library,
  # so its test builds the source itself.
  add_executable(mcp-sdk-tests ${TEST_SRCS} src/detail/sealed_state.cpp)
  target_link_libraries(mcp-sdk-tests PRIVATE mcp-cpp-sdk GTest::gtest_main)
  # Compiled apart from the test program, whose unity build would make Task
  # complete: the simple-mode headers must take synchronous handlers while Task
  # is only declared.
  add_library(mcp-simple-mode-headers OBJECT test/headers/simple_server_tu.cpp)
  target_link_libraries(mcp-simple-mode-headers PRIVATE mcp-cpp-sdk)
  add_dependencies(mcp-sdk-tests mcp-simple-mode-headers)
  # The server the stdio client tests launch as a child process.
  add_executable(mcp-test-stdio-server test/support/stdio_test_server.cpp)
  target_link_libraries(mcp-test-stdio-server PRIVATE mcp-cpp-sdk)
  add_dependencies(mcp-sdk-tests mcp-test-stdio-server)
  target_compile_definitions(
    mcp-sdk-tests
    PRIVATE MCP_TEST_STDIO_SERVER="$<TARGET_FILE:mcp-test-stdio-server>"
            MCP_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
  target_compile_definitions(
    mcp-sdk-tests
    PRIVATE MCP_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/test/data")
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # The resolve and socket gates interpose getaddrinfo() and socket() and
    # forward with dlsym(), which lives in libdl before glibc 2.34.
    target_sources(mcp-sdk-tests PRIVATE test/support/resolve_gate.cpp
                                         test/support/socket_gate.cpp)
    target_link_libraries(mcp-sdk-tests PRIVATE ${CMAKE_DL_LIBS})
  endif()
  if(MSVC)
    # A gtest translation unit emits a COMDAT section per template
    # instantiation, and the suites built on nlohmann::json and Boost.Asio
    # instantiate enough of them to pass the 65,279 sections a COFF object can
    # hold (error C1128). The flag lifts that limit to 2^32; the only cost is a
    # larger object file.
    target_compile_options(mcp-sdk-tests PRIVATE /bigobj)
  endif()

  # The peer-input matrix is generated from scripts/json_census.py, and its
  # worth depends on covering every protocol type. Checking that here means a
  # new type with a from_json breaks the build until it is either in the matrix
  # or excluded with a stated reason, rather than quietly arriving untested.
  # Source archives ship test/ but not the matrix scripts, so a packager's
  # BUILD_TESTING=ON build defaults the check off instead of failing on it.
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_json_matrix.py")
    set(_mcp_json_matrix_default ON)
  else()
    set(_mcp_json_matrix_default OFF)
  endif()
  option(
    MCP_CPP_SDK_CHECK_JSON_MATRIX
    "Fail the build when the peer-input matrix is stale (needs Python 3.9+)"
    ${_mcp_json_matrix_default})
  if(MCP_CPP_SDK_CHECK_JSON_MATRIX)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_json_matrix.py")
      message(
        FATAL_ERROR
          "MCP_CPP_SDK_CHECK_JSON_MATRIX=ON but scripts/check_json_matrix.py "
          "is not in this source tree")
    endif()
    find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)
    add_custom_target(
      mcp-json-matrix-complete ALL
      # -B: the build must not write bytecode into the source tree.
      COMMAND "${Python3_EXECUTABLE}" -B
              "${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_json_matrix.py"
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      COMMENT "Checking the peer-input matrix covers every protocol type")
    add_dependencies(mcp-sdk-tests mcp-json-matrix-complete)
    # Inside the gate for the same reason as the check: source archives do not
    # ship these scripts.
    add_test(
      NAME json-matrix-check-unittest
      COMMAND "${Python3_EXECUTABLE}" scripts/test_check_json_matrix.py -v
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
    set_tests_properties(json-matrix-check-unittest
                         PROPERTIES ENVIRONMENT PYTHONDONTWRITEBYTECODE=1)
  elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_json_matrix.py")
    message(STATUS "Peer-input matrix check skipped "
                   "(disabled by MCP_CPP_SDK_CHECK_JSON_MATRIX=OFF)")
  else()
    message(
      STATUS
        "Peer-input matrix check skipped (MCP_CPP_SDK_CHECK_JSON_MATRIX=OFF; "
        "scripts/ is not shipped in source archives)")
  endif()

  include(cmake/McpCppSdkTestSuites.cmake)
  # Default per-test timeout so one deadlocked test cannot stall a ctest run for
  # the 25-minute ctest default (or forever without --timeout). Builds that run
  # the tests many times slower, such as MemorySanitizer, raise it.
  set(MCP_CPP_SDK_TEST_TIMEOUT
      120
      CACHE STRING "Seconds each test may run before ctest stops it")
  if(NOT MCP_CPP_SDK_TEST_TIMEOUT MATCHES "^[0-9]+$" OR MCP_CPP_SDK_TEST_TIMEOUT
                                                        LESS_EQUAL 20)
    message(
      FATAL_ERROR
        "MCP_CPP_SDK_TEST_TIMEOUT must be a whole number of seconds above 20")
  endif()
  # The tests' hang guards follow it and stay below it, so that a hung test
  # reports where it stopped before ctest stops it (see hang_guard.hpp).
  target_compile_definitions(
    mcp-sdk-tests
    PRIVATE MCP_CPP_SDK_TEST_TIMEOUT_SECONDS=${MCP_CPP_SDK_TEST_TIMEOUT})
  # One ctest test per suite: ctest starts the program for each test it
  # registers, and a sanitizer build of it takes longer to start than most tests
  # take to run. test/support/test_timeout.cpp keeps the per-test timeout.
  mcp_cpp_sdk_add_test_suites(mcp-sdk-tests TIMEOUT ${MCP_CPP_SDK_TEST_TIMEOUT})

  # Compiling the test files in batches of a few per translation unit parses the
  # shared headers and instantiates the shared templates once per batch rather
  # than once per file. Only the test program is batched, not the libraries.
  option(MCP_CPP_SDK_TEST_UNITY_BUILD
         "Compile the test program as a unity build" ON)
  set(MCP_CPP_SDK_TEST_UNITY_BATCH_SIZE
      4
      CACHE STRING "Test files compiled together in one unity translation unit")
  if(MCP_CPP_SDK_TEST_UNITY_BUILD)
    if(NOT MCP_CPP_SDK_TEST_UNITY_BATCH_SIZE MATCHES "^[1-9][0-9]*$")
      message(
        FATAL_ERROR
          "MCP_CPP_SDK_TEST_UNITY_BATCH_SIZE must be a positive whole number")
    endif()
    set_target_properties(
      mcp-sdk-tests
      PROPERTIES UNITY_BUILD ON UNITY_BUILD_BATCH_SIZE
                                ${MCP_CPP_SDK_TEST_UNITY_BATCH_SIZE})
    # The support sources are not tests: the gates replace libc functions and
    # sealed_state.cpp is a library source that the test program rebuilds.
    set_source_files_properties(
      test/support/test_timeout.cpp test/support/resolve_gate.cpp
      test/support/socket_gate.cpp src/detail/sealed_state.cpp
      PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)
    # A fixture in a test file's unnamed namespace is the base of the test
    # classes that gtest defines in the enclosing namespace. GCC reports that as
    # a linkage mismatch when the file is included from the unity source, where
    # it cannot reach another translation unit.
    target_compile_options(
      mcp-sdk-tests PRIVATE "$<$<CXX_COMPILER_ID:GNU>:-Wno-subobject-linkage>")
  endif()

  # Optimizing the test code takes most of a test file's compile at -O3, so
  # release builds compile the test program at -O1. The SDK libraries keep the
  # build's own optimization.
  set(GCC_OR_CLANG "$<CXX_COMPILER_ID:GNU,Clang,AppleClang>")
  set(RELEASE_CONFIG "$<CONFIG:Release,RelWithDebInfo,MinSizeRel>")
  target_compile_options(
    mcp-sdk-tests PRIVATE "$<$<AND:${GCC_OR_CLANG},${RELEASE_CONFIG}>:-O1>")

  option(ENABLE_COVERAGE "Enable coverage reporting" OFF)
  if(ENABLE_COVERAGE)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
      # The libraries too, so that the report covers src/ as well as the
      # headers. Whatever links them also links the coverage runtime. Counters
      # are updated atomically because the tests run the SDK on many threads.
      set(COVERAGE_FLAGS --coverage -fprofile-update=atomic -O0 -g)
      foreach(sdk_target IN LISTS MCP_CPP_SDK_INSTALL_TARGETS)
        target_compile_options(${sdk_target} PRIVATE ${COVERAGE_FLAGS})
        target_link_options(${sdk_target} PUBLIC --coverage)
      endforeach()
      target_compile_options(mcp-sdk-tests PRIVATE ${COVERAGE_FLAGS})
      target_link_options(mcp-sdk-tests PRIVATE --coverage)
    endif()
  endif()

  option(ENABLE_SANITIZERS
         "Enable AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
  option(ENABLE_TSAN "Enable ThreadSanitizer" OFF)
  if(ENABLE_TSAN AND ENABLE_SANITIZERS)
    message(
      FATAL_ERROR
        "ENABLE_TSAN and ENABLE_SANITIZERS cannot be combined: ThreadSanitizer "
        "does not run with AddressSanitizer")
  endif()
  if(ENABLE_TSAN)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
      # -g1 keeps the line tables a report needs and drops the rest.
      # ThreadSanitizer stops every thread while it symbolizes a report,
      # including one a suppression then discards, and that pause grows with the
      # debug information it has to read: with full -g it outlasted the tests'
      # own time bounds on CI.
      set(TSAN_FLAGS -fsanitize=thread -fno-omit-frame-pointer -g1 -O1)
      foreach(sdk_target IN LISTS MCP_CPP_SDK_INSTALL_TARGETS)
        target_compile_options(${sdk_target} PUBLIC ${TSAN_FLAGS})
        target_link_options(${sdk_target} PUBLIC -fsanitize=thread)
      endforeach()
      target_compile_options(mcp-sdk-tests PRIVATE ${TSAN_FLAGS})
      target_link_options(mcp-sdk-tests PRIVATE -fsanitize=thread)
    else()
      message(WARNING "ENABLE_TSAN is only supported with GCC or Clang")
    endif()
  endif()
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
      if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        # AddressSanitizer guards each UndefinedBehaviorSanitizer check record
        # as a global, and a position-independent program relocates every one of
        # them each time it starts.
        target_link_options(mcp-sdk-tests PRIVATE -no-pie)
      endif()
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
option(BUILD_EXAMPLES "Build example programs" ${MCP_CPP_SDK_IS_TOP_LEVEL})
if(BUILD_EXAMPLES)
  # The simple-mode programs: server_base and client_base, and the add-ons that
  # each grow one of them by one feature (examples/simple/progression.json).
  set(MCP_SIMPLE_EXAMPLES
      server_base
      server_add_resource
      server_add_prompt
      server_add_http
      server_add_auth
      server_add_context
      server_add_completions
      server_add_handler_pool
      server_add_own_loop
      server_add_elicitation
      server_add_sampling
      server_add_roots
      server_add_middleware
      server_add_pagination
      server_add_tasks
      server_add_subscriptions
      server_add_apps
      server_add_skills
      server_all
      client_base
      client_add_http
      client_add_auth
      client_add_progress
      client_add_logging
      client_add_coroutine
      client_add_restart
      client_add_own_loop
      client_add_elicitation
      client_add_sampling
      client_add_roots
      client_add_tasks
      client_add_subscriptions
      client_add_errors
      client_add_legacy
      client_all)
  foreach(example IN LISTS MCP_SIMPLE_EXAMPLES)
    string(REPLACE "_" "-" example_target "example-simple-${example}")
    add_executable(${example_target} examples/simple/${example}.cpp)
    target_link_libraries(${example_target} PRIVATE mcp-cpp-sdk)
  endforeach()

  add_executable(example-server-stdio examples/servers/stdio/server_stdio.cpp)
  target_link_libraries(example-server-stdio PRIVATE mcp-cpp-sdk)

  add_executable(example-client-stdio examples/clients/stdio/client_stdio.cpp)
  target_link_libraries(example-client-stdio PRIVATE mcp-cpp-sdk)
  # Launches the stdio server that is built beside it.
  add_dependencies(example-client-stdio example-server-stdio)

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

  add_executable(example-feature-schema-validation
                 examples/features/schema_validation.cpp)
  target_link_libraries(example-feature-schema-validation PRIVATE mcp-cpp-sdk)

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

  add_executable(example-feature-modern-client
                 examples/features/modern_client.cpp)
  target_link_libraries(example-feature-modern-client PRIVATE mcp-cpp-sdk)

  add_executable(example-feature-stdio-launch
                 examples/features/stdio_launch.cpp)
  target_link_libraries(example-feature-stdio-launch PRIVATE mcp-cpp-sdk)
  # Launches the simple stdio server that is built beside it.
  add_dependencies(example-feature-stdio-launch example-server-simple)

  add_executable(example-feature-legacy-protocols
                 examples/features/legacy_protocols.cpp)
  target_link_libraries(example-feature-legacy-protocols PRIVATE mcp-cpp-sdk)

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

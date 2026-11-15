# Registers one ctest test per GoogleTest suite of a test program, where
# gtest_discover_tests() registers one per test. ctest starts the program once
# for every test it registers, and in a sanitizer build starting the program
# takes longer than most of its tests do.
#
# mcp_cpp_sdk_add_test_suites(<target> TIMEOUT <seconds per test>)
#
# The suites are listed after each link. A suite may run for the per-test
# timeout times its number of tests; test/support/test_timeout.cpp holds each
# test inside it to the per-test timeout.

set(_mcp_cpp_sdk_list_test_suites
    "${CMAKE_CURRENT_LIST_DIR}/list-test-suites.cmake")

# Registers the suites of <target> with ctest; see the top of this file.
function(mcp_cpp_sdk_add_test_suites target)
  cmake_parse_arguments(PARSE_ARGV 1 arg "" "TIMEOUT" "")
  set(suites_file "${CMAKE_CURRENT_BINARY_DIR}/${target}_suites.cmake")
  set(include_file "${CMAKE_CURRENT_BINARY_DIR}/${target}_suites_include.cmake")
  add_custom_command(
    TARGET ${target}
    POST_BUILD
    COMMAND
      "${CMAKE_COMMAND}" "-DTEST_EXECUTABLE=$<TARGET_FILE:${target}>"
      "-DTEST_TIMEOUT=${arg_TIMEOUT}" "-DSUITES_FILE=${suites_file}" -P
      "${_mcp_cpp_sdk_list_test_suites}"
    BYPRODUCTS "${suites_file}"
    COMMENT "Listing the test suites of ${target}"
    VERBATIM)
  file(
    WRITE "${include_file}"
    "if(EXISTS \"${suites_file}\")\n" "  include(\"${suites_file}\")\n"
    "else()\n" "  add_test(${target}_NOT_BUILT ${target}_NOT_BUILT)\n"
    "endif()\n")
  set_property(
    DIRECTORY
    APPEND
    PROPERTY TEST_INCLUDE_FILES "${include_file}")
endfunction()

# Writes one add_test() per GoogleTest suite of TEST_EXECUTABLE into
# SUITES_FILE, with a timeout of TEST_TIMEOUT seconds per test in the suite.
# mcp_cpp_sdk_add_test_suites() runs it after each link.

execute_process(
  COMMAND "${TEST_EXECUTABLE}" --gtest_list_tests
  OUTPUT_VARIABLE LISTING
  ERROR_VARIABLE ERRORS
  RESULT_VARIABLE RESULT
  TIMEOUT ${TEST_TIMEOUT})
if(NOT RESULT EQUAL 0)
  message(FATAL_ERROR "Listing the tests of ${TEST_EXECUTABLE} failed "
                      "(${RESULT}):\n${LISTING}${ERRORS}")
endif()

# gtest follows a parameterized name with a comment ("  # GetParam() = ...").
# Without the comments only names are left, which hold no list separators or
# brackets.
string(REPLACE "\r" "" LISTING "${LISTING}")
string(REGEX REPLACE "[ ]+#[^\n]*" "" LISTING "${LISTING}")
string(REPLACE "\n" ";" LINES "${LISTING}")

# Appends the test of SUITE, which holds COUNT tests, to CONTENT.
function(append_suite_test)
  if(NOT SUITE STREQUAL "" AND COUNT GREATER 0)
    math(EXPR timeout "${TEST_TIMEOUT} * ${COUNT}")
    set(test "[==[${SUITE}]==]")
    set(content "${CONTENT}")
    string(APPEND content "add_test(${test} [==[${TEST_EXECUTABLE}]==] "
           "[==[--gtest_filter=${SUITE}.*]==])\n"
           "set_tests_properties(${test} PROPERTIES TIMEOUT ${timeout})\n")
    set(CONTENT
        "${content}"
        PARENT_SCOPE)
  endif()
endfunction()

set(CONTENT "")
set(SUITE "")
set(COUNT 0)
foreach(line IN LISTS LINES)
  if(line MATCHES "^([A-Za-z0-9_/]+)\\.$")
    append_suite_test()
    set(SUITE "${CMAKE_MATCH_1}")
    set(COUNT 0)
    if(SUITE MATCHES "(^|/)DISABLED_")
      set(SUITE "")
    endif()
  elseif(line MATCHES "^  ([A-Za-z0-9_/]+)$" AND NOT CMAKE_MATCH_1 MATCHES
                                                 "^DISABLED_")
    math(EXPR COUNT "${COUNT} + 1")
  endif()
endforeach()
append_suite_test()

if(CONTENT STREQUAL "")
  message(FATAL_ERROR "${TEST_EXECUTABLE} lists no tests")
endif()
file(WRITE "${SUITES_FILE}" "${CONTENT}")

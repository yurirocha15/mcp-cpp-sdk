if(NOT DEFINED MCP_CPP_SDK_CONFIG_VERSION_TEMPLATE
   OR NOT DEFINED MCP_CPP_SDK_CONFIG_VERSION_TEST_OUTPUT_DIR)
  message(
    FATAL_ERROR
      "config-version stable test requires its template and output directory")
endif()

file(MAKE_DIRECTORY "${MCP_CPP_SDK_CONFIG_VERSION_TEST_OUTPUT_DIR}")

set(MCP_CPP_SDK_VERSION_FULL "0.2.1")
set(MCP_CPP_SDK_VERSION_PRERELEASE "")
set(MCP_CPP_SDK_VERSION_MAJOR 0)
set(MCP_CPP_SDK_VERSION_MINOR 2)
set(MCP_CPP_SDK_VERSION_CORE "0.2.1")
set(CONFIG_VERSION_FILE
    "${MCP_CPP_SDK_CONFIG_VERSION_TEST_OUTPUT_DIR}/config-version-stable.cmake")
configure_file("${MCP_CPP_SDK_CONFIG_VERSION_TEMPLATE}"
               "${CONFIG_VERSION_FILE}" @ONLY)

foreach(expectation IN ITEMS "0.2.1|FALSE|TRUE|TRUE" "0.2.0|FALSE|FALSE|FALSE"
                             "0.2|FALSE|FALSE|FALSE" "0.2.1|TRUE|FALSE|FALSE")
  string(REPLACE "|" ";" expectation_values "${expectation}")
  list(GET expectation_values 0 REQUESTED)
  list(GET expectation_values 1 IS_RANGE)
  list(GET expectation_values 2 EXPECTED_COMPATIBLE)
  list(GET expectation_values 3 EXPECTED_EXACT)
  unset(PACKAGE_VERSION)
  unset(PACKAGE_VERSION_COMPATIBLE)
  unset(PACKAGE_VERSION_EXACT)
  unset(PACKAGE_FIND_VERSION_RANGE)
  unset(PACKAGE_FIND_VERSION_RANGE_MIN)
  unset(PACKAGE_FIND_VERSION_RANGE_MAX)
  set(PACKAGE_FIND_VERSION "${REQUESTED}")
  if(IS_RANGE)
    set(PACKAGE_FIND_VERSION_RANGE "${REQUESTED}...<0.3.0")
    set(PACKAGE_FIND_VERSION_RANGE_MIN INCLUDE)
    set(PACKAGE_FIND_VERSION_RANGE_MAX EXCLUDE)
    set(PACKAGE_FIND_VERSION_MAX "0.3.0")
  endif()
  include("${CONFIG_VERSION_FILE}")

  if(NOT PACKAGE_VERSION_COMPATIBLE STREQUAL "${EXPECTED_COMPATIBLE}")
    message(
      FATAL_ERROR
        "expected compatibility ${EXPECTED_COMPATIBLE} for ${REQUESTED}, got "
        "${PACKAGE_VERSION_COMPATIBLE}")
  endif()
  if(NOT PACKAGE_VERSION_EXACT STREQUAL "${EXPECTED_EXACT}")
    message(
      FATAL_ERROR "expected exactness ${EXPECTED_EXACT} for ${REQUESTED}, got "
                  "${PACKAGE_VERSION_EXACT}")
  endif()
endforeach()

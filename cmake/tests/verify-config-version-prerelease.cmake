if(NOT DEFINED MCP_CPP_SDK_CONFIG_VERSION_TEMPLATE)
  message(FATAL_ERROR "config-version prerelease test is missing its template")
endif()

set(MCP_CPP_SDK_VERSION_FULL "0.2.0-rc.1")
set(MCP_CPP_SDK_VERSION_PRERELEASE "-rc.1")
set(MCP_CPP_SDK_VERSION_MAJOR 0)
set(MCP_CPP_SDK_VERSION_MINOR 2)
set(MCP_CPP_SDK_VERSION_CORE "0.2.0")
set(CONFIG_VERSION_FILE "${CMAKE_CURRENT_BINARY_DIR}/config-version-rc.cmake")
configure_file("${MCP_CPP_SDK_CONFIG_VERSION_TEMPLATE}"
               "${CONFIG_VERSION_FILE}" @ONLY)

foreach(expectation IN ITEMS "0.2.0-rc.1|TRUE|TRUE" "0.2.0|FALSE|FALSE"
                             "0.2.0-rc.2|FALSE|FALSE")
  string(REPLACE "|" ";" expectation_values "${expectation}")
  list(GET expectation_values 0 REQUESTED)
  list(GET expectation_values 1 EXPECTED_COMPATIBLE)
  list(GET expectation_values 2 EXPECTED_EXACT)
  unset(PACKAGE_VERSION)
  unset(PACKAGE_VERSION_COMPATIBLE)
  unset(PACKAGE_VERSION_EXACT)
  unset(PACKAGE_FIND_VERSION_RANGE)
  set(PACKAGE_FIND_VERSION "${REQUESTED}")
  include("${CONFIG_VERSION_FILE}")

  if(NOT PACKAGE_VERSION_COMPATIBLE STREQUAL "${EXPECTED_COMPATIBLE}")
    message(
      FATAL_ERROR
        "expected compatibility ${EXPECTED_COMPATIBLE} for ${REQUESTED}, got\n"
        "${PACKAGE_VERSION_COMPATIBLE}")
  endif()
  if(NOT PACKAGE_VERSION_EXACT STREQUAL "${EXPECTED_EXACT}")
    message(
      FATAL_ERROR "expected exactness ${EXPECTED_EXACT} for ${REQUESTED}, got\n"
                  "${PACKAGE_VERSION_EXACT}")
  endif()
endforeach()

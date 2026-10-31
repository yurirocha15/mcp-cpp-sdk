if(NOT DEFINED MCP_CPP_SDK_SOURCE_DIR
   OR NOT DEFINED MCP_CPP_SDK_TEST_OUTPUT_DIR
   OR NOT DEFINED MCP_CPP_SDK_GENERATOR)
  message(FATAL_ERROR "subproject defaults test is missing configuration")
endif()

file(REMOVE_RECURSE "${MCP_CPP_SDK_TEST_OUTPUT_DIR}")
set(_consumer_dir "${MCP_CPP_SDK_TEST_OUTPUT_DIR}/consumer")
file(MAKE_DIRECTORY "${_consumer_dir}")

# The consumer clears the three options after project() because a Conan
# toolchain, which the parent build may pass on, sets them; a consumer's own
# toolchain usually does not.
string(
  CONCAT _consumer_lists
         "cmake_minimum_required(VERSION 3.20)\n"
         "project(mcp-cpp-sdk-subproject-consumer CXX)\n"
         "unset(BUILD_TESTING CACHE)\n"
         "unset(BUILD_EXAMPLES CACHE)\n"
         "unset(BUILD_DOCS CACHE)\n"
         "if(CONSUMER_WANTS_SDK_TESTS)\n"
         "  set(BUILD_TESTING ON)\n"
         "endif()\n"
         "add_subdirectory(\"${MCP_CPP_SDK_SOURCE_DIR}\" mcp-cpp-sdk)\n")
file(WRITE "${_consumer_dir}/CMakeLists.txt" "${_consumer_lists}")

set(_configure_arguments
    -G "${MCP_CPP_SDK_GENERATOR}" -DCMAKE_DISABLE_FIND_PACKAGE_GTest=ON
    -DCMAKE_DISABLE_FIND_PACKAGE_Doxygen=ON)
if(NOT "${MCP_CPP_SDK_GENERATOR_PLATFORM}" STREQUAL "")
  list(APPEND _configure_arguments -A "${MCP_CPP_SDK_GENERATOR_PLATFORM}")
endif()
if(NOT "${MCP_CPP_SDK_GENERATOR_TOOLSET}" STREQUAL "")
  list(APPEND _configure_arguments -T "${MCP_CPP_SDK_GENERATOR_TOOLSET}")
endif()
foreach(variable IN ITEMS CMAKE_MAKE_PROGRAM CMAKE_CXX_COMPILER
                          CMAKE_TOOLCHAIN_FILE CMAKE_BUILD_TYPE)
  if(NOT "${MCP_CPP_SDK_${variable}}" STREQUAL "")
    list(APPEND _configure_arguments "-D${variable}=${MCP_CPP_SDK_${variable}}")
  endif()
endforeach()
if(NOT "${MCP_CPP_SDK_PREFIX_PATH}" STREQUAL "")
  string(REPLACE "|" ";" _prefix_path "${MCP_CPP_SDK_PREFIX_PATH}")
  list(APPEND _configure_arguments "-DCMAKE_PREFIX_PATH=${_prefix_path}")
endif()

# Configures the consumer in `build_name` with the extra arguments; sets
# `out_result` and `out_log`.
function(_mcp_configure_consumer build_name out_result out_log)
  execute_process(
    COMMAND
      "${CMAKE_COMMAND}" -S "${_consumer_dir}" -B
      "${MCP_CPP_SDK_TEST_OUTPUT_DIR}/${build_name}" ${_configure_arguments}
      ${ARGN}
    RESULT_VARIABLE configure_result
    OUTPUT_VARIABLE configure_output
    ERROR_VARIABLE configure_output)
  set(${out_result}
      "${configure_result}"
      PARENT_SCOPE)
  set(${out_log}
      "${configure_output}"
      PARENT_SCOPE)
endfunction()

_mcp_configure_consumer(defaults defaults_result defaults_log)
if(NOT defaults_result EQUAL 0)
  message(
    FATAL_ERROR "configuring the SDK as a subproject without GTest failed:\n"
                "${defaults_log}")
endif()
file(READ "${MCP_CPP_SDK_TEST_OUTPUT_DIR}/defaults/CMakeCache.txt"
     defaults_cache)
foreach(option IN ITEMS BUILD_TESTING BUILD_EXAMPLES BUILD_DOCS)
  if(NOT defaults_cache MATCHES "\n${option}:BOOL=OFF\n")
    message(FATAL_ERROR "${option} did not default to OFF in a subproject")
  endif()
endforeach()

# The same configure with the SDK's tests requested has to reach GTest, which
# both shows that an explicit value still wins and that the first configure
# succeeded because the tests were off, not because GTest was found.
_mcp_configure_consumer(explicit-testing explicit_result explicit_log
                        -DCONSUMER_WANTS_SDK_TESTS=ON)
if(explicit_result EQUAL 0)
  message(
    FATAL_ERROR "BUILD_TESTING=ON in the consumer did not enable the SDK tests")
endif()
if(NOT explicit_log MATCHES "CMAKE_DISABLE_FIND_PACKAGE_GTest")
  message(
    FATAL_ERROR "BUILD_TESTING=ON in the consumer failed for another reason:\n"
                "${explicit_log}")
endif()

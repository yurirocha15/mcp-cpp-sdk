if(NOT DEFINED MCP_CPP_SDK_BUILD_DIR
   OR NOT DEFINED MCP_CPP_SDK_TEST_SOURCE
   OR NOT DEFINED MCP_CPP_SDK_CXX_COMPILER
   OR NOT DEFINED MCP_CPP_SDK_INSTALL_LIBDIR
   OR NOT DEFINED MCP_CPP_SDK_BOOST_INCLUDE_DIRS
   OR NOT DEFINED MCP_CPP_SDK_NLOHMANN_INCLUDE_DIRS)
  message(FATAL_ERROR "pkg-config consumer test is missing configuration")
endif()

find_program(PKG_CONFIG_EXECUTABLE pkg-config REQUIRED)

set(_test_root "${MCP_CPP_SDK_BUILD_DIR}/pkgconfig-static-consumer")
set(_install_root "${_test_root}/install")
set(_dependency_pc_dir "${_test_root}/dependencies")
set(_test_executable "${_test_root}/consumer")
file(REMOVE_RECURSE "${_test_root}")
file(MAKE_DIRECTORY "${_dependency_pc_dir}")

foreach(component IN ITEMS Development StaticDevelopment)
  execute_process(
    COMMAND "${CMAKE_COMMAND}" --install "${MCP_CPP_SDK_BUILD_DIR}" --prefix
            "${_install_root}" --component "${component}"
    RESULT_VARIABLE install_result
    OUTPUT_VARIABLE install_output
    ERROR_VARIABLE install_error)
  if(NOT install_result EQUAL 0)
    message(
      FATAL_ERROR
        "failed to install ${component}:\n${install_output}\n${install_error}")
  endif()
endforeach()

string(REPLACE "|" ";" _boost_include_dirs "${MCP_CPP_SDK_BOOST_INCLUDE_DIRS}")
string(REPLACE "|" ";" _nlohmann_include_dirs
               "${MCP_CPP_SDK_NLOHMANN_INCLUDE_DIRS}")
list(GET _nlohmann_include_dirs 0 _nlohmann_include_dir)
string(CONCAT _nlohmann_pc "Name: nlohmann_json\n"
              "Description: JSON for Modern C++\n" "Version: 3.0.0\n"
              "Cflags: -I${_nlohmann_include_dir}\n")
file(WRITE "${_dependency_pc_dir}/nlohmann_json.pc" "${_nlohmann_pc}")

string(CONCAT _pc_path
              "${_install_root}/${MCP_CPP_SDK_INSTALL_LIBDIR}/pkgconfig:"
              "${_dependency_pc_dir}")
if(DEFINED ENV{PKG_CONFIG_PATH} AND NOT "$ENV{PKG_CONFIG_PATH}" STREQUAL "")
  string(APPEND _pc_path ":$ENV{PKG_CONFIG_PATH}")
endif()

# Runs pkg-config with the test's search path and the given arguments; sets
# `out_flags` to its output.
function(_mcp_pkg_config out_flags)
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "PKG_CONFIG_PATH=${_pc_path}"
            "${PKG_CONFIG_EXECUTABLE}" ${ARGN}
    RESULT_VARIABLE pkg_config_result
    OUTPUT_VARIABLE pkg_config_flags
    ERROR_VARIABLE pkg_config_error OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(NOT pkg_config_result EQUAL 0)
    message(FATAL_ERROR "pkg-config ${ARGN} failed: ${pkg_config_error}")
  endif()
  set(${out_flags}
      "${pkg_config_flags}"
      PARENT_SCOPE)
endfunction()

# Fails unless `flags` contains every flag given after it.
function(_mcp_require_flags label flags)
  foreach(expected_flag IN LISTS ARGN)
    string(FIND "${flags}" "${expected_flag}" flag_position)
    if(flag_position EQUAL -1)
      message(FATAL_ERROR "${label} output lacks ${expected_flag}: ${flags}")
    endif()
  endforeach()
endfunction()

if(DEFINED MCP_CPP_SDK_TEST_LINK_OPTIONS)
  string(REPLACE "|" ";" _test_link_options "${MCP_CPP_SDK_TEST_LINK_OPTIONS}")
endif()

# Builds `source` with `flags` into `executable`; sets `out_result` to the
# compiler's exit code and `out_log` to its output.
function(_mcp_link source flags executable out_result out_log)
  separate_arguments(pkg_config_arguments UNIX_COMMAND "${flags}")
  set(compile_arguments "-std=c++20")
  foreach(include_dir IN LISTS _boost_include_dirs)
    list(APPEND compile_arguments "-I${include_dir}")
  endforeach()
  list(
    APPEND
    compile_arguments
    "${source}"
    ${pkg_config_arguments}
    "-o"
    "${executable}"
    ${_test_link_options})
  execute_process(
    COMMAND "${MCP_CPP_SDK_CXX_COMPILER}" ${compile_arguments}
    RESULT_VARIABLE compile_result
    OUTPUT_VARIABLE compile_output
    ERROR_VARIABLE compile_error)
  set(${out_result}
      "${compile_result}"
      PARENT_SCOPE)
  set(${out_log}
      "${compile_output}\n${compile_error}"
      PARENT_SCOPE)
endfunction()

# Builds the consumer source with `flags` and runs it; fails on either error.
function(_mcp_link_and_run label flags executable)
  _mcp_link("${MCP_CPP_SDK_TEST_SOURCE}" "${flags}" "${executable}"
            compile_result compile_log)
  if(NOT compile_result EQUAL 0)
    message(FATAL_ERROR "${label} consumer failed to link:\n${compile_log}\n"
                        "flags: ${flags}")
  endif()
  execute_process(COMMAND "${executable}" RESULT_VARIABLE run_result)
  if(NOT run_result EQUAL 0)
    message(FATAL_ERROR "${label} consumer exited ${run_result}")
  endif()
endfunction()

# The static library linked against the system's shared dependencies: the usual
# way to use it, and what needs nothing beyond the -dev packages.
_mcp_pkg_config(_plain_flags --cflags --libs mcp-cpp-sdk-static)
_mcp_require_flags("pkg-config" "${_plain_flags}" "-lmcp-cpp-sdk-static"
                   "-lcrypto")
_mcp_link_and_run("pkg-config" "${_plain_flags}" "${_test_executable}")

# --static also asks for OpenSSL's own static dependencies. Several
# distributions list libraries in libcrypto.pc (zlib, zstd) that their OpenSSL
# development package does not install, so --static is checked only where
# OpenSSL's own static flags link.
_mcp_pkg_config(_crypto_static_flags --libs --static libcrypto)
set(_probe_source "${_test_root}/probe.cpp")
file(WRITE "${_probe_source}" "int main() { return 0; }\n")
_mcp_link("${_probe_source}" "${_crypto_static_flags}" "${_test_root}/probe"
          probe_result probe_log)
if(NOT probe_result EQUAL 0)
  message(STATUS "OpenSSL's static dependencies do not link here "
                 "(${_crypto_static_flags}); --static was not checked")
  return()
endif()

_mcp_pkg_config(_static_flags --cflags --libs --static mcp-cpp-sdk-static)
_mcp_require_flags("pkg-config --static" "${_static_flags}"
                   "-lmcp-cpp-sdk-static" "-lcrypto")
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
  _mcp_require_flags("Linux static link" "${_static_flags}" "-ldl" "-pthread")
endif()
_mcp_link_and_run("pkg-config --static" "${_static_flags}"
                  "${_test_root}/consumer-static")

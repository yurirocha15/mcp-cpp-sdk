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

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env "PKG_CONFIG_PATH=${_pc_path}"
          "${PKG_CONFIG_EXECUTABLE}" --cflags --libs --static mcp-cpp-sdk-static
  RESULT_VARIABLE pkg_config_result
  OUTPUT_VARIABLE pkg_config_flags
  ERROR_VARIABLE pkg_config_error OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT pkg_config_result EQUAL 0)
  message(FATAL_ERROR "pkg-config failed: ${pkg_config_error}")
endif()

foreach(expected_flag IN ITEMS "-lmcp-cpp-sdk-static" "-lcrypto")
  string(FIND "${pkg_config_flags}" "${expected_flag}" flag_position)
  if(flag_position EQUAL -1)
    message(
      FATAL_ERROR
        "pkg-config --static output lacks ${expected_flag}: ${pkg_config_flags}"
    )
  endif()
endforeach()
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
  foreach(expected_flag IN ITEMS "-ldl" "-pthread")
    string(FIND "${pkg_config_flags}" "${expected_flag}" flag_position)
    if(flag_position EQUAL -1)
      message(
        FATAL_ERROR
          "Linux static link output lacks ${expected_flag}: ${pkg_config_flags}"
      )
    endif()
  endforeach()
endif()

separate_arguments(_pkg_config_arguments UNIX_COMMAND "${pkg_config_flags}")
if(DEFINED MCP_CPP_SDK_TEST_LINK_OPTIONS)
  string(REPLACE "|" ";" _test_link_options "${MCP_CPP_SDK_TEST_LINK_OPTIONS}")
endif()
set(_compile_arguments "-std=c++20")
foreach(include_dir IN LISTS _boost_include_dirs)
  list(APPEND _compile_arguments "-I${include_dir}")
endforeach()
list(APPEND _compile_arguments "${MCP_CPP_SDK_TEST_SOURCE}")
list(APPEND _compile_arguments ${_pkg_config_arguments} "-o"
     "${_test_executable}")
list(APPEND _compile_arguments ${_test_link_options})

execute_process(
  COMMAND "${MCP_CPP_SDK_CXX_COMPILER}" ${_compile_arguments}
  RESULT_VARIABLE compile_result
  OUTPUT_VARIABLE compile_output
  ERROR_VARIABLE compile_error)
if(NOT compile_result EQUAL 0)
  string(CONCAT _compile_failure "pkg-config static consumer failed to link:\n"
                "${compile_output}\n${compile_error}\n"
                "flags: ${pkg_config_flags}")
  message(FATAL_ERROR "${_compile_failure}")
endif()

execute_process(COMMAND "${_test_executable}" RESULT_VARIABLE run_result)
if(NOT run_result EQUAL 0)
  message(FATAL_ERROR "pkg-config static consumer exited ${run_result}")
endif()

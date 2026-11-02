# Verifies that the vendored jsoncons stays private to the SDK library: no
# symbol of the original jsoncons namespace in the static archive, no jsoncons
# symbol exported from the shared library, and no jsoncons file or reference in
# the install tree other than its license, which must be there.
#
# Inputs: MCP_CPP_SDK_NM, MCP_CPP_SDK_BUILD_DIR, and optionally
# MCP_CPP_SDK_STATIC_LIBRARY and MCP_CPP_SDK_SHARED_LIBRARY.

foreach(required MCP_CPP_SDK_NM MCP_CPP_SDK_BUILD_DIR)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "${required} is required")
  endif()
endforeach()

if(MCP_CPP_SDK_STATIC_LIBRARY)
  execute_process(
    COMMAND "${MCP_CPP_SDK_NM}" "${MCP_CPP_SDK_STATIC_LIBRARY}"
    OUTPUT_VARIABLE static_symbols
    RESULT_VARIABLE static_result)
  if(NOT static_result EQUAL 0)
    message(FATAL_ERROR "nm failed on ${MCP_CPP_SDK_STATIC_LIBRARY}")
  endif()
  # N8jsoncons is the Itanium mangling of the namespace jsoncons; the vendored
  # copy is renamed mcp_detail_jsoncons, which mangles differently.
  if(static_symbols MATCHES "N8jsoncons")
    message(
      FATAL_ERROR "the static library contains symbols in namespace jsoncons")
  endif()
endif()

if(MCP_CPP_SDK_SHARED_LIBRARY)
  execute_process(
    COMMAND "${MCP_CPP_SDK_NM}" -D --defined-only
            "${MCP_CPP_SDK_SHARED_LIBRARY}"
    OUTPUT_VARIABLE shared_symbols
    RESULT_VARIABLE shared_result)
  if(NOT shared_result EQUAL 0)
    message(FATAL_ERROR "nm failed on ${MCP_CPP_SDK_SHARED_LIBRARY}")
  endif()
  string(TOLOWER "${shared_symbols}" shared_symbols)
  if(shared_symbols MATCHES "jsoncons")
    message(FATAL_ERROR "the shared library exports jsoncons symbols")
  endif()
endif()

set(VENDORED_INSTALL_DIR "${MCP_CPP_SDK_BUILD_DIR}/vendored-symbols-install")
file(REMOVE_RECURSE "${VENDORED_INSTALL_DIR}")
execute_process(
  COMMAND "${CMAKE_COMMAND}" --install "${MCP_CPP_SDK_BUILD_DIR}" --prefix
          "${VENDORED_INSTALL_DIR}" OUTPUT_QUIET RESULT_VARIABLE install_result)
if(NOT install_result EQUAL 0)
  message(FATAL_ERROR "cmake --install failed")
endif()
file(
  GLOB_RECURSE installed_files
  LIST_DIRECTORIES false
  "${VENDORED_INSTALL_DIR}/*")
set(JSONCONS_LICENSE_INSTALLED FALSE)
foreach(installed IN LISTS installed_files)
  file(RELATIVE_PATH installed_relative "${VENDORED_INSTALL_DIR}"
       "${installed}")
  string(TOLOWER "${installed_relative}" installed_lower)
  if(installed_relative MATCHES "/licenses/mcp-cpp-sdk/jsoncons/LICENSE$")
    set(JSONCONS_LICENSE_INSTALLED TRUE)
  elseif(installed_lower MATCHES "jsoncons")
    message(FATAL_ERROR "jsoncons file installed: ${installed}")
  endif()
  if(installed MATCHES "\\.(cmake|pc)$")
    file(READ "${installed}" contents)
    string(TOLOWER "${contents}" contents)
    if(contents MATCHES "jsoncons|third_party")
      message(FATAL_ERROR "${installed} refers to the vendored jsoncons")
    endif()
  endif()
endforeach()
if(NOT JSONCONS_LICENSE_INSTALLED)
  message(FATAL_ERROR "the jsoncons license is not in the install tree")
endif()
file(REMOVE_RECURSE "${VENDORED_INSTALL_DIR}")

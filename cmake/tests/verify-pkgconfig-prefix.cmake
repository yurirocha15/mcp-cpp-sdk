include("${CMAKE_CURRENT_LIST_DIR}/../McpCppSdkPkgConfig.cmake")

mcp_cpp_sdk_pkgconfig_prefix_from_libdir(lib_prefix "lib")
if(NOT lib_prefix STREQUAL "../../")
  message(FATAL_ERROR "lib/pkgconfig prefix was '${lib_prefix}'")
endif()

mcp_cpp_sdk_pkgconfig_prefix_from_libdir(multiarch_prefix
                                         "lib/x86_64-linux-gnu")
if(NOT multiarch_prefix STREQUAL "../../../")
  message(FATAL_ERROR "multiarch pkgconfig prefix was '${multiarch_prefix}'")
endif()

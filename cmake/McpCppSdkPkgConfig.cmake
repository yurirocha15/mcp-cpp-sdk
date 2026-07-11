# Compute the relative path from an installed pkg-config directory to prefix.
function(mcp_cpp_sdk_pkgconfig_prefix_from_libdir output_variable
         install_libdir)
  if(IS_ABSOLUTE "${install_libdir}")
    message(
      FATAL_ERROR
        "CMAKE_INSTALL_LIBDIR must be relative for relocatable pkg-config files"
    )
  endif()

  file(RELATIVE_PATH prefix_from_pcfile "/${install_libdir}/pkgconfig" "/")
  set(${output_variable}
      "${prefix_from_pcfile}"
      PARENT_SCOPE)
endfunction()

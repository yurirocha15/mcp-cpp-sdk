# Compute the relative path from an installed pkg-config directory to prefix.
function(mcp_cpp_sdk_pkgconfig_prefix_from_libdir output_variable
         install_libdir)
  if(IS_ABSOLUTE "${install_libdir}")
    message(
      FATAL_ERROR
        "CMAKE_INSTALL_LIBDIR must be relative for relocatable pkg-config files"
    )
  endif()

  set(synthetic_prefix "${CMAKE_CURRENT_BINARY_DIR}/pkgconfig-prefix")
  set(synthetic_pcfile_dir "${synthetic_prefix}/${install_libdir}/pkgconfig")
  file(RELATIVE_PATH prefix_from_pcfile "${synthetic_pcfile_dir}"
       "${synthetic_prefix}")
  set(${output_variable}
      "${prefix_from_pcfile}"
      PARENT_SCOPE)
endfunction()

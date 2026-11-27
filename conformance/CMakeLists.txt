add_executable(mcp-conformance-everything-server everything_server.cpp)
target_link_libraries(mcp-conformance-everything-server PRIVATE mcp-cpp-sdk)

add_executable(mcp-conformance-everything-client everything_client.cpp)
target_link_libraries(mcp-conformance-everything-client PRIVATE mcp-cpp-sdk)

# The fixtures report the SDK version from VERSION as their implementation
# version.
foreach(fixture IN ITEMS mcp-conformance-everything-server
                         mcp-conformance-everything-client)
  target_compile_definitions(
    ${fixture}
    PRIVATE MCP_CONFORMANCE_SDK_VERSION="${MCP_CPP_SDK_VERSION_FULL}")
endforeach()

# The server fixture serves the deprecated features the conformance scenarios
# still cover; the client fixture must not use one.
target_compile_definitions(mcp-conformance-everything-server
                           PRIVATE MCP_DISABLE_DEPRECATION_ATTRIBUTES)
mcp_cpp_sdk_deprecations_as_errors(mcp-conformance-everything-client)

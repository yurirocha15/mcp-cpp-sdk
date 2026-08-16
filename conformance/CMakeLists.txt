add_executable(mcp-conformance-everything-server everything_server.cpp)
target_link_libraries(mcp-conformance-everything-server PRIVATE mcp-cpp-sdk)

add_executable(mcp-conformance-everything-client everything_client.cpp)
target_link_libraries(mcp-conformance-everything-client PRIVATE mcp-cpp-sdk)

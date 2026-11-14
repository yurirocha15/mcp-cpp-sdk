#pragma once

#include <string>

namespace mcp_benchmark {

/// The JSON input schema of one benchmark tool, shared by every adapter.
std::string tool_input_schema(const std::string& tool_name);

}  // namespace mcp_benchmark

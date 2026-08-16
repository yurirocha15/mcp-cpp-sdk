#pragma once

#include <memory>
#include <string>

namespace mcp_benchmark {

/// The TM Dev Lab v2 tool workload shared by alternative SDK adapters.
///
/// The SDK-specific adapter owns MCP registration and transport behavior. This
/// class owns only the Redis + API operations, so every adapter executes the
/// same application work and returns the same JSON text shape.
class Workload {
   public:
    explicit Workload(std::string server_type);
    ~Workload();

    Workload(const Workload&) = delete;
    Workload& operator=(const Workload&) = delete;

    std::string invoke(const std::string& tool_name, const std::string& arguments_json);

    static std::string input_schema(const std::string& tool_name);

   private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mcp_benchmark

#include "benchmark_schemas.hpp"

#include <stdexcept>

namespace mcp_benchmark {

std::string tool_input_schema(const std::string& tool_name) {
    if (tool_name == "search_products") {
        return R"({"type":"object","properties":{"category":{"type":"string"},"min_price":{"type":"number"},"max_price":{"type":"number"},"limit":{"type":"integer"}}})";
    }
    if (tool_name == "get_user_cart") {
        return R"({"type":"object","properties":{"user_id":{"type":"string"}}})";
    }
    if (tool_name == "checkout") {
        return R"({"type":"object","properties":{"user_id":{"type":"string"},"items":{"type":"array","items":{"type":"object","properties":{"product_id":{"type":"integer"},"quantity":{"type":"integer"}},"required":["product_id","quantity"]}}}})";
    }
    throw std::invalid_argument("unknown benchmark tool: " + tool_name);
}

}  // namespace mcp_benchmark

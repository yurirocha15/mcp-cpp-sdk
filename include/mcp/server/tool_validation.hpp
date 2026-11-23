#pragma once

#include <cstdint>

namespace mcp {

/**
 * @brief Which checks a Server applies to tool calls (see Server::set_tool_validation()).
 *
 * Both protocol revisions require servers to validate tool inputs; turning a check off is for an
 * application that validates them itself.
 */
struct ToolValidationOptions {
    /// Check `tools/call` arguments against the tool's input schema. Arguments that do not conform
    /// are answered with an `isError` result listing the violations, and the handler does not run.
    bool validate_arguments = true;
    /// Check the `structuredContent` of a successful result against the tool's output schema. A
    /// result that does not conform is a fault in the tool, answered with
    /// `mcp::constants::jsonrpc::g_INTERNAL_ERROR`.
    bool validate_structured_content = true;
};

/**
 * @brief Tool calls a Server's validation has refused (see Server::tool_validation_stats()).
 */
struct ToolValidationStats {
    std::uint64_t argument_rejections = 0;  ///< Calls whose arguments did not conform.
    /// Calls whose result's `structuredContent` did not conform to the output schema.
    std::uint64_t structured_content_rejections = 0;
};

}  // namespace mcp

#include <mcp/protocol/tools.hpp>

#include <utility>

namespace mcp {

CallToolResult make_tool_text_result(std::string text) {
    CallToolResult result;
    TextContent content;
    content.text = std::move(text);
    result.content.emplace_back(std::move(content));
    return result;
}

CallToolResult make_tool_error_result(std::string message) {
    auto result = make_tool_text_result(std::move(message));
    result.isError = true;
    return result;
}

CallToolResult make_tool_structured_result(nlohmann::json structured_content,
                                           std::optional<std::string> text) {
    auto result = make_tool_text_result(text ? std::move(*text) : structured_content.dump());
    result.structuredContent = std::move(structured_content);
    return result;
}

}  // namespace mcp

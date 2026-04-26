#include <mcp/server.hpp>

namespace mcp {

void Server::add_tool(const std::string& name, const std::string& description,
                      const nlohmann::json& input_schema,
                      std::function<nlohmann::json(const nlohmann::json&)> handler) {
    Tool tool;
    tool.name = name;
    tool.description = description;
    tool.inputSchema = input_schema;

    register_tool(tool, name,
                  [h = std::move(handler)](Context& /*ctx*/,
                                           const nlohmann::json& params) -> Task<nlohmann::json> {
                      try {
                          co_return h(params);
                      } catch (const std::exception& e) {
                          CallToolResult err;
                          TextContent tc;
                          tc.text = e.what();
                          err.content.emplace_back(std::move(tc));
                          err.isError = true;
                          nlohmann::json j = std::move(err);
                          co_return j;
                      }
                  });
}

}  // namespace mcp

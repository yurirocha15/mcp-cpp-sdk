#include <mcp/server/server.hpp>

namespace mcp {

void Server::add_tool(const std::string& name, const std::string& description,
                      const nlohmann::json& input_schema,
                      std::function<nlohmann::json(const nlohmann::json&)> handler) {
    Tool tool;
    tool.name = name;
    tool.description = description;
    tool.inputSchema = input_schema;

    register_tool(
        tool, name,
        [h = std::move(handler)](Context& /*ctx*/, const nlohmann::json& params)
            -> Task<nlohmann::json> { co_return h(params); },
        detail::ToolResultMode::eNormalize);
}

void Server::add_raw_tool(const Tool& tool, TypeErasedHandler handler) {
    register_tool(tool, tool.name, std::move(handler), detail::ToolResultMode::eValidated);
}

void Server::add_raw_tool(const Tool& tool,
                          std::function<nlohmann::json(const nlohmann::json&)> handler) {
    // Exceptions are turned into isError results by the dispatcher, which is also where the
    // message is made safe to hand to the peer.
    add_raw_tool(tool,
                 [h = std::move(handler)](Context& /*ctx*/, const nlohmann::json& params)
                     -> Task<nlohmann::json> { co_return h(params); });
}

}  // namespace mcp

#include <mcp/server/server.hpp>

#include <functional>
#include <string>
#include <utility>

namespace mcp {

void Server::add_tool(const std::string& name, const std::string& description,
                      const nlohmann::json& input_schema,
                      std::function<nlohmann::json(const nlohmann::json&)> handler) {
    Tool tool;
    tool.name = name;
    tool.description = description;

    register_sync_tool(
        std::move(tool), input_schema, nullptr,
        [h = std::move(handler)](Context& /*ctx*/, const nlohmann::json& params) { return h(params); },
        detail::ToolResultMode::eNormalize);
}

void Server::add_tool(const std::string& name, const std::string& description,
                      const nlohmann::json& input_schema,
                      std::function<nlohmann::json(Context&, const nlohmann::json&)> handler) {
    Tool tool;
    tool.name = name;
    tool.description = description;
    register_sync_tool(std::move(tool), input_schema, nullptr, std::move(handler),
                       detail::ToolResultMode::eNormalize);
}

void Server::add_tool(const Tool& tool, std::function<nlohmann::json(const nlohmann::json&)> handler) {
    register_sync_tool(
        tool,
        [h = std::move(handler)](Context& /*ctx*/, const nlohmann::json& params) { return h(params); },
        detail::ToolResultMode::eNormalize);
}

void Server::add_tool(const Tool& tool,
                      std::function<nlohmann::json(Context&, const nlohmann::json&)> handler) {
    register_sync_tool(tool, std::move(handler), detail::ToolResultMode::eNormalize);
}

void Server::add_raw_tool(const Tool& tool,
                          std::function<nlohmann::json(const nlohmann::json&)> handler) {
    // Exceptions are turned into isError results by the dispatcher, which is also where the
    // message is made safe to hand to the peer.
    register_sync_tool(
        tool,
        [h = std::move(handler)](Context& /*ctx*/, const nlohmann::json& params) { return h(params); },
        detail::ToolResultMode::eValidated);
}

}  // namespace mcp

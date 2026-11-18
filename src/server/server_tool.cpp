#include <mcp/server/server.hpp>

#include <algorithm>
#include <array>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace mcp {

nlohmann::json tool_params(std::initializer_list<ParamSpec> params) {
    static constexpr std::array<std::string_view, 6> s_types{"string",  "number", "integer",
                                                             "boolean", "array",  "object"};
    auto properties = nlohmann::json::object();
    auto required = nlohmann::json::array();
    std::set<std::string, std::less<>> names;
    for (const auto& param : params) {
        if (param.name.empty()) {
            throw std::invalid_argument("tool_params: an argument has no name");
        }
        if (!names.insert(param.name).second) {
            throw std::invalid_argument("tool_params: the argument " + param.name + " is given twice");
        }
        if (std::ranges::find(s_types, param.type) == s_types.end()) {
            throw std::invalid_argument("tool_params: the argument " + param.name + " has the type \"" +
                                        param.type + "\", which is not a JSON Schema type");
        }
        auto property = nlohmann::json::object();
        property["type"] = param.type;
        if (!param.description.empty()) {
            property["description"] = param.description;
        }
        properties[param.name] = std::move(property);
        if (param.required) {
            required.push_back(param.name);
        }
    }
    nlohmann::json schema = {{"type", "object"}, {"properties", std::move(properties)}};
    if (!required.empty()) {
        schema["required"] = std::move(required);
    }
    return schema;
}

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

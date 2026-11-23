#include <mcp/server/tool_params.hpp>

#include <algorithm>
#include <array>
#include <functional>
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

}  // namespace mcp

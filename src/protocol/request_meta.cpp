#include <mcp/protocol/request_meta.hpp>

namespace mcp {

void to_json(nlohmann::json& json_obj, const UnsupportedProtocolVersionData& data) {
    json_obj = nlohmann::json{{"supported", data.supported}, {"requested", data.requested}};
}

void from_json(const nlohmann::json& json_obj, UnsupportedProtocolVersionData& data) {
    json_obj.at("supported").get_to(data.supported);
    json_obj.at("requested").get_to(data.requested);
}

}  // namespace mcp

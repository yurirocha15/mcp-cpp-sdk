#include <mcp/transport/http_types.hpp>

namespace mcp {

std::string_view http_bearer_token(std::string_view authorization_header) {
    constexpr std::string_view scheme = "Bearer ";
    if (authorization_header.size() <= scheme.size()) {
        return {};
    }

    for (std::size_t index = 0; index < scheme.size(); ++index) {
        auto actual = authorization_header[index];
        auto expected = scheme[index];
        if (actual >= 'A' && actual <= 'Z') {
            actual = static_cast<char>(actual - 'A' + 'a');
        }
        if (expected >= 'A' && expected <= 'Z') {
            expected = static_cast<char>(expected - 'A' + 'a');
        }
        if (actual != expected) {
            return {};
        }
    }
    return authorization_header.substr(scheme.size());
}

}  // namespace mcp

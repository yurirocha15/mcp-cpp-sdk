#include <mcp/client/client_protocol.hpp>

#include <mcp/core/constants.hpp>

#include "mcp/detail/text/diagnostic_text.hpp"

#include <stdexcept>
#include <string>
#include <utility>

namespace mcp {

ClientProtocol ClientProtocol::automatic() { return {Kind::eAuto, std::string()}; }

ClientProtocol ClientProtocol::legacy() {
    return {Kind::eLegacy, std::string(mcp::constants::protocol::g_LATEST_LEGACY_PROTOCOL_VERSION)};
}

ClientProtocol ClientProtocol::pinned(std::string version) {
    if (!is_supported_protocol_version(version)) {
        throw std::invalid_argument("Unknown protocol version: " +
                                    detail::sanitize_for_diagnostics(version));
    }
    return {Kind::eVersion, std::move(version)};
}

}  // namespace mcp

#include <mcp/server/protocol_policy.hpp>

#include "server_impl.hpp"

#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace mcp {

void ServerProtocolPolicy::validate() const {
    if (!serve_modern && !serve_legacy) {
        throw std::invalid_argument("A protocol policy must serve at least one protocol era");
    }
    if (legacy_versions) {
        if (serve_legacy && legacy_versions->empty()) {
            throw std::invalid_argument("A protocol policy serving legacy clients needs a version");
        }
        for (const auto& version : *legacy_versions) {
            if (!is_legacy_protocol_version(version)) {
                throw std::invalid_argument("Not a protocol version negotiated by initialize: " +
                                            detail::sanitize_for_diagnostics(version));
            }
        }
    }
}

std::string negotiate_protocol_version(std::string_view requested, const ServerProtocolPolicy& policy) {
    if (policy_allows_legacy_version(policy, requested)) {
        return std::string(requested);
    }
    // mcp::constants::protocol::g_LEGACY_PROTOCOL_VERSIONS is oldest first, so the last one allowed is
    // the newest.
    for (auto version = mcp::constants::protocol::g_LEGACY_PROTOCOL_VERSIONS.rbegin();
         version != mcp::constants::protocol::g_LEGACY_PROTOCOL_VERSIONS.rend(); ++version) {
        if (policy_allows_legacy_version(policy, *version)) {
            return std::string(*version);
        }
    }
    return std::string(mcp::constants::protocol::g_LATEST_LEGACY_PROTOCOL_VERSION);
}

std::vector<std::string> served_protocol_versions(const ServerProtocolPolicy& policy) {
    std::vector<std::string> versions;
    if (policy.serve_legacy) {
        for (const auto version : mcp::constants::protocol::g_LEGACY_PROTOCOL_VERSIONS) {
            if (policy_allows_legacy_version(policy, version)) {
                versions.emplace_back(version);
            }
        }
    }
    if (policy.serve_modern) {
        versions.insert(versions.end(), mcp::constants::protocol::g_MODERN_PROTOCOL_VERSIONS.begin(),
                        mcp::constants::protocol::g_MODERN_PROTOCOL_VERSIONS.end());
    }
    return versions;
}

}  // namespace mcp

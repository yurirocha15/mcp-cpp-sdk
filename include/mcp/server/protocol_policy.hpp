#pragma once

#include <mcp/core/export.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mcp {

/**
 * @brief Which protocol revisions a Server serves (see Server::set_protocol_policy()).
 *
 * @details A request that carries its protocol version in `_meta` (2026-07-28 and later) is
 * served on its own, without initialize. A client of 2025-11-25 or earlier starts a session with
 * initialize. By default a Server serves both, on the same transport.
 */
struct ServerProtocolPolicy {
    /// Serve requests that carry their protocol version in `_meta`. When false, every such
    /// request is answered with `mcp::constants::jsonrpc::g_UNSUPPORTED_PROTOCOL_VERSION`.
    bool serve_modern = true;
    /// Serve clients that start a session with initialize. When false, initialize is answered
    /// with `mcp::constants::jsonrpc::g_METHOD_NOT_FOUND` naming the versions served in its message and
    /// in `data.supported`, and any other request without a protocol version in `_meta` with
    /// `mcp::constants::jsonrpc::g_INVALID_PARAMS`.
    bool serve_legacy = true;
    /// The versions initialize may negotiate, each one of
    /// mcp::constants::protocol::g_LEGACY_PROTOCOL_VERSIONS. Unset means all of them. An initialize
    /// asking for another version negotiates the newest one listed.
    std::optional<std::vector<std::string>> legacy_versions;

    bool operator==(const ServerProtocolPolicy&) const = default;

    /**
     * @brief Check that the policy can be applied.
     *
     * @throws std::invalid_argument If neither era is served, if legacy clients are served with
     *         an empty `legacy_versions`, or if `legacy_versions` names a version that is not one
     *         of mcp::constants::protocol::g_LEGACY_PROTOCOL_VERSIONS.
     */
    void validate() const;
};

/**
 * @brief The version initialize negotiates under @p policy.
 *
 * @return @p requested when the policy lets initialize negotiate it, otherwise the newest legacy
 *         version the policy allows.
 */
MCP_API std::string negotiate_protocol_version(std::string_view requested,
                                               const ServerProtocolPolicy& policy);

/**
 * @brief The protocol versions served under @p policy, oldest first: the legacy versions
 * initialize may negotiate, then the versions served per request.
 */
MCP_API std::vector<std::string> served_protocol_versions(const ServerProtocolPolicy& policy);

/**
 * @brief Requests a Server refused for their protocol version or per-request protocol fields
 * (see Server::protocol_rejection_stats()).
 */
struct ProtocolRejectionStats {
    /// Requests whose required `_meta` fields were missing or malformed
    /// (`mcp::constants::jsonrpc::g_INVALID_PARAMS`).
    std::uint64_t invalid_metadata = 0;
    /// Requests naming a protocol version the server does not serve
    /// (`mcp::constants::jsonrpc::g_UNSUPPORTED_PROTOCOL_VERSION`).
    std::uint64_t unsupported_version = 0;
    /// Refusals of requests that name no protocol version in `_meta`: server/discover without
    /// one, and, when legacy clients are not served, initialize and every other such request. A
    /// legacy client turned away shows up here and nowhere else.
    std::uint64_t legacy_shaped = 0;
};

}  // namespace mcp

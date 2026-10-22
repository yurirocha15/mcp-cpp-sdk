#pragma once

#include <mcp/protocol/capabilities.hpp>
#include <mcp/protocol/notification.hpp>

#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mcp {

/// @brief `_meta` key carrying a request's protocol version (2026-07-28 and later).
inline constexpr std::string_view g_META_PROTOCOL_VERSION = "io.modelcontextprotocol/protocolVersion";

/// @brief `_meta` key carrying the client capabilities relevant to a request.
inline constexpr std::string_view g_META_CLIENT_CAPABILITIES =
    "io.modelcontextprotocol/clientCapabilities";

/// @brief `_meta` key carrying the client's name and version.
inline constexpr std::string_view g_META_CLIENT_INFO = "io.modelcontextprotocol/clientInfo";

/// @brief `_meta` key carrying the minimum log level the server should emit for a request.
inline constexpr std::string_view g_META_LOG_LEVEL = "io.modelcontextprotocol/logLevel";

/// @brief `_meta` key carrying the server's name and version on a result.
inline constexpr std::string_view g_META_SERVER_INFO = "io.modelcontextprotocol/serverInfo";

/// @brief `_meta` key carrying the id of the subscriptions/listen request a message belongs to.
inline constexpr std::string_view g_META_SUBSCRIPTION_ID = "io.modelcontextprotocol/subscriptionId";

/**
 * @brief The per-request protocol fields of a 2026-07-28 request.
 *
 * @details A request of protocol revision 2026-07-28 or later carries its protocol version,
 * the client's capabilities and, optionally, the client's identity and a log level in
 * `params._meta`, and needs no initialize. A server handler reads them through
 * Context::request_meta(), which is null for a request of an earlier revision.
 */
struct RequestMetadata {
    std::string protocolVersion;               ///< The protocol version of this request.
    ClientCapabilities clientCapabilities;     ///< Client capabilities declared for this request.
    std::optional<Implementation> clientInfo;  ///< Client name and version, when sent.
    std::optional<LoggingLevel> logLevel;      ///< Minimum level to log for this request, when sent.
    /// The request's whole `_meta` object, including keys such as `progressToken`,
    /// `traceparent`, `tracestate` and `baggage`.
    nlohmann::json meta = nlohmann::json::object();
};

/**
 * @brief The `data` of an UnsupportedProtocolVersionError (`g_UNSUPPORTED_PROTOCOL_VERSION`).
 */
struct UnsupportedProtocolVersionData {
    std::vector<std::string> supported;  ///< Protocol versions the server supports.
    std::string requested;               ///< The protocol version the request asked for.
};

inline void to_json(nlohmann::json& json_obj, const UnsupportedProtocolVersionData& data) {
    json_obj = nlohmann::json{{"supported", data.supported}, {"requested", data.requested}};
}

inline void from_json(const nlohmann::json& json_obj, UnsupportedProtocolVersionData& data) {
    json_obj.at("supported").get_to(data.supported);
    json_obj.at("requested").get_to(data.requested);
}

}  // namespace mcp

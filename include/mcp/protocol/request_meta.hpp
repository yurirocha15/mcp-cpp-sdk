#pragma once

#include <mcp/core/constants.hpp>
#include <mcp/protocol/capabilities.hpp>
#include <mcp/protocol/notification.hpp>

#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mcp {

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
    /// `traceparent`, `tracestate` and `baggage`. The server shares it with the request it read
    /// it from rather than copying it; it is never null, and an empty object by default.
    std::shared_ptr<const nlohmann::json> meta = empty_meta();

    /// The empty `_meta` object that a default-constructed RequestMetadata holds.
    static std::shared_ptr<const nlohmann::json> empty_meta() {
        static const auto empty = std::make_shared<const nlohmann::json>(nlohmann::json::object());
        return empty;
    }
};

/**
 * @brief The `data` of an UnsupportedProtocolVersionError
 * (`mcp::constants::jsonrpc::g_UNSUPPORTED_PROTOCOL_VERSION`).
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

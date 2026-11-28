#include <mcp/client/error.hpp>
#include <mcp/core/constants.hpp>

#include "mcp/detail/text/diagnostic_text.hpp"

#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace mcp {

/// `error.message` is text the peer chose (dispatch_response() deserializes `error` with no
/// validation) and what() is what an application logs, so what() is flattened and bounded. error(),
/// and the message() it exposes, are structured protocol data a caller may compare or re-encode, and
/// stay exactly as the peer sent them; the split matches MetadataPolicyError's.
McpError::McpError(Error error)
    : std::runtime_error("JSON-RPC error " + std::to_string(error.code) + ": " +
                         detail::sanitize_for_diagnostics(error.message)),
      error_(std::move(error)) {}

McpError::McpError(int code, std::string message, std::optional<nlohmann::json> data)
    : McpError(Error{.code = code, .message = std::move(message), .data = std::move(data)}) {}

std::optional<std::vector<ElicitRequestURLParams>> url_elicitations_required(const McpError& error) {
    const auto& data = error.data();
    if (error.code() != constants::jsonrpc::g_URL_ELICITATION_REQUIRED || !data || !data->is_object()) {
        return std::nullopt;
    }
    const auto listed = data->find("elicitations");
    if (listed == data->end() || !listed->is_array()) {
        return std::nullopt;
    }
    std::vector<ElicitRequestURLParams> elicitations;
    elicitations.reserve(listed->size());
    try {
        for (const auto& item : *listed) {
            auto elicitation = item.get<ElicitRequestURLParams>();
            if (elicitation.mode != "url" || !elicitation.elicitationId) {
                return std::nullopt;
            }
            elicitations.push_back(std::move(elicitation));
        }
    } catch (const nlohmann::json::exception&) {
        return std::nullopt;
    }
    return elicitations;
}

}  // namespace mcp

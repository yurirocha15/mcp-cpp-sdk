#include <mcp/client/error.hpp>

#include "mcp/detail/text/diagnostic_text.hpp"

#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <utility>

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
    : McpError(Error{code, std::move(message), std::move(data)}) {}

}  // namespace mcp

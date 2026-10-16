#pragma once

#include <stdexcept>

namespace mcp::detail {

/// Thrown by the bundled auth middleware to refuse a request.
///
/// Middleware wraps every handler kind, but only a tool result has a place for an error: a tool
/// call reports the refusal inside its result as `isError`, the way it reports a tool's own
/// failure, and every other request kind answers it as a JSON-RPC error.
class MiddlewareRejection : public std::runtime_error {
   public:
    using std::runtime_error::runtime_error;
};

}  // namespace mcp::detail

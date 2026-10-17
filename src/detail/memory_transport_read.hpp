#pragma once

#include <mcp/transport/memory.hpp>

#include <nlohmann/json.hpp>

#include <optional>
#include <string>

namespace mcp::detail {

/// Reads used by the SDK's own loops over a MemoryTransport, which end at close.
///
/// MemoryTransport::read_message() and read_json() report a closed transport by throwing. A read
/// loop meets that once per session, and the exception passes through every coroutine frame
/// between the transport and the loop. These return std::nullopt instead.
struct MemoryTransportAccess {
    static Task<std::optional<std::string>> read_message_until_closed(MemoryTransport& transport);
    static Task<std::optional<nlohmann::json>> read_json_until_closed(MemoryTransport& transport);
};

}  // namespace mcp::detail

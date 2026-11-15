#pragma once

#include <mcp/core/constants.hpp>
#include <mcp/core/context.hpp>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace mcp {

/**
 * @brief Per-call state for Server::dispatch_request_direct().
 */
struct RequestContext {
    /// Cancellation flag for this call: setting it cancels the request, and its handler sees it
    /// through Context::is_cancelled(). When null, the call gets a flag of its own.
    std::shared_ptr<std::atomic<bool>> cancelled;
    /// The `Mcp-Param-{Name}` headers of the HTTP request that carried this call, as
    /// {Name, value} pairs with the value as received. When set, a 2026-07-28 tools/call is
    /// answered with `mcp::constants::jsonrpc::g_HEADER_MISMATCH` unless each argument its tool mirrors
    /// into a header
    /// (`x-mcp-header`) that is present and not null has exactly one matching header, and every
    /// header the tool names has a valid value. Leave unset for a request that did not arrive
    /// over HTTP.
    std::optional<std::vector<std::pair<std::string, std::string>>> param_headers;
    /// Receives, serialized and in order, each notification a handler sends while serving a
    /// 2026-07-28 request (log messages, progress), so a transport can deliver them ahead of the
    /// answer. When unset, they go to the transport of a running session, if any.
    MessageSender notifications;
    /// Who sent this call, as the transport that carried it authenticated the sender; handlers see
    /// it as Context::principal(). Empty when unknown.
    std::string principal;
    /// Called by the server, while it serves a subscriptions/listen request, with a function to
    /// call whenever `cancelled` is set (from any thread, after setting it), so a request that
    /// waits for the client to go away is woken. A transport that streams notifications supplies
    /// it so that closing the stream ends the subscription. When unset, `cancelled` is not
    /// watched by anything but the request itself.
    std::function<void(std::function<void()>)> watch_cancel;
};

}  // namespace mcp

#pragma once

// The MCP server and client API, without Boost. Code that defines or awaits coroutines adds
// <mcp/core/async.hpp>; transports and other advanced parts have headers of their own, such as
// <mcp/transport/stdio.hpp>.

#include <mcp/client/client.hpp>
#include <mcp/core/concepts.hpp>
#include <mcp/core/export.hpp>
#include <mcp/core/runtime.hpp>
#include <mcp/core/version.hpp>
#include <mcp/protocol/protocol.hpp>
#include <mcp/server/server.hpp>
#include <mcp/transport/transport_factory.hpp>

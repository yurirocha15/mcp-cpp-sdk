#pragma once

#include <mcp/core/export.hpp>
#include <mcp/transport/transport.hpp>

#include <memory>
#include <string>

namespace mcp {

class Runtime;

/**
 * @brief Legacy free-function factory for stdio transport.
 * @deprecated Construct the transport with Runtime::executor() instead, such as
 *             `std::make_shared<StdioTransport>(runtime.executor())` (`mcp/transport/stdio.hpp`).
 */
MCP_DEPRECATED(
    "construct StdioTransport with Runtime::executor(); removal no earlier than the next major "
    "release")
MCP_API std::shared_ptr<ITransport> make_stdio_transport(Runtime& runtime);

/**
 * @brief Legacy free-function factory for HTTP client transport.
 * @deprecated Construct the transport with Runtime::executor() instead, such as
 *             `std::make_shared<HttpClientTransport>(runtime.executor(), url)`
 *             (`mcp/transport/http_client.hpp`).
 * @throws std::invalid_argument If @p url is malformed.
 */
MCP_DEPRECATED(
    "construct HttpClientTransport with Runtime::executor(); removal no earlier than the next "
    "major release")
MCP_API std::shared_ptr<ITransport> make_http_client_transport(Runtime& runtime,
                                                               const std::string& url);

}  // namespace mcp

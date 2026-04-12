#pragma once

#include <mcp/export.hpp>
#include <mcp/transport.hpp>

#include <memory>
#include <string>

namespace mcp {

class Runtime;

/**
 * @brief Create a stdio transport bound to the given runtime.
 *
 * @param runtime The event-loop runtime that drives async I/O.
 * @return Owning pointer to the stdio transport.
 */
MCP_API std::unique_ptr<ITransport> make_stdio_transport(Runtime& runtime);

/**
 * @brief Create an HTTP client transport bound to the given runtime.
 *
 * @param runtime The event-loop runtime that drives async I/O.
 * @param url     HTTP endpoint URL (e.g. "http://localhost:8080/mcp").
 * @return Owning pointer to the HTTP client transport.
 *
 * @throws std::invalid_argument If the URL is malformed or does not use http://.
 */
MCP_API std::unique_ptr<ITransport> make_http_client_transport(Runtime& runtime,
                                                               const std::string& url);

}  // namespace mcp

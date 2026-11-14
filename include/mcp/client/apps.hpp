#pragma once

#include <mcp/client/client.hpp>
#include <mcp/core/export.hpp>
#include <mcp/protocol/apps.hpp>

#include <optional>
#include <string>

/**
 * @file apps.hpp
 * @brief Client-side helpers that detect and read MCP Apps views.
 *
 * @details These cover the protocol side of a host: finding the tools that link a view and
 * fetching the view with its security metadata. Rendering it in a sandbox is the host
 * application's work. A host declares support with apps::declare_support() on the capabilities it
 * passes to Client::connect().
 */

namespace mcp::apps {

/// A view as a host receives it.
struct View {
    std::string uri;       ///< The view's `ui://` URI.
    std::string mimeType;  ///< Always `text/html;profile=mcp-app`.
    std::string html;      ///< The HTML document (decoded when the server sent it as a blob).
    /// The `_meta.ui` of the content, or of the listing entry passed to read_view() when the
    /// content has none.
    std::optional<ResourceMeta> meta;
};

/**
 * @brief Read a view with `resources/read`.
 *
 * @param client A connected client.
 * @param uri The view's `ui://` URI, from apps::tool_view_uri() or a resource listing.
 * @param listed The `resources/list` entry of the view, when the host has it. Its `_meta.ui` is the
 *        fallback when the content carries none; the content's value takes precedence.
 * @return The view.
 * @throws std::invalid_argument When @p uri does not start with `ui://`.
 * @throws std::runtime_error When the read returns no content for the URI, content that is not
 *         `text/html;profile=mcp-app`, or a blob that is not valid base64.
 * @throws McpError When the server refuses the read.
 */
MCP_API Task<View> read_view(Client& client, std::string uri,
                             std::optional<Resource> listed = std::nullopt);

/// Reads the view a tool links to. @throws std::invalid_argument When the tool links no view.
MCP_API Task<View> read_tool_view(Client& client, Tool tool,
                                  std::optional<Resource> listed = std::nullopt);

}  // namespace mcp::apps

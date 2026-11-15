#pragma once

#include <mcp/core/export.hpp>
#include <mcp/protocol/apps.hpp>
#include <mcp/server/server.hpp>

#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

/**
 * @file apps.hpp
 * @brief Server-side registration of MCP Apps views and the tools that link to them.
 *
 * @details A server registers each view once as a `ui://` resource and names it from the tools that
 * render with it. A server that wants a text-only variant for hosts without MCP Apps support checks
 * apps::supports_views() on the request's Context.
 */

namespace mcp::apps {

/// A view: an HTML document the host renders in a sandbox when a linked tool returns.
struct ViewResource {
    std::string uri;                         ///< `ui://...`; must start with `ui://`.
    std::string name;                        ///< Human-readable name.
    std::optional<std::string> title;        ///< Display title.
    std::optional<std::string> description;  ///< What the view does and when it applies.
    std::string html;                        ///< The HTML5 document, served as `text`.
    /// Content Security Policy, permissions, domain and border preference. Sent on the content of
    /// every read, where the host looks first.
    std::optional<ResourceMeta> meta;
    /// Also send @ref meta on the `resources/list` entry, where a host can review it at connection
    /// time without reading the view.
    bool list_meta{false};
};

/**
 * @brief Register a view as a resource with the `text/html;profile=mcp-app` MIME type.
 *
 * @param server The server to register on, before it serves requests.
 * @param view The view to serve.
 * @throws std::invalid_argument When the URI does not start with `ui://`, @p view.html is empty,
 *         or a resource with that URI is registered already.
 */
MCP_API void add_view_resource(Server& server, const ViewResource& view);

/**
 * @brief Return @p tool with its `_meta.ui` set to @p meta, keeping its other `_meta` members.
 *
 * @throws std::invalid_argument When @p meta names a resource URI that does not start with `ui://`.
 */
[[nodiscard]] MCP_API Tool link_tool_to_view(Tool tool, const ToolMeta& meta);

/**
 * @brief Register a tool whose result a host renders with a view.
 *
 * @details The tool is registered like Server::add_raw_tool(), so its handler returns a complete
 * CallToolResult object. It should return meaningful `content` as well, for hosts that render
 * nothing.
 *
 * @param server The server to register on.
 * @param tool The tool; its `_meta.ui` is set from @p meta.
 * @param meta The view the tool links to and who may call it.
 * @param handler Handler returning the CallToolResult object.
 * @throws std::invalid_argument As link_tool_to_view() and Server::add_raw_tool().
 */
MCP_API void add_view_tool(Server& server, Tool tool, const ToolMeta& meta,
                           std::function<nlohmann::json(const nlohmann::json&)> handler);

/// Asynchronous form of add_view_tool().
MCP_API void add_view_tool(Server& server, Tool tool, const ToolMeta& meta, TypeErasedHandler handler);

}  // namespace mcp::apps

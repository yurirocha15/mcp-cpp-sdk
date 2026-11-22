MCP Apps
========

MCP Apps (``io.modelcontextprotocol/ui``) lets a server attach an interactive
HTML view to a tool. The server registers the view once, as a ``ui://``
resource served with the MIME type ``text/html;profile=mcp-app``, and names it
in the ``_meta.ui`` member of each tool that renders with it. A host that
supports the extension reads the view with ``resources/read`` and shows it in a
sandbox next to the tool's result; a host that does not shows the tool's
ordinary result.

The SDK covers the protocol side: registering views and linking tools to them
on a server, and detecting and reading them on a client. Rendering a view, the
sandbox, and the messages between a view and its host are the host
application's work and are not part of the SDK. The types follow revision
2026-01-26 of the extension specification.

Using MCP Apps
--------------

``apps::add_view_resource()`` registers the page under a ``ui://`` URI, and
``apps::add_view_tool()`` registers a tool linked to it. In sync mode the tool's
handler is a plain function; in async mode it is a coroutine that takes the
request's ``Context``:

.. literalinclude:: ../../examples/sync/server.cpp
   :language: cpp
   :caption: Sync mode
   :start-after: // MCP Apps:
   :end-before: // Agent Skills.
   :dedent: 4

.. literalinclude:: ../../examples/async/server.cpp
   :language: cpp
   :caption: Async mode
   :start-after: // MCP Apps:
   :end-before: // Agent Skills.
   :dedent: 4

Serving views
-------------

.. code-block:: cpp

   #include <mcp/server/apps.hpp>

   mcp::ServerCapabilities capabilities;
   capabilities.tools = mcp::ServerCapabilities::ToolsCapability{};
   capabilities.resources = mcp::ServerCapabilities::ResourcesCapability{};
   mcp::Server server({"weather", "1.0.0"}, capabilities);

   mcp::apps::ViewResource view;
   view.uri = "ui://weather/dashboard";
   view.name = "weather_dashboard";
   view.html = "<!DOCTYPE html><html>...</html>";
   view.meta = mcp::apps::ResourceMeta{};
   view.meta->csp = mcp::apps::ResourceCsp{};
   view.meta->csp->connectDomains = std::vector<std::string>{"https://api.openweathermap.org"};
   view.meta->prefersBorder = true;
   mcp::apps::add_view_resource(server, view);

   mcp::Tool tool;
   tool.name = "get_weather";
   tool.inputSchema = {{"type", "object"}};

   mcp::apps::ToolMeta link;
   link.resourceUri = "ui://weather/dashboard";
   mcp::apps::add_view_tool(server, tool, link,
       [](const nlohmann::json&) {
           return nlohmann::json{
               {"content", {{{"type", "text"}, {"text", "Sunny, 21 C"}}}}};
       });

``add_view_resource()`` rejects a URI that does not start with ``ui://``. The
view's metadata (the Content Security Policy domains, requested permissions, a
dedicated domain and the border preference) is sent with the content of every
read, which is where a host looks first. Setting ``list_meta`` also sends it
with the ``resources/list`` entry, so a host can review it at connection time.

``add_view_tool()`` registers the tool like ``Server::add_raw_tool()``, so the
handler returns a complete ``CallToolResult`` object; it should return
meaningful ``content`` as well, for hosts that render nothing.
``ToolMeta::visibility`` limits who may call the tool: ``Visibility::eModel``
for the model and ``Visibility::eApp`` for the view. A tool that lists only
``eApp`` is for the view alone, and a host leaves it out of the tool list it
gives the model. Without a visibility, both may call it.

Serving a text-only variant
---------------------------

A host declares support in the capabilities it sends. A handler checks them
with ``mcp::apps::supports_views(ctx)``, which reads the capabilities of a
2026-07-28 request and is false for a request of an earlier revision:

.. code-block:: cpp

   server.add_raw_tool(tool, [](mcp::Context& ctx, const nlohmann::json&)
                                 -> mcp::Task<nlohmann::json> {
       const bool view = mcp::apps::supports_views(ctx);
       co_return nlohmann::json{{"content",
           {{{"type", "text"}, {"text", view ? "See the dashboard" : "Sunny, 21 C"}}}}};
   });

Hosts
-----

A host declares support when it connects:

.. code-block:: cpp

   #include <mcp/core/async.hpp>
   #include <mcp/client/apps.hpp>

   mcp::ClientCapabilities capabilities;
   mcp::apps::declare_support(capabilities);   // mimeTypes: text/html;profile=mcp-app
   auto connected = co_await client.connect(info, capabilities);

   auto tools = co_await client.list_tools();
   for (const auto& tool : tools.tools) {
       if (!mcp::apps::visible_to_model(tool)) continue;   // app-only tools are not the model's
       if (auto uri = mcp::apps::tool_view_uri(tool)) {
           mcp::apps::View view = co_await mcp::apps::read_view(client, *uri);
           // view.html, view.meta (CSP, permissions, ...): hand them to the sandbox
       }
   }

``tool_view_uri()`` reads ``_meta.ui.resourceUri``, and falls back to the
deprecated flat ``_meta["ui/resourceUri"]`` that earlier servers send.
``read_view()`` returns the HTML (decoded when the server sent a blob) and the
``_meta.ui`` of the content; it throws when the content is not
``text/html;profile=mcp-app``. When the host passes the view's
``resources/list`` entry, that entry's ``_meta.ui`` is the fallback for content
without any, and the content's value takes precedence.

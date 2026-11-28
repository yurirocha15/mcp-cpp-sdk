Resources
=========

Resources are static or dynamic data provided by the MCP server to the LLM.
Unlike tools, resources are read-only and allow the server to expose files,
database records, or live telemetry data to the model.

In ``mcp-cpp-sdk``, resources can be registered either directly or via templates.
Resources are identified by URIs, which should follow a hierarchical structure
appropriate for your application.

Using Resources
---------------

``add_text_resource()`` registers a resource whose contents are the text a
function returns, read again on each ``resources/read``. In sync mode the
function returns ``std::string``; in async mode it is a coroutine that returns
``mcp::Task<std::string>``:

.. literalinclude:: ../../examples/sync/server.cpp
   :language: cpp
   :caption: Sync mode
   :start-after: // Resources.
   :end-before: // Resource subscriptions:
   :dedent: 4

.. literalinclude:: ../../examples/async/server.cpp
   :language: cpp
   :caption: Async mode
   :start-after: // Resources.
   :end-before: // Resource subscriptions:
   :dedent: 4

The MIME type is ``text/plain`` unless a fourth argument names another. A
``Resource`` passed in place of the URI and name sets the title, description,
annotations, icons and size as well.

A client reads a resource with ``read_resource()``. Its ``contents`` hold one
``TextResourceContents`` or ``BlobResourceContents`` per part:

.. literalinclude:: ../../examples/sync/client.cpp
   :language: cpp
   :caption: Sync mode
   :start-after: // Resources.
   :end-before: // Prompts.
   :dedent: 4

.. literalinclude:: ../../examples/async/client.cpp
   :language: cpp
   :caption: Async mode
   :start-after: // Resources.
   :end-before: // Prompts.
   :dedent: 4

Static Resources
----------------

A static resource is a direct mapping from a fixed URI to a specific value or
handler. These are ideal for configuration files, documentation, or fixed data
sets that don't change their identification scheme.

Register metadata and a typed read handler with ``add_resource``:

.. code-block:: cpp

   #include <mcp/mcp.hpp>

   mcp::Resource resource;
   resource.uri = "mcp://logs/system";
   resource.name = "System logs";
   resource.mimeType = "text/plain";

   server.add_resource<mcp::ReadResourceRequestParams, mcp::ReadResourceResult>(
       resource, [](mcp::ReadResourceRequestParams request) {
           mcp::TextResourceContents contents;
           contents.uri = std::move(request.uri);
           contents.mimeType = "text/plain";
           contents.text = "System is running normally.";

           mcp::ReadResourceResult result;
           result.contents.emplace_back(std::move(contents));
           return result;
       });

For more complex resources, you can use the structured protocol types:

.. literalinclude:: ../../examples/servers/stdio/server_stdio.cpp
   :language: cpp
   :start-after: // Resource: config://app/settings
   :end-before: // Resource template: users://{user_id}/profile
   :dedent: 8
   :linenos:
   :caption: Example of a structured static resource registration.

Resource Templates
------------------

Resource templates let one handler serve a family of URIs. The matcher
supports the common RFC 6570 expression forms for routing, but it is not a full
RFC 6570 expansion or variable-extraction engine. The handler receives the full
requested URI and can parse or validate application-specific segments itself.

.. code-block:: cpp

   mcp::ResourceTemplate logs;
   logs.uriTemplate = "mcp://logs/{node}";
   logs.name = "Node logs";
   logs.mimeType = "text/plain";

   server.add_resource_template<mcp::ReadResourceRequestParams,
                                mcp::ReadResourceResult>(
       logs, [](mcp::ReadResourceRequestParams request) {
           mcp::TextResourceContents contents;
           contents.uri = std::move(request.uri);
           contents.mimeType = "text/plain";
           contents.text = "Logs for requested node";

           mcp::ReadResourceResult result;
           result.contents.emplace_back(std::move(contents));
           return result;
       });

Dynamic Resources
-----------------

Dynamic resources are those whose content or availability changes over time.
While the registration might look similar to static resources, the handler
is invoked every time the client reads the resource, allowing you to return
fresh data.

If the list of available resources itself changes (e.g., new files are created),
you should notify the client so they can refresh their cache:

.. code-block:: cpp

   // After adding or removing resources
   co_await server.notify_resources_list_changed();

Resource Subscriptions
----------------------

Clients can subscribe to resources to receive notifications when their content
changes.

The server can track these subscriptions and perform actions (like starting a
polling loop or opening a file watch) when a client expresses interest.

.. literalinclude:: ../../examples/features/legacy_protocols.cpp
   :language: cpp
   :start-after: auto unsubscribed = std::make_shared<std::vector<std::string>>();
   :end-before: auto transport = start_server(server, executor);
   :dedent: 4
   :linenos:
   :caption: Handling resource subscriptions and unsubscriptions.

See :doc:`subscriptions` for the notifications they deliver.

Resource Change Notifications
-----------------------------

When a resource's content changes, call ``notify_resource_updated`` on the
``Server``. It sends only to the clients that subscribed to the exact URI, with
``resources/subscribe`` in a session of an older revision or in the filter of a
``subscriptions/listen`` stream (see :doc:`subscriptions`). A ``Server`` served
by ``run_http()``, or given to a ``StreamableHttpSessionManager``, reaches all of
its sessions with one call. A manager built from a server factory serves each
session of an older revision with a ``Server`` of its own, so the application
calls the method on each of them and on the manager's ``modern_server()``.

.. code-block:: cpp

   // Notify subscribers that a specific resource has changed
   co_await server.notify_resource_updated("mcp://status/counter");

The notification tells a subscribed client that it should read the resource
again; delivery and refresh timing still depend on the connection and client.

.. literalinclude:: ../../examples/features/notifications_subscriptions.cpp
   :language: cpp
   :start-after: // ========== SERVER SENDS, CLIENT READS ==========
   :end-before: std::cout << "[Server] Triggering notify_tools_list_changed
   :dedent: 20
   :linenos:
   :caption: Triggering a resource update notification.

Resource Capabilities
---------------------

A server that has a resource or resource template advertises the resources
capability on its own. What registration cannot imply, such as subscriptions or
list-changed notifications, is set in the capabilities the server is constructed
with, or on ``server.capabilities()`` before it serves:

.. code-block:: cpp

   mcp::ServerCapabilities caps;
   caps.resources = mcp::ServerCapabilities::ResourcesCapability{
       .subscribe = true // Enable support for subscriptions
   };

   mcp::Server server(info, std::move(caps));

Best Practices
--------------

- **Use Descriptive URIs**: Follow a clear scheme like ``myapp://records/{id}``.
- **MIME Types**: Always provide accurate MIME types to help the client
  interpret the data.
- **Error Handling**: Throwing an exception in a resource handler will
  automatically return an appropriate JSON-RPC error to the client.
- **Context Awareness**: A resource handler can use ``Context`` for logging and
  reverse requests. Resource reads do not currently propagate a request
  ``progressToken`` into the handler context, so do not rely on progress
  reporting for them. See :doc:`context` for more information.

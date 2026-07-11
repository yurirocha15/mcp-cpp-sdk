Resources
=========

Resources are static or dynamic data provided by the MCP server to the LLM.
Unlike tools, resources are read-only and allow the server to expose files,
database records, or live telemetry data to the model.

In ``mcp-cpp-sdk``, resources can be registered either directly or via templates.
Resources are identified by URIs, which should follow a hierarchical structure
appropriate for your application.

Static Resources
----------------

A static resource is a direct mapping from a fixed URI to a specific value or
handler. These are ideal for configuration files, documentation, or fixed data
sets that don't change their identification scheme.

You can add a static resource using the ``add_resource`` method. The library
provides high-level overloads for common use cases:

.. code-block:: cpp

   #include <mcp/mcp.hpp>

   server.add_resource("mcp://logs/system", "System logs", "text/plain",
                       []() -> std::string {
                           return "System is running normally.";
                       });

For more complex resources, you can use the structured protocol types:

.. literalinclude:: ../../examples/servers/stdio/server_stdio.cpp
   :language: cpp
   :lines: 120-136
   :linenos:
   :caption: Example of a structured static resource registration.

Resource Templates
------------------

Resource templates allow you to define a URI pattern with parameters using the
RFC 6570 URI Template syntax. This is powerful for exposing collections of data
where the specific resource identity depends on a parameter, such as a database
record ID or a filename.

The server will expand the URI when a client requests it, and pass the extracted
parameters to your handler.

.. code-block:: cpp

   server.add_resource_template("mcp://logs/{node}", "Node logs",
                                [](const std::map<std::string, std::string>& params) -> std::string {
                                    std::string node = params.at("node");
                                    return "Logs for node: " + node;
                                });

Templates are particularly useful when you have a large or open-ended set of
resources that share the same schema or purpose.

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
changes. This is essential for live data like logs, telemetry, or collaborative
documents.

The server can track these subscriptions and perform actions (like starting a
polling loop or opening a file watch) when a client expresses interest.

.. literalinclude:: ../../examples/features/notifications_subscriptions.cpp
   :language: cpp
   :lines: 64-73
   :linenos:
   :caption: Handling resource subscriptions and unsubscriptions.

For more details on how notifications work across the protocol, see :doc:`/index`.

Resource Change Notifications
-----------------------------

When a resource's content changes, the server should notify all active
subscribers using ``notify_resource_updated``. The library handles the
routing of these notifications to the correct clients.

.. code-block:: cpp

   // Notify subscribers that a specific resource has changed
   co_await server.notify_resource_updated("mcp://status/counter");

This mechanism ensures that the LLM or the client application always has
access to the most up-to-date information without constant polling.

.. literalinclude:: ../../examples/features/notifications_subscriptions.cpp
   :language: cpp
   :lines: 148-150
   :linenos:
   :caption: Triggering a resource update notification.

Resource Capabilities
---------------------

To allow clients to discover your resources, ensure you enable the resources
capability in your server configuration. You can also specify if the server
supports sending notifications for resource changes.

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
- **Context Awareness**: Use the handler's ``Context`` object for logging or
  reporting progress for long-running resource generation. See :doc:`context`
  for more information.

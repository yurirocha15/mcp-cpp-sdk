Resources
=========

Resources are static or dynamic data provided by the MCP server to the LLM.
Unlike tools, resources are read-only and allow the server to expose files,
database records, or live telemetry data to the model.

In `mcp-cpp-sdk`, resources can be registered either directly or via templates.

Static Resources
----------------

A static resource is a direct mapping from a URI to a value:

.. code-block:: cpp

   #include <mcp/mcp.hpp>

   server.add_resource("mcp://logs/system", "System logs", "text/plain",
                       []() -> std::string {
                           return "System is running normally.";
                       });

Dynamic Resources (Templates)
-----------------------------

Resource templates allow you to define a URI pattern with parameters. The server
will expand the URI when a client requests it:

.. code-block:: cpp

   server.add_resource_template("mcp://logs/{node}", "Node logs",
                                [](const std::map<std::string, std::string>& params) -> std::string {
                                    std::string node = params.at("node");
                                    return "Logs for node: " + node;
                                });

Resource Capabilities
---------------------

To allow clients to discover your resources, ensure you enable the resources
capability in your server configuration:

.. code-block:: cpp

   mcp::ServerCapabilities caps;
   caps.resources = mcp::ServerCapabilities::ResourcesCapability{};
   // Add other capabilities as needed...

   mcp::Server server(info, std::move(caps));

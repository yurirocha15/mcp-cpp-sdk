Extensions
==========

MCP extensions are optional additions to the protocol that a client and a
server negotiate through the ``extensions`` member of their capabilities, a map
from an extension identifier (``{vendor-prefix}/{extension-name}``) to that
extension's settings object. An empty object declares support with no settings.
An extension is off until the application turns it on: nothing in the SDK
declares one by itself.

This SDK supports the following official extensions, each specified in a
repository of the ``modelcontextprotocol`` organization on GitHub:

.. list-table::
   :header-rows: 1

   * - Extension
     - Identifier
     - Specification
     - Page
   * - MCP Apps
     - ``io.modelcontextprotocol/ui``
     - ``ext-apps``, revision 2026-01-26
     - :doc:`apps`
   * - Skills
     - ``io.modelcontextprotocol/skills``
     - ``ext-skills``, ``specification/beta``
     - :doc:`skills`
   * - OAuth client credentials
     - ``io.modelcontextprotocol/oauth-client-credentials``
     - ``ext-auth``, ``specification/stable`` (2026-07-28)
     - :ref:`oauth-client-credentials`
   * - Enterprise-managed authorization
     - ``io.modelcontextprotocol/enterprise-managed-authorization``
     - ``ext-auth``, ``specification/stable`` (2026-07-28)
     - :ref:`oauth-enterprise-managed-authorization`

If one side supports an extension and the other does not, the supporting side
falls back to the core protocol: a server offering MCP Apps still returns
ordinary tool results, and a server that requires an authorization extension
refuses the request.

Declaring an extension
----------------------

A client lists the extensions it supports in the capabilities it passes to
``Client::connect()``. On a 2026-07-28 session they travel with every request,
and a handler reads them from ``Context::request_meta()->clientCapabilities``:

.. code-block:: cpp

   #include <mcp/protocol/extensions.hpp>

   mcp::ClientCapabilities capabilities;
   mcp::declare_extension(capabilities, "com.example/widgets", {{"version", 2}});

   auto connected = co_await client.connect(
       mcp::Implementation{"my-client", "1.0.0"}, capabilities);

   // The server's declarations arrive in the connect result.
   bool server_has_widgets =
       mcp::declares_extension(connected.capabilities, "com.example/widgets");

A server declares extensions with ``Server::declare_extension()`` before it
serves its first request. They appear in the capabilities that
``server/discover`` and ``initialize`` return, and ``Server::capabilities()``
returns a copy of what is advertised:

.. code-block:: cpp

   server.declare_extension("com.example/widgets", {{"version", 2}});

``Server::declare_extension()`` and ``mcp::declare_extension()`` write the same
``extensions`` member, and ``mcp::declares_extension()`` and
``mcp::extension_settings()`` read it for either side. Each extension in this SDK
also has its own declaration helper, such as ``mcp::apps::declare_support()`` for
a host, which calls ``mcp::declare_extension()``.

Request methods of an extension
-------------------------------

An extension can add request methods. ``Server::add_extension_method()``
serves one to clients of protocol revision 2026-07-28:

.. code-block:: cpp

   server.add_extension_method(
       "com.example/echo",
       [](mcp::Context& ctx, const nlohmann::json& params) -> mcp::Task<nlohmann::json> {
           co_return nlohmann::json{{"echo", params.at("value")}};
       });

The handler gets the request's ``params``, including ``_meta``, and returns the
result object. The server adds ``resultType`` and the members every 2026-07-28
result carries; a result that sets ``ttlMs`` and ``cacheScope`` keeps them. A
handler reports invalid parameters by throwing ``std::invalid_argument``, which
the client receives as ``-32602``; any other exception becomes an internal
error. Middleware registered with ``server.use()`` wraps these handlers as it
wraps ``resources/read``, so an authentication middleware also guards them. A
name that the SDK serves itself, or that is registered already, is refused. Sessions of 2025-11-25 and
earlier answer an extension method with ``-32601``.

The client sends such a method with ``Client::send_request()``.
